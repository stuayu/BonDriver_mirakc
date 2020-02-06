#include "BonDriver_Mirakurun.h"

//////////////////////////////////////////////////////////////////////
// DLLMain
//////////////////////////////////////////////////////////////////////

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD fdwReason, LPVOID lpReserved)
{
	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		if (Init(hModule) != 0) {
			return FALSE;
		}
		// モジュールハンドル保存
		CBonTuner::m_hModule = hModule;
		break;

	case DLL_PROCESS_DETACH:
		// 未開放の場合はインスタンス開放
		if (CBonTuner::m_pThis) {
			CBonTuner::m_pThis->Release();
		}
		break;
	}

	return TRUE;
}

static int Init(HMODULE hModule)
{
	GetModuleFileName(hModule, g_IniFilePath, MAX_PATH);

	wchar_t drive[_MAX_DRIVE];
	wchar_t dir[_MAX_DIR];
	wchar_t fname[_MAX_FNAME];
	_wsplitpath_s(g_IniFilePath,
		drive, _MAX_DRIVE, dir, _MAX_DIR, fname, _MAX_FNAME, NULL, NULL);
	wsprintf(g_IniFilePath, L"%s%s%s.ini\0", drive, dir, fname);

	HANDLE hFile = CreateFile(g_IniFilePath, GENERIC_READ,
		FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return -2;
	}
	CloseHandle(hFile);

	GetPrivateProfileStringW(L"GLOBAL", L"SERVER_HOST", L"localhost"
		, g_ServerHost, MAX_HOST_LEN, g_IniFilePath);
	g_ServerPort = GetPrivateProfileInt(
		L"GLOBAL", L"SERVER_PORT", 40772, g_IniFilePath);

	g_DecodeB25 = GetPrivateProfileInt(
		L"GLOBAL", L"DECODE_B25", 0, g_IniFilePath);
	g_Priority = GetPrivateProfileInt(
		L"GLOBAL", L"PRIORITY", 0, g_IniFilePath);
	g_Service_Split = GetPrivateProfileInt(
		L"GLOBAL", L"SERVICE_SPLIT", 0, g_IniFilePath);

	return 0;
}


//////////////////////////////////////////////////////////////////////
// インスタンス生成メソッド
//////////////////////////////////////////////////////////////////////

extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver()
{
	// スタンス生成(既存の場合はインスタンスのポインタを返す)
	return (CBonTuner::m_pThis)?
		CBonTuner::m_pThis : ((IBonDriver *) new CBonTuner);
}


//////////////////////////////////////////////////////////////////////
// 構築/消滅
//////////////////////////////////////////////////////////////////////

// 静的メンバ初期化
CBonTuner * CBonTuner::m_pThis = NULL;
HINSTANCE CBonTuner::m_hModule = NULL;

CBonTuner::CBonTuner()
	: m_hMutex(NULL)
	, m_hOnStreamEvent(NULL)
	, m_hStopEvent(NULL)
	, m_hRecvThread(NULL)
	, m_pGrabTsData(NULL)
	, hSession(NULL)
	, hConnect(NULL)
	, hRequest(NULL)
	, m_dwCurSpace(0xffffffff)
	, m_dwCurChannel(0xffffffff)
{
	m_pThis = this;

	::InitializeCriticalSection(&m_CriticalSection);

	// GrabTsDataインスタンス作成
	m_pGrabTsData = new GrabTsData(&m_hOnStreamEvent);
}

CBonTuner::~CBonTuner()
{
	// 開かれてる場合は閉じる
	CloseTuner();

	// GrabTsDataインスタンス開放
	if (m_pGrabTsData) {
		delete m_pGrabTsData;
	}

	::DeleteCriticalSection(&m_CriticalSection);

	m_pThis = NULL;
}

