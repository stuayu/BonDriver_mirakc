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

	HANDLE hFile = CreateFile(g_IniFilePath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return -2;
	}
	CloseHandle(hFile);

	setlocale(LC_ALL, "ja-JP");

	size_t ret;
	wchar_t tmpServerHost[MAX_HOST_LEN];
	GetPrivateProfileStringW(L"GLOBAL", L"SERVER_HOST", L"localhost", tmpServerHost,
		MAX_HOST_LEN, g_IniFilePath);
	wcstombs_s(&ret, g_ServerHost, MAX_HOST_LEN, tmpServerHost, _TRUNCATE);

	wchar_t tmpServerPort[MAX_PORT_LEN];
	GetPrivateProfileStringW(L"GLOBAL", L"SERVER_PORT", L"8888", tmpServerPort,
		MAX_PORT_LEN, g_IniFilePath);
	wcstombs_s(&ret, g_ServerPort, MAX_PORT_LEN, tmpServerPort, _TRUNCATE);

	g_DecodeB25 = GetPrivateProfileInt(L"GLOBAL", L"DECODE_B25", 0, g_IniFilePath);
	g_Priority = GetPrivateProfileInt(L"GLOBAL", L"PRIORITY", 0, g_IniFilePath);
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
	return (CBonTuner::m_pThis)? CBonTuner::m_pThis : ((IBonDriver *) new CBonTuner);
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
	, m_pSrc(NULL)
	, m_bWinsock(FALSE)
	, m_res(NULL)
	, m_sock(INVALID_SOCKET)
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
	// ミューテックス作成
	m_hMutex = ::CreateMutexA(NULL, TRUE, g_TunerName);
	if (!m_hMutex) {
		return FALSE;
	}

	while (1) {
		// TSバッファー確保
		m_pSrc = (BYTE *)malloc(DATA_BUF_SIZE);
		if (!m_pSrc) {
			break;
		}

		// Winsock初期化
		WSADATA stWsa;
		if (WSAStartup(MAKEWORD(2, 2), &stWsa) != 0) {
			break;
		}
		m_bWinsock = TRUE;

		// ホスト名解決
		struct addrinfo hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_NUMERICSERV;
		if (getaddrinfo(g_ServerHost, g_ServerPort, &hints, &m_res) < 0) {
			break;
		}

		//Initialize channel
		setlocale(LC_ALL, ".utf-8");
		if (!InitChannel()) {
			break;
		}

		// イベントオブジェクト作成
		m_hOnStreamEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
		m_hStopEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);

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

			char szDebugOut[128];
			sprintf_s(szDebugOut,
				"%s: CloseTuner() ::TerminateThread\n", g_TunerName);
			::OutputDebugStringA(szDebugOut);
		}
		::CloseHandle(m_hRecvThread);
		m_hRecvThread = NULL;
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

	// チューニング空間解放
	for (int i = 0; i <= g_Max_Type; i++) {
		if (g_pType[i]) {
			free(g_pType[i]);
		}
	}
	g_Max_Type = -1;

	// ソケットクローズ
	if (m_sock != INVALID_SOCKET) {
		closesocket(m_sock);
		m_sock = INVALID_SOCKET;
	}

	// アドレスリソース開放
	if (m_res) {
		freeaddrinfo(m_res);
		m_res = NULL;
	}

	// Winsock終了
	if (m_bWinsock) {
		WSACleanup();
		m_bWinsock = FALSE;
	}

	// TSバッファー開放
	if (m_pSrc) {
		free(m_pSrc);
		m_pSrc = NULL;
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
	if (dwSpace > (UINT)g_Max_Type) {
		return NULL;
	}

	// 使用可能なチューニング空間を返す
	size_t ret;
	const int len = 8;
	static TCHAR buf[len];
	mbstowcs_s(&ret, buf, len, g_pType[dwSpace], _TRUNCATE);

	return buf;
}

LPCTSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (dwSpace > (UINT)g_Max_Type) {
		return NULL;
	}
	if (dwSpace < (UINT)g_Max_Type) {
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
	size_t ret;
	const int len = 128;
	static TCHAR buf[len];
	mbstowcs_s(&ret, buf, len,
		channel_obj["name"].get<std::string>().c_str(), _TRUNCATE);

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
	if (dwSpace > (UINT)g_Max_Type) {
		return FALSE;
	}

	DWORD Bon_Channel = dwChannel + g_Channel_Base[dwSpace];
	if (!g_Channel_JSON.contains(Bon_Channel)) {
		return FALSE;
	}

	picojson::object& channel_obj =
		g_Channel_JSON.get(Bon_Channel).get<picojson::object>();

	// Server request
	char url[128];
	if (g_Service_Split == 1) {
		const INT64 id = (INT64)channel_obj["id"].get<double>();
		sprintf_s(url, "/api/services/%lld/stream?decode=%d", id, g_DecodeB25);
	}
	else {
		const char *type = channel_obj["type"].get<std::string>().c_str();
		const char *channel = channel_obj["channel"].get<std::string>().c_str();
		sprintf_s(url, "/api/channels/%s/%s/stream?decode=%d", type, channel, g_DecodeB25);
	}
	if (!sendURL(url)) {
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
	const int len = 1024 * 64;
	char *buf;

	buf = (char *)malloc(len);
	if (!buf) {
		return FALSE;
	}

	strcpy_s(buf, len, "/api/");
	if (service_split == 1) {
		strcat_s(buf, len, "services");
	}
	else {
		strcat_s(buf, len, "channels");
	}

	while (1) {
		if (!sendURL(buf)) {
			break;
		}

		int ret;
		ret = recv(m_sock, buf, len - 1, 0);
		closesocket(m_sock);
		if (ret < 1) {
			break;
		}
		*(buf + ret) = '\0';

		char *p = strstr(buf, "[{");
		if (!p) {
			break;
		}

		picojson::value v;
		std::string err = picojson::parse(v, p);
		if (!err.empty()) {
			break;
		}
		*channel_json = v;

		free(buf);

		return TRUE;
	}

	free(buf);

	return FALSE;
}

BOOL CBonTuner::sendURL(char *url)
{
	BOOL ret = TRUE;
	struct addrinfo *ai;

	EnterCriticalSection(&m_CriticalSection);

	// Close opened socket
	if (m_sock != INVALID_SOCKET) {
		closesocket(m_sock);
		m_sock = INVALID_SOCKET;
	}

	for (ai = m_res; ai; ai = ai->ai_next) {
		m_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (m_sock == INVALID_SOCKET) {
			continue;
		}

		if (connect(m_sock, ai->ai_addr, (int)ai->ai_addrlen) >= 0) {
			// OK
			break;
		}
		closesocket(m_sock);
		m_sock = INVALID_SOCKET;
	}

	if (m_sock == INVALID_SOCKET) {
		char szDebugOut[128];
		sprintf_s(szDebugOut,
			"%s: connection error %d\n", g_TunerName, WSAGetLastError());
		::OutputDebugStringA(szDebugOut);
		ret = FALSE;
	}
	else {
		char serverRequest[256];
		sprintf_s(serverRequest,
			"GET %s HTTP/1.1\r\nX-Mirakurun-Priority: %d\r\n\r\n", url, g_Priority);
		if (send(m_sock, serverRequest, (int)strlen(serverRequest), 0) < 0) {
			closesocket(m_sock);
			char szDebugOut[128];
			sprintf_s(szDebugOut,
				"%s: send error %d\n", g_TunerName, WSAGetLastError());
			::OutputDebugStringA(szDebugOut);
			ret = FALSE;
		}
	}

	LeaveCriticalSection(&m_CriticalSection);

	return ret;
}

UINT WINAPI CBonTuner::RecvThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;
	int len = 0;

	if (!pThis->m_pSrc) {
		return 0;
	}

	while (1) {
		if (::WaitForSingleObject(pThis->m_hStopEvent, 0) != WAIT_TIMEOUT) {
			//中止
			break;
		}
		::EnterCriticalSection(&pThis->m_CriticalSection);
		if (pThis->m_sock != INVALID_SOCKET) {
			len = recv(pThis->m_sock, (char *)pThis->m_pSrc, DATA_BUF_SIZE, 0);
		}
		::LeaveCriticalSection(&pThis->m_CriticalSection);
		if (len > 0) {
			pThis->m_pGrabTsData->put_TsStream(pThis->m_pSrc, len);
			len = 0;
		}
	}

	return 0;
}
