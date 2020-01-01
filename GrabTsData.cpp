//------------------------------------------------------------------------------
// File: GrabTsData.cpp
//   Implementation of GrabTsData
//------------------------------------------------------------------------------

#include "GrabTsData.h" // declares myself

// Put TS data in the ring buffer
// (Transform function)
//
BOOL GrabTsData::put_TsStream(BYTE *pSrc, DWORD dwSize)
{
	if (dwSize < 1)
		return FALSE;

	// accumulate number of TS data received
	std::atomic_fetch_add(&m_nAccumData, dwSize);

	// lock push and pull positions
	DWORD nPush, nPull;
	nPush = std::atomic_load(&m_nPush);
	nPull = std::atomic_load(&m_nPull);
	// copy TS data to the ring buffer
	DWORD nTail = RING_BUF_SIZE - nPush; // size between the current position and the buffer end
	DWORD nData = (RING_BUF_SIZE + nPush - nPull) % RING_BUF_SIZE; // size of TS data stored
	nData = RING_BUF_SIZE - nData - 1; // size of available buffer (empty buffer - 1)
	nData = min(nData, dwSize);
	if (nData < nTail) {
		CopyMemory(m_pBuf + nPush, pSrc, nData);
		nPush += nData;
	}
	else {
		CopyMemory(m_pBuf + m_nPush, pSrc, nTail);
		CopyMemory(m_pBuf, pSrc + nTail, nData - nTail);
		nPush = nData - nTail;
	}
	// update the push position
	std::atomic_store(&m_nPush, nPush);

	// set WaitTsStream to the signaled state
	if (m_phOnStreamEvent)
		SetEvent(*m_phOnStreamEvent);

	return TRUE;
}

// Get TS data in the ring buffer
//
BOOL GrabTsData::get_TsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	// purge the pull buffer
	if (std::atomic_load(&m_bPurge)) {
		std::atomic_store(&m_nPull, std::atomic_load(&m_nPush));
		std::atomic_store(&m_nAccumData, 0); // reset bitrate
		std::atomic_store(&m_bPurge, FALSE);
		return FALSE;
	}

	// lock push and pull positions
	DWORD nPush, nPull;
	nPush = std::atomic_load(&m_nPush);
	nPull = std::atomic_load(&m_nPull);
	// copy TS data to the destination buffer
	DWORD nTail = RING_BUF_SIZE - nPull; // size between the current position and the buffer end
	DWORD nData = (RING_BUF_SIZE + nPush - nPull) % RING_BUF_SIZE; // size of TS data stored
	DWORD nRemain = nData;
	if (nData > 0) {
		nData = min(nData, DATA_BUF_SIZE);
		if (nData < nTail) {
			CopyMemory(m_pDst, m_pBuf + nPull, nData);
			nPull += nData;
		} else {
			CopyMemory(m_pDst, m_pBuf + nPull, nTail);
			CopyMemory(m_pDst + nTail, m_pBuf, nData - nTail);
			nPull = nData - nTail;
		}
		nRemain -= nData;
		// update the pull position
		std::atomic_store(&m_nPull, nPull);
	}

	// set destination variables
	*ppDst = m_pDst;
	*pdwSize = nData;
	if (pdwRemain) {
		*pdwRemain = CEIL(nRemain, DATA_BUF_SIZE);
	}

	return TRUE;
}

// Purge TS data
//
BOOL GrabTsData::purge_TsStream(void)
{
	std::atomic_store(&m_bPurge, TRUE);

	return TRUE;
}

// Get the number of TS data blocks in the ring buffer
//
BOOL GrabTsData::get_ReadyCount(DWORD *pdwRemain)
{
	if (pdwRemain) {
		DWORD nPush, nPull;
		nPush = std::atomic_load(&m_nPush);
		nPull = std::atomic_load(&m_nPull);
		*pdwRemain = CEIL((RING_BUF_SIZE + nPush - nPull) % RING_BUF_SIZE, DATA_BUF_SIZE);
	}

	return TRUE;
}

// Calculate bitrate
//
BOOL GrabTsData::get_Bitrate(float *pfBitrate)
{
	static double dBitrate = 0;
	static ULONGLONG dwLastTime = GetTickCount64();
	ULONGLONG dwNow = GetTickCount64(); // ms
	ULONGLONG dwDuration = dwNow - dwLastTime;

	if (dwDuration >= 1000) {
		dBitrate = std::atomic_exchange(&m_nAccumData, 0) / dwDuration * 8 * 1000 / 1024 / 1024.0; // Mbps
		dwLastTime = dwNow;
	}
	*pfBitrate = (float)min(dBitrate, 100);

	return TRUE;
}