const BOOL CBonTuner::OpenTuner()
{
	while (1) {
		// ミューテックス作成
		m_hMutex = ::CreateMutexA(NULL, TRUE, g_TunerName);
		if (!m_hMutex) {
			break;
		}

		// イベントオブジェクト作成
		g_hCloseEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		m_hOnStreamEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		m_hStopEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		if (!g_hCloseEvent || !m_hOnStreamEvent || !m_hStopEvent) {
			break;
		}

		// WinHTTP初期化
		hSession = WinHttpOpen(
			TEXT(TUNER_NAME "/1.0"), WINHTTP_ACCESS_TYPE_NO_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!hSession) {
			char szDebugOut[64];
			sprintf_s(szDebugOut, "%s: WinHTTP not supported\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
			break;
		}

		// サーバー接続
		hConnect = WinHttpConnect(hSession, g_ServerHost, g_ServerPort, 0);
		if (!hConnect) {
			char szDebugOut[64];
			sprintf_s(szDebugOut, "%s: Connection failed\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
			break;
		}

		//Initialize channel
		if (!InitChannel()) {
			break;
		}

		// スレッド起動
		m_hRecvThread = (HANDLE)_beginthreadex(NULL, 0, CBonTuner::RecvThread,
			(LPVOID)this, 0, NULL);
		if (!m_hRecvThread) {
			break;
		}

		//return SetChannel(0UL,0UL);

		return TRUE;
	}

	CloseTuner();

	return FALSE;
}

void CBonTuner::CloseTuner()
{
	// チャンネル初期化
	m_dwCurSpace = 0xffffffff;
	m_dwCurChannel = 0xffffffff;

	// スレッド終了
	if (m_hRecvThread) {
		::SetEvent(m_hStopEvent);
		if (::WaitForSingleObject(m_hRecvThread, 10000) == WAIT_TIMEOUT) {
			// スレッド強制終了
			::TerminateThread(m_hRecvThread, 0xffffffff);

			char szDebugOut[64];
			sprintf_s(szDebugOut,
				"%s: CloseTuner() ::TerminateThread\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
		}
		::CloseHandle(m_hRecvThread);
		m_hRecvThread = NULL;
	}

	// チューニング空間解放
	for (int i = 0; i <= g_Max_Type; i++) {
		if (g_pType[i]) {
			free(g_pType[i]);
		}
	}
	g_Max_Type = -1;

	// WinHTTP開放
	if (hRequest) {
		WinHttpCloseHandle(hRequest);
		::WaitForSingleObject(g_hCloseEvent, 5000);
		hRequest = NULL;
	}
	if (hConnect) {
		WinHttpCloseHandle(hConnect);
		hConnect = NULL;
	}
	if (hSession) {
		WinHttpCloseHandle(hSession);
		hSession = NULL;
	}

	// イベントオブジェクト開放
	if (m_hStopEvent) {
		::CloseHandle(m_hStopEvent);
		m_hStopEvent = NULL;
	}
	if (m_hOnStreamEvent) {
		::CloseHandle(m_hOnStreamEvent);
		m_hOnStreamEvent = NULL;
	}
	if (g_hCloseEvent) {
		::CloseHandle(g_hCloseEvent);
		g_hCloseEvent = NULL;
	}

	// ミューテックス開放
	if (m_hMutex) {
		::ReleaseMutex(m_hMutex);
		::CloseHandle(m_hMutex);
		m_hMutex = NULL;
	}
}

const DWORD CBonTuner::WaitTsStream(const DWORD dwTimeOut)
{
	// 終了チェック
	if (!m_hOnStreamEvent) {
		return WAIT_ABANDONED;
	}

	// イベントがシグナル状態になるのを待つ
	const DWORD dwRet = ::WaitForSingleObject(m_hOnStreamEvent,
		(dwTimeOut) ? dwTimeOut : INFINITE);

	switch (dwRet) {
		case WAIT_ABANDONED :
			// チューナが閉じられた
			return WAIT_ABANDONED;

		case WAIT_OBJECT_0 :
		case WAIT_TIMEOUT :
			// ストリーム取得可能
			return dwRet;

		case WAIT_FAILED :
		default:
			// 例外
			return WAIT_FAILED;
	}
}

const DWORD CBonTuner::GetReadyCount()
{
	DWORD dwCount = 0;
	if (m_pGrabTsData) {
		m_pGrabTsData->get_ReadyCount(&dwCount);
	}

	return dwCount;
}

const BOOL CBonTuner::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *pSrc = NULL;

	// TSデータをバッファから取り出す
	if (GetTsStream(&pSrc, pdwSize, pdwRemain)) {
		if (*pdwSize) {
			::CopyMemory(pDst, pSrc, *pdwSize);
		}

		return TRUE;
	}

	return FALSE;
}

const BOOL CBonTuner::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_pGrabTsData || m_dwCurChannel == 0xffffffff) {
		return FALSE;
	}

	return m_pGrabTsData->get_TsStream(ppDst, pdwSize, pdwRemain);
}

void CBonTuner::PurgeTsStream()
{
	if (m_pGrabTsData) {
		m_pGrabTsData->purge_TsStream();
	}
}

void CBonTuner::Release()
{
	// インスタンス開放
	delete this;
}

LPCTSTR CBonTuner::GetTunerName(void)
{
	// チューナ名を返す
	return TEXT(TUNER_NAME);
}

const BOOL CBonTuner::IsTunerOpening(void)
{
	// チューナの使用中の有無を返す(全プロセスを通して)
	HANDLE hMutex = ::OpenMutexA(MUTEX_ALL_ACCESS, FALSE, g_TunerName);

	if (hMutex) {
		// 既にチューナは開かれている
		::CloseHandle(hMutex);
		return TRUE;
	}

	// チューナは開かれていない
	return FALSE;
}

