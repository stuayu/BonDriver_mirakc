#include <process.h>
#include <windows.h>
#include <winhttp.h>
#include "IBonDriver2.h"
#include "GrabTsData.h"
#include "picojson\picojson.h"

#pragma comment(lib, "winhttp.lib")

#if !defined(_BONTUNER_H_)
#define _BONTUNER_H_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#define dllimport dllexport
#define TUNER_NAME "BonDriver_Mirakurun"

static const char g_TunerName[] = TUNER_NAME;
static wchar_t g_IniFilePath[MAX_PATH] = { '\0' };

#define MAX_HOST_LEN 256
static wchar_t g_ServerHost[MAX_HOST_LEN];
static uint32_t g_ServerPort;
static int g_DecodeB25;
static int g_Priority;
static int g_Service_Split;

#define SPACE_NUM 8
static char *g_pType[SPACE_NUM];
static int g_Max_Type = -1;
static DWORD g_Channel_Base[SPACE_NUM];
picojson::value g_Channel_JSON;

static int Init(HMODULE hModule);

class CBonTuner : public IBonDriver2
{
public:
	CBonTuner();
	virtual ~CBonTuner();

	// IBonDriver
	const BOOL OpenTuner(void);
	void CloseTuner(void);

	const BOOL SetChannel(const BYTE bCh);
	const float GetSignalLevel(void);

	const DWORD WaitTsStream(const DWORD dwTimeOut = 0);
	const DWORD GetReadyCount(void);

	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain);
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain);

	void PurgeTsStream(void);

	// IBonDriver2(暫定)
	LPCTSTR GetTunerName(void);

	const BOOL IsTunerOpening(void);

	LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);

	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);

	const DWORD GetCurSpace(void);
	const DWORD GetCurChannel(void);

	void Release(void);

	static CBonTuner *m_pThis;
	static HINSTANCE m_hModule;

protected:
	CRITICAL_SECTION m_CriticalSection;
	HANDLE m_hMutex;
	HANDLE m_hOnStreamEvent;
	HANDLE m_hStopEvent;
	HANDLE m_hRecvThread;

	GrabTsData *m_pGrabTsData;

	HINTERNET hSession;
	HINTERNET hConnect;
	HINTERNET hRequest;

	DWORD m_dwCurSpace;
	DWORD m_dwCurChannel;

	BOOL InitChannel(void);
	BOOL GetApiChannels(picojson::value *json_array, int service_split);
	BOOL SendRequest(wchar_t *url);
	static unsigned WINAPI RecvThread(LPVOID pParam);
};

#endif // !defined(_BONTUNER_H_)
