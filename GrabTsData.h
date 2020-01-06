//------------------------------------------------------------------------------
// File: GrabTsData.h
//   Header file of GrabTsData
//------------------------------------------------------------------------------

#include <windows.h>
#include <atomic>
#define CEIL(a,b) (((a)+(b)-1)/(b))

// TS data buffer size
//
#define DATA_BUF_SIZE (188 * 256)
#define RING_BUF_SIZE (DATA_BUF_SIZE * 512) // > 24Mbps / 8bit * 5sec

// GrabTsData class
//
class GrabTsData
{
public:
	// Constructor
	GrabTsData(HANDLE *phOnStreamEvent)
	{
		m_phOnStreamEvent = phOnStreamEvent;
		std::atomic_init(&m_nAccumData, 0);
		std::atomic_init(&m_bPurge, FALSE);
		std::atomic_init(&m_nPush, 0);
		std::atomic_init(&m_nPull, 0);
		m_pDst = (BYTE *)malloc(DATA_BUF_SIZE);
		m_pBuf = (BYTE *)malloc(RING_BUF_SIZE);
	}
	// Destructor
	~GrabTsData()
	{
		if (m_pBuf) {
			free(m_pBuf);
		}
		if (m_pDst) {
			free(m_pDst);
		}
	}
	// Interfaces
	BOOL put_TsStream(BYTE *pSrc, DWORD dwSize);
	BOOL get_TsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain);
	BOOL purge_TsStream(void);
	BOOL get_ReadyCount(DWORD *pdwRemain);
	BOOL get_Bitrate(float *pfBitrate);

private:
	// Stream event
	HANDLE *m_phOnStreamEvent;
	// Bitrate calculation
	std::atomic_long m_nAccumData;
	// Purge flag
	std::atomic_bool m_bPurge;
	// TS data buffer (simple ring buffer)
	std::atomic_ulong m_nPush;
	std::atomic_ulong m_nPull;
	BYTE *m_pDst;
	BYTE *m_pBuf;
};