LPCTSTR CBonTuner::EnumTuningSpace(const DWORD dwSpace)
{
	if ((int32_t)dwSpace > g_Max_Type) {
		return NULL;
	}

	// 使用可能なチューニング空間を返す
	const int len = 8;
	static TCHAR buf[len];
	::MultiByteToWideChar(CP_UTF8, 0, g_pType[dwSpace], -1, buf, len);

	return buf;
}

LPCTSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if ((int32_t)dwSpace > g_Max_Type) {
		return NULL;
	}
	if ((int32_t)dwSpace < g_Max_Type) {
		if (dwChannel >= g_Channel_Base[dwSpace + 1] - g_Channel_Base[dwSpace]) {
			return NULL;
		}
	}

	DWORD Bon_Channel = dwChannel + g_Channel_Base[dwSpace];
	if (!g_Channel_JSON.contains(Bon_Channel)) {
		return NULL;
	}

	picojson::object& channel_obj =
		g_Channel_JSON.get(Bon_Channel).get<picojson::object>();

	// 使用可能なチャンネル名を返す
	const int len = 128;
	static TCHAR buf[len];
	::MultiByteToWideChar(CP_UTF8, 0,
		channel_obj["name"].get<std::string>().c_str(), -1, buf, len);

	return buf;
}

const DWORD CBonTuner::GetCurSpace(void)
{
	// 現在のチューニング空間を返す
	return m_dwCurSpace;
}

const DWORD CBonTuner::GetCurChannel(void)
{
	// 現在のチャンネルを返す
	return m_dwCurChannel;
}

// チャンネル設定
const BOOL CBonTuner::SetChannel(const BYTE bCh)
{
	return SetChannel((DWORD)0,(DWORD)bCh - 13);
}

// チャンネル設定
const BOOL CBonTuner::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	if ((int32_t)dwSpace > g_Max_Type) {
		return FALSE;
	}

	DWORD Bon_Channel = dwChannel + g_Channel_Base[dwSpace];
	if (!g_Channel_JSON.contains(Bon_Channel)) {
		return FALSE;
	}

	picojson::object& channel_obj =
		g_Channel_JSON.get(Bon_Channel).get<picojson::object>();

	// Server request
	const int len = 64;
	wchar_t url[len];
	if (g_Service_Split == 1) {
		const int64_t id = (int64_t)channel_obj["id"].get<double>();
		swprintf_s(url, len,
			L"/api/services/%lld/stream?decode=%d", id, g_DecodeB25);
	}
	else {
		const char *type = channel_obj["type"].get<std::string>().c_str();
		const char *channel = channel_obj["channel"].get<std::string>().c_str();
		swprintf_s(url, len,
			L"/api/channels/%S/%S/stream?decode=%d", type, channel, g_DecodeB25);
	}
	if (!SendRequest(url)) {
		return FALSE;
	}

	// チャンネル情報更新
	m_dwCurSpace = dwSpace;
	m_dwCurChannel = dwChannel;

	// TSデータパージ
	PurgeTsStream();

	return TRUE;
}

// 信号レベル(ビットレート)取得
const float CBonTuner::GetSignalLevel(void)
{
	// チャンネル番号不明時は0を返す
	float fSignalLevel = 0;
	if (m_dwCurChannel != 0xffffffff && m_pGrabTsData)
		m_pGrabTsData->get_Bitrate(&fSignalLevel);
	return fSignalLevel;
}

BOOL CBonTuner::InitChannel()
{
	// Mirakurun APIよりchannel取得
	if (!GetApiChannels(&g_Channel_JSON, g_Service_Split)) {
		return FALSE;
	}
	if (g_Channel_JSON.is<picojson::null>()) {
		return FALSE;
	}
	if (!g_Channel_JSON.contains(0)) {
		return FALSE;
	}

	// チューニング空間取得
	int i = 0;
	int j = -1;
	while (j < SPACE_NUM - 1) {
		if (!g_Channel_JSON.contains(i)) {
			break;
		}
		picojson::object& channel_obj =
			g_Channel_JSON.get(i).get<picojson::object>();
		const char *type;
		if (g_Service_Split == 1) {
			picojson::object& channel_detail =
				channel_obj["channel"].get<picojson::object>();
			type = channel_detail["type"].get<std::string>().c_str();
		}
		else {
			type = channel_obj["type"].get<std::string>().c_str();
		}
		if (j < 0 || strcmp(g_pType[j], type)) {
			j++;
			int len = (int)strlen(type) + 1;
			g_pType[j] = (char *)malloc(len);
			if (!g_pType[j]) {
				j--;
				break;
			}
			strcpy_s(g_pType[j], len, type);
			g_Channel_Base[j] = i;
		}
		i++;
	}
	if (j < 0) {
		return FALSE;
	}
	g_Max_Type = j;

	return TRUE;
}

BOOL CBonTuner::GetApiChannels(picojson::value *channel_json, int service_split)
{
	const int len = 14;
	wchar_t url[len];

	wcscpy_s(url, len, L"/api/");
	if (service_split == 1) {
		wcscat_s(url, len, L"services");
	}
	else {
		wcscat_s(url, len, L"channels");
	}

	if (!SendRequest(url)) {
		return FALSE;
	}

	char *data = NULL;
	char *prev = NULL;
	DWORD dwSize;
	DWORD dwTotalSize = 0;
	BOOL ret;

	while(1) {
		ret = WinHttpQueryDataAvailable(hRequest, &dwSize);
		if (ret && dwSize > 0) {
			data = (char *)malloc((size_t)dwTotalSize + dwSize + 1);
			if (!data) {
				if (prev) {
					free(prev);
				}
				return FALSE;
			}
			if (prev) {
				::CopyMemory(data, prev, dwTotalSize);
				free(prev);
			}
			WinHttpReadData(hRequest, data + dwTotalSize, dwSize, NULL);
			prev = data;
			dwTotalSize += dwSize;
		}
		else {
			break;
		}
	}

	if (!data) {
		return FALSE;
	}

	*(data + dwTotalSize) = '\0';

	picojson::value v;
	std::string err = picojson::parse(v, data);
	if (!err.empty()) {
		return FALSE;
	}
	*channel_json = v;

	free(data);

	return TRUE;
}

BOOL CBonTuner::SendRequest(wchar_t *url)
{
	BOOL ret = FALSE;
	char szDebugOut[64];

	::EnterCriticalSection(&m_CriticalSection);

	int i = 0;
	while (i++ < 3) {
		if (i > 0) {
			::Sleep(500);
		}

		if (hRequest) {
			WinHttpCloseHandle(hRequest);
			::WaitForSingleObject(g_hCloseEvent, 5000);
		}

		hRequest = WinHttpOpenRequest(
			hConnect, L"GET", url, NULL, WINHTTP_NO_REFERER, NULL, 0);
		if (!hRequest) {
			sprintf_s(szDebugOut, "%s: OpenRequest failed\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
			continue;
		}

		if (WINHTTP_INVALID_STATUS_CALLBACK ==
			WinHttpSetStatusCallback(hRequest, InternetCallback,
				WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL)) {
			char szDebugOut[64];
			sprintf_s(szDebugOut,
				"%s: Callback function not set\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
			continue;
		}

		const int len = 64;
		wchar_t szHeader[len];
		swprintf_s(szHeader, len,
			L"Connection: close\r\nX-Mirakurun-Priority: %d", g_Priority);
		if (!WinHttpSendRequest(
			hRequest, szHeader, -1L, WINHTTP_NO_REQUEST_DATA, 0,
			WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, 0)) {
			sprintf_s(szDebugOut, "%s: SendRequest failed\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
			continue;
		}

		WinHttpReceiveResponse(hRequest, NULL);

		DWORD dwStatusCode = 0;
		DWORD dwSize = sizeof(dwStatusCode);
		WinHttpQueryHeaders(hRequest,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize,
			WINHTTP_NO_HEADER_INDEX);
		if (dwStatusCode != HTTP_STATUS_OK) {
			sprintf_s(szDebugOut, "%s: Invalid response\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
			continue;
		}

		ret = TRUE;
		break;
	}

	::LeaveCriticalSection(&m_CriticalSection);

	return ret;
}

void CALLBACK CBonTuner::InternetCallback(
	HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus,
	LPVOID lpvStatusInformation, DWORD dwStatusInformationLength)
{
	switch (dwInternetStatus)
	{
		case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
			SetEvent(g_hCloseEvent);
			break;
		case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
			char szDebugOut[64];
			sprintf_s(szDebugOut, "%s: Request error\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
	}
}

UINT WINAPI CBonTuner::RecvThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;
	BYTE *data;
	DWORD dwSize;
	BOOL ret;

	while (1) {
		if (::WaitForSingleObject(pThis->m_hStopEvent, 0) != WAIT_TIMEOUT) {
			//中止
			break;
		}

		::EnterCriticalSection(&pThis->m_CriticalSection);

		if (pThis->hRequest) {
			ret = WinHttpQueryDataAvailable(pThis->hRequest, &dwSize);
			if (ret && dwSize > 0) {
				data = (BYTE *)malloc(dwSize);
				if (data) {
					WinHttpReadData(pThis->hRequest, data, dwSize, NULL);
					pThis->m_pGrabTsData->put_TsStream(data, dwSize);
					free(data);
				}
			}
		}

		::LeaveCriticalSection(&pThis->m_CriticalSection);
	}

	return 0;
}
