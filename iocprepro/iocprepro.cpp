// iocprepro.cpp - a standalone specimen for eMule's I/O completion port usage.
//
// WHY THIS EXISTS
// ---------------
// The same question that wsrepro/ answered for Winsock, asked of the next
// Win32 subsystem eMule leans on: completion ports. Two of eMule's threads are
// built entirely around one - srchybrid/UploadDiskIOThread.cpp reads the file
// blocks it uploads, srchybrid/PartFileWriteThread.cpp writes the ones it
// downloads - and both use the same loop:
//
//     while (m_Run && GetQueuedCompletionStatus(port, .., INFINITE) && key) {
//         ...start new overlapped I/O...
//         do { ...take one completion... }
//         while (GetQueuedCompletionStatus(port, .., 0));   // drain, no wait
//         if (InterlockedExchange8(&m_bNewData, 0) && m_listPendingIO.IsEmpty())
//             PostQueuedCompletionStatus(port, 0, WAKEUP, NULL);
//     }
//
// That is the same shape that turned out to be fatal on the listening socket:
// a thread parked in an unbounded wait, woken only by a notification that
// something else must deliver, with a drain loop that runs until the source
// says "nothing left". If one packet is ever lost - a completion, or one of
// the hand-posted wakeups - the thread parks forever and the transfers it
// serves stop, exactly like the web interface did.
//
// This program is that loop with nothing else around it, and a ledger: every
// I/O issued is checked off when its completion is taken, so a lost, duplicated
// or mis-addressed packet is counted rather than inferred.
//
// WHAT IT MEASURES
// ---------------
//   --mode contract   a fixed table of deterministic probes of the API's
//                     contract (what GetQueuedCompletionStatus writes into its
//                     out-parameters when it times out, whether a failing
//                     ReadFile queues a packet, what CloseHandle does to
//                     pending I/O, ...). Each line starts with "PROBE " and is
//                     meant to be DIFFED between Windows and Wine: no
//                     timestamps, no pointers, no run-dependent numbers. The
//                     oracle is Windows; a difference is the finding.
//   --mode strict     stress with exact accounting: every wakeup is posted and
//                     counted, every completion is checked off. posted==taken
//                     and issued==completed must hold. A shortfall here is the
//                     platform, with no room for interpretation.
//   --mode emule      eMule's loop reproduced faithfully, including the
//                     unsynchronized WakeUpCall() merge. This one can stall on
//                     a correct platform too, because that merge has a race of
//                     its own - which is precisely why it is a separate mode:
//                     a stall on BOTH platforms is eMule's defect, a stall on
//                     one of them is the platform's.
//   --mode bigoff     64-bit file offsets: eMule writes them as
//                     *(uint64*)&oOverlap.Offset and its part files go past
//                     4 GB. Reads and writes straddling that boundary, with
//                     the content verified byte for byte.
//
// Built and run by .github/workflows/build-iocprepro.yml on the Windows runner
// and, with the very same x64 binary, under Wine on the Linux runner.

#include <afxwin.h>
#include <afxtempl.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>

// MFC console application: AfxWinInit needs an application object to exist,
// and CWinThread needs the module state that object sets up. eMule's two I/O
// threads are CWinThread-derived and started with AfxBeginThread, so the
// specimen must live in the same world.
CWinApp theApp;

// eMule's own names, copied verbatim from UploadDiskIOThread.cpp
#define RUN_STOP    0
#define RUN_IDLE    1
#define RUN_WORK    2
#define WAKEUP      ((ULONG_PTR)(~0))

/////////////////////////////////////////////////////////////////////////////
// Options

enum EMode
{
	MODE_CONTRACT = 0,
	MODE_STRICT,
	MODE_EMULE,
	MODE_BIGOFF
};

static const char *const s_pszModeNames[] = { "contract", "strict", "emule", "bigoff" };

struct SOptions
{
	int nMode;
	int nPending;     // how many overlapped reads may be in flight at once
	int nProducers;   // threads asking for blocks, as the upload queue does
	int nDelayMs;     // pause between requests, per producer
	int nBlockKB;     // size of one read
	int nFileMB;      // size of the test file
	int nStallMs;     // no completion for this long, with work outstanding -> stall
	int nDurationS;
	int nBigGB;       // bigoff mode: how far out the sparse file goes
};

static SOptions s_opt = { MODE_CONTRACT, 32, 4, 0, 64, 64, 5000, 60, 5 };

/////////////////////////////////////////////////////////////////////////////
// Output
//
// Two channels on purpose. Log() is for humans reading one run; Probe() is for
// the machine comparing two runs, so it carries nothing that can differ
// between two correct runs on the same platform.

static CRITICAL_SECTION s_csLog;

static void Log(const char *pszFmt, ...)
{
	::EnterCriticalSection(&s_csLog);
	printf("[%8u] ", ::GetTickCount());
	va_list args;
	va_start(args, pszFmt);
	vprintf(pszFmt, args);
	va_end(args);
	printf("\n");
	fflush(stdout);
	::LeaveCriticalSection(&s_csLog);
}

static void Probe(const char *pszName, const char *pszFmt, ...)
{
	::EnterCriticalSection(&s_csLog);
	printf("PROBE %-20s ", pszName);
	va_list args;
	va_start(args, pszFmt);
	vprintf(pszFmt, args);
	va_end(args);
	printf("\n");
	fflush(stdout);
	::LeaveCriticalSection(&s_csLog);
}

/////////////////////////////////////////////////////////////////////////////
// The test file and its content
//
// A byte's value is a function of its offset alone, so a block read from
// anywhere can be verified without keeping a copy of what was written - and a
// read that lands at the wrong offset is caught, not just a corrupted one.

static inline BYTE PatternByte(ULONGLONG uOff)
{
	ULONGLONG x = uOff + 0x9E3779B97F4A7C15ull;
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	return (BYTE)(x >> 33);
}

static void FillPattern(BYTE *pBuf, ULONGLONG uOff, DWORD dwLen)
{
	for (DWORD i = 0; i < dwLen; ++i)
		pBuf[i] = PatternByte(uOff + i);
}

// -1 when the block is what it should be, else the offset of the first byte
// that is not.
static LONG CheckPattern(const BYTE *pBuf, ULONGLONG uOff, DWORD dwLen)
{
	for (DWORD i = 0; i < dwLen; ++i)
		if (pBuf[i] != PatternByte(uOff + i))
			return (LONG)i;
	return -1;
}

struct TestFile
{
	HANDLE    hFile;
	ULONGLONG uSize;
	TCHAR     szPath[MAX_PATH];
};

static bool MakeTempPath(TCHAR *pszOut)
{
	TCHAR szDir[MAX_PATH];
	if (!::GetTempPath(MAX_PATH, szDir))
		return false;
	return ::GetTempFileName(szDir, _T("iocp"), 0, pszOut) != 0;
}

// Writes the file the ordinary way; the overlapped handle is opened afterwards,
// as eMule does in AssociateFile().
static bool CreateTestFile(TestFile &rFile, int nMB)
{
	if (!MakeTempPath(rFile.szPath))
		return false;

	HANDLE h = ::CreateFile(rFile.szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	const DWORD dwChunk = 1024 * 1024;
	BYTE *pBuf = new BYTE[dwChunk];
	bool bOk = true;
	for (int i = 0; bOk && i < nMB; ++i) {
		FillPattern(pBuf, (ULONGLONG)i * dwChunk, dwChunk);
		DWORD dwWritten = 0;
		bOk = ::WriteFile(h, pBuf, dwChunk, &dwWritten, NULL) && dwWritten == dwChunk;
	}
	delete[] pBuf;
	::CloseHandle(h);
	if (!bOk)
		return false;

	rFile.uSize = (ULONGLONG)nMB * dwChunk;
	// exactly eMule's flags in CUploadDiskIOThread::AssociateFile
	rFile.hFile = ::CreateFile(rFile.szPath, GENERIC_READ
		, FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE
		, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	return rFile.hFile != INVALID_HANDLE_VALUE;
}

static void DeleteTestFile(TestFile &rFile)
{
	if (rFile.hFile && rFile.hFile != INVALID_HANDLE_VALUE)
		::CloseHandle(rFile.hFile);
	rFile.hFile = INVALID_HANDLE_VALUE;
	if (rFile.szPath[0])
		::DeleteFile(rFile.szPath);
	rFile.szPath[0] = 0;
}

/////////////////////////////////////////////////////////////////////////////
// The overlapped request
//
// Same layout as eMule's OverlappedRead_Struct: an OVERLAPPED first, so the
// address of the structure is the address of the OVERLAPPED and the pointer
// that comes back out of the port is the request itself. nSlot and nState are
// not eMule's - they are the ledger.

struct OverlappedIo_Struct
{
	OVERLAPPED oOverlap;      // must be the first member
	TestFile  *pFile;
	ULONGLONG  uStartOffset;
	DWORD      dwLength;
	BYTE      *pBuffer;
	int        nSlot;
	LONG       nState;        // 0 free, 1 issued, 2 completed
};

/////////////////////////////////////////////////////////////////////////////
// Mode "contract": deterministic probes
//
// Every one of these answers a question eMule's code already assumes the
// answer to. They are cheap, they do not depend on timing, and they are
// printed in a form that can be compared line by line between two platforms.

static int DrainPort(HANDLE hPort)
{
	int n = 0;
	for (;;) {
		DWORD dwBytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED pOv = NULL;
		if (!::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 0) && pOv == NULL)
			break;
		++n;
		if (n > 4096)
			break;
	}
	return n;
}

static const char *DescribeDword(DWORD dw, DWORD dwPoison)
{
	return dw == dwPoison ? "untouched" : (dw == 0 ? "zeroed" : "other");
}

// Issues one overlapped read and reports what the API did with it, at both
// ends: what ReadFile returned, and what came out of the port.
static void ProbeOneRead(HANDLE hPort, TestFile &rFile, const char *pszName, ULONGLONG uOff, DWORD dwLen, bool bCheckData)
{
	OverlappedIo_Struct stIo;
	::ZeroMemory(&stIo, sizeof stIo);
	stIo.pFile = &rFile;
	stIo.uStartOffset = uOff;
	stIo.dwLength = dwLen;
	stIo.pBuffer = new BYTE[dwLen ? dwLen : 1];
	::memset(stIo.pBuffer, 0xCC, dwLen ? dwLen : 1);
	// eMule writes the 64-bit offset straight over the two DWORDs, so the
	// specimen does too rather than using a portable-looking equivalent.
	*(ULONGLONG*)&stIo.oOverlap.Offset = uOff;

	::SetLastError(0);
	const BOOL bRet = ::ReadFile(rFile.hFile, stIo.pBuffer, dwLen, NULL, (LPOVERLAPPED)&stIo);
	const DWORD dwErr = ::GetLastError();

	// Wait briefly for a packet. Which packet: the one belonging to THIS
	// request, so a stray one cannot be mistaken for it.
	DWORD dwBytes = 0;
	ULONG_PTR key = 0;
	LPOVERLAPPED pOv = NULL;
	const BOOL bGot = ::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 1000);
	const DWORD dwGqcsErr = ::GetLastError();

	const bool bMine = (pOv == (LPOVERLAPPED)&stIo);
	const char *pszData = "n/a";
	if (bCheckData && bMine && dwBytes > 0)
		pszData = (CheckPattern(stIo.pBuffer, uOff, dwBytes) < 0) ? "ok" : "WRONG";

	// Whether the read completed synchronously or went pending is NOT part of
	// the probe line: it depends on the file cache and can differ between two
	// runs on the same machine, which would make the comparison noisy. Both
	// mean the same thing here - the request was accepted - and a packet is
	// expected either way. Only a synchronous REFUSAL is a different answer,
	// and that one is reported with its error code.
	char szAccepted[32];
	if (bRet || dwErr == ERROR_IO_PENDING)
		strcpy(szAccepted, "accepted");
	else
		sprintf(szAccepted, "refused:%u", dwErr);

	Probe(pszName, "read=%s packet=%s gqcs=%s gqcserr=%u key=%s bytes=%s data=%s"
		, szAccepted
		, pOv == NULL ? "none" : (bMine ? "mine" : "other")
		, bGot ? "TRUE" : "FALSE", bGot ? 0 : dwGqcsErr
		, bMine ? (key == (ULONG_PTR)&rFile ? "ok" : "WRONG") : "n/a"
		, bMine ? (dwBytes == dwLen ? "full" : (dwBytes == 0 ? "zero" : "partial")) : "n/a"
		, pszData);

	Log("note: %s returned %s synchronously", pszName, bRet ? "TRUE" : "FALSE");
	if (!bMine && pOv != NULL)
		Log("note: %s took a packet that was not its own", pszName);

	delete[] stIo.pBuffer;
	DrainPort(hPort);
}

static int RunContract()
{
	TestFile stFile;
	::ZeroMemory(&stFile, sizeof stFile);
	if (!CreateTestFile(stFile, 4)) {
		Log("could not create the test file, err=%u", ::GetLastError());
		return 2;
	}

	// 1. The port itself, created the way eMule creates it.
	HANDLE hPort = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
	Probe("port-create", "ok=%d", hPort != NULL);
	if (!hPort) {
		DeleteTestFile(stFile);
		return 2;
	}

	// 2. Association. eMule tests the return value for EQUALITY with the port
	//    it passed in (m_hPort != CreateIoCompletionPort(...)), not for NULL.
	HANDLE hAssoc = ::CreateIoCompletionPort(stFile.hFile, hPort, (ULONG_PTR)&stFile, 0);
	Probe("assoc", "same-handle=%d", hAssoc == hPort);

	// 3. Associating the same handle twice - what eMule's AssociateFile would
	//    hit if its m_hRead bookkeeping ever slipped.
	::SetLastError(0);
	HANDLE hAgain = ::CreateIoCompletionPort(stFile.hFile, hPort, (ULONG_PTR)&stFile, 0);
	Probe("assoc-twice", "ret=%s err=%u", hAgain == NULL ? "NULL" : (hAgain == hPort ? "port" : "other"), hAgain ? 0 : ::GetLastError());

	// 4. The empty port with no wait. This is the call that ENDS eMule's inner
	//    drain loop, and the loop then reads completionKey to decide whether it
	//    was asked to terminate - so what this call leaves in the
	//    out-parameters is not academic.
	{
		DWORD dwBytes = 0xDEADBEEF;
		ULONG_PTR key = (ULONG_PTR)0xDEADBEEF;
		LPOVERLAPPED pOv = (LPOVERLAPPED)0xDEADBEEF;
		::SetLastError(0);
		const BOOL b = ::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 0);
		Probe("gqcs-empty", "ret=%s err=%u bytes=%s key=%s ov=%s"
			, b ? "TRUE" : "FALSE", ::GetLastError()
			, DescribeDword(dwBytes, 0xDEADBEEF)
			, key == (ULONG_PTR)0xDEADBEEF ? "untouched" : (key == 0 ? "zeroed" : "other")
			, pOv == (LPOVERLAPPED)0xDEADBEEF ? "untouched" : (pOv == NULL ? "NULL" : "other"));
	}

	// 5. The empty port with a bounded wait: does it wait, and for how long.
	{
		const DWORD dwT0 = ::GetTickCount();
		DWORD dwBytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED pOv = NULL;
		const BOOL b = ::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 200);
		const DWORD dwElapsed = ::GetTickCount() - dwT0;
		Probe("gqcs-timeout", "ret=%s err=%u waited=%s"
			, b ? "TRUE" : "FALSE", ::GetLastError()
			, dwElapsed < 150 ? "TOO-SHORT" : (dwElapsed <= 1000 ? "as-asked" : "TOO-LONG"));
	}

	// 6. A hand-posted packet, delivered verbatim. eMule's wakeups are these,
	//    and its termination is one of these with key 0.
	{
		::PostQueuedCompletionStatus(hPort, 0x1234, WAKEUP, (LPOVERLAPPED)0x5678);
		DWORD dwBytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED pOv = NULL;
		const BOOL b = ::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 1000);
		Probe("post-roundtrip", "ret=%s bytes=%d key=%d ov=%d"
			, b ? "TRUE" : "FALSE", dwBytes == 0x1234, key == WAKEUP, pOv == (LPOVERLAPPED)0x5678);
	}

	// 7. The termination packet: key 0, no overlapped. eMule's loop condition
	//    is "&& completionKey", so this must arrive AS key 0 and must not be
	//    reported as a failure.
	{
		::PostQueuedCompletionStatus(hPort, 0, 0, NULL);
		DWORD dwBytes = 0xDEADBEEF;
		ULONG_PTR key = (ULONG_PTR)0xDEADBEEF;
		LPOVERLAPPED pOv = (LPOVERLAPPED)0xDEADBEEF;
		const BOOL b = ::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 1000);
		Probe("post-key0", "ret=%s key=%s ov=%s"
			, b ? "TRUE" : "FALSE"
			, key == 0 ? "zero" : "other"
			, pOv == NULL ? "NULL" : "other");
	}

	// 8. Order of delivery. Not something eMule depends on, but a queue that
	//    reorders would be worth knowing about before anything else is blamed.
	{
		for (ULONG_PTR i = 1; i <= 8; ++i)
			::PostQueuedCompletionStatus(hPort, (DWORD)i, i, NULL);
		char szOrder[64];
		int nPos = 0;
		for (int i = 0; i < 8; ++i) {
			DWORD dwBytes = 0;
			ULONG_PTR key = 0;
			LPOVERLAPPED pOv = NULL;
			if (!::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 1000))
				break;
			nPos += sprintf(szOrder + nPos, "%d", (int)key);
		}
		szOrder[nPos] = 0;
		Probe("post-order", "seq=%s", szOrder);
	}
	DrainPort(hPort);

	// 9. The ordinary read, and the three edges around the end of the file.
	//    eMule never reads past its own EOF on purpose, but it decides what to
	//    do with a failed read by whether ReadFile failed synchronously - and
	//    the answer to "does a synchronous failure still queue a packet"
	//    decides whether its pending list leaks an entry.
	ProbeOneRead(hPort, stFile, "read-plain", 0, 64 * 1024, true);
	ProbeOneRead(hPort, stFile, "read-offset", 1024 * 1024 + 4096, 64 * 1024, true);
	ProbeOneRead(hPort, stFile, "read-cross-eof", stFile.uSize - 4096, 64 * 1024, true);
	ProbeOneRead(hPort, stFile, "read-past-eof", stFile.uSize + 65536, 4096, false);
	ProbeOneRead(hPort, stFile, "read-zero", 0, 0, false);
	ProbeOneRead(hPort, stFile, "read-unaligned", 12345, 4321, true);

	// 10. CancelIo, and the loop EMSocket.cpp:1329 runs after it: does every
	//     cancelled request still produce a packet, and does the port stay
	//     consistent afterwards.
	{
		const int nIo = 8;
		OverlappedIo_Struct *pIo = new OverlappedIo_Struct[nIo];
		int nIssued = 0;
		for (int i = 0; i < nIo; ++i) {
			::ZeroMemory(&pIo[i], sizeof(OverlappedIo_Struct));
			pIo[i].dwLength = 256 * 1024;
			pIo[i].pBuffer = new BYTE[pIo[i].dwLength];
			*(ULONGLONG*)&pIo[i].oOverlap.Offset = (ULONGLONG)i * pIo[i].dwLength;
			if (::ReadFile(stFile.hFile, pIo[i].pBuffer, pIo[i].dwLength, NULL, (LPOVERLAPPED)&pIo[i])
				|| ::GetLastError() == ERROR_IO_PENDING)
				++nIssued;
		}
		::CancelIo(stFile.hFile);
		int nPackets = 0;
		const DWORD dwT0 = ::GetTickCount();
		while (nPackets < nIssued && ::GetTickCount() - dwT0 < 2000) {
			DWORD dwBytes = 0;
			ULONG_PTR key = 0;
			LPOVERLAPPED pOv = NULL;
			if (::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 100) || pOv != NULL)
				++nPackets;
		}
		Probe("cancel-io", "issued=%d packets=%s", nIssued, nPackets == nIssued ? "all" : "MISSING");
		if (nPackets != nIssued)
			Log("note: cancel-io saw %d packets for %d requests", nPackets, nIssued);
		for (int i = 0; i < nIo; ++i)
			delete[] pIo[i].pBuffer;
		delete[] pIo;
		DrainPort(hPort);
	}

	// 11. eMule's shutdown, word for word from PartFileWriteThread.cpp:
	//     "Improper termination of asynchronous I/O follows... close file
	//     handles to release I/O completion port". The requests are still in
	//     flight when the handle goes; whether their packets arrive afterwards
	//     decides whether that is merely untidy or a use-after-free.
	{
		TestFile stFile2;
		::ZeroMemory(&stFile2, sizeof stFile2);
		if (CreateTestFile(stFile2, 4) && ::CreateIoCompletionPort(stFile2.hFile, hPort, (ULONG_PTR)&stFile2, 0) == hPort) {
			const int nIo = 8;
			OverlappedIo_Struct *pIo = new OverlappedIo_Struct[nIo];
			int nIssued = 0;
			for (int i = 0; i < nIo; ++i) {
				::ZeroMemory(&pIo[i], sizeof(OverlappedIo_Struct));
				pIo[i].dwLength = 256 * 1024;
				pIo[i].pBuffer = new BYTE[pIo[i].dwLength];
				*(ULONGLONG*)&pIo[i].oOverlap.Offset = (ULONGLONG)i * pIo[i].dwLength;
				if (::ReadFile(stFile2.hFile, pIo[i].pBuffer, pIo[i].dwLength, NULL, (LPOVERLAPPED)&pIo[i])
					|| ::GetLastError() == ERROR_IO_PENDING)
					++nIssued;
			}
			::CloseHandle(stFile2.hFile);
			stFile2.hFile = INVALID_HANDLE_VALUE;
			int nPackets = 0;
			const DWORD dwT0 = ::GetTickCount();
			while (nPackets < nIssued && ::GetTickCount() - dwT0 < 2000) {
				DWORD dwBytes = 0;
				ULONG_PTR key = 0;
				LPOVERLAPPED pOv = NULL;
				if (::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 100) || pOv != NULL)
					++nPackets;
			}
			Probe("close-pending", "issued=%d packets=%s", nIssued, nPackets == nIssued ? "all" : (nPackets == 0 ? "none" : "some"));
			for (int i = 0; i < nIo; ++i)
				delete[] pIo[i].pBuffer;
			delete[] pIo;
		} else
			Probe("close-pending", "skipped");
		DeleteTestFile(stFile2);
		DrainPort(hPort);
	}

	// 12. And the port after the handles it was serving are gone: eMule closes
	//     the port right after that drain, from the same thread.
	Probe("port-close", "ok=%d", ::CloseHandle(hPort) != 0);

	DeleteTestFile(stFile);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Modes "strict" and "emule": the loop under load, with a ledger
//
// One I/O thread, exactly eMule's; a handful of producer threads standing in
// for the upload queue; and a monitor watching for the one symptom that
// matters - work outstanding and nothing happening.

static HANDLE   s_hPort = NULL;
static TestFile s_stFile;
static CWinThread *s_pIoThread = NULL;

static volatile LONG s_Run = RUN_STOP;
static volatile char s_bNewData = 0;

static CRITICAL_SECTION s_csTodo;
static CList<ULONGLONG, ULONGLONG> s_listTodo;

static OverlappedIo_Struct *s_pPool = NULL;
static volatile LONG s_nInFlight = 0;         // requests issued and not yet completed

static volatile LONG s_nIssued = 0;
static volatile LONG s_nCompleted = 0;
static volatile LONG s_nPosted = 0;           // wakeup packets handed to the port
static volatile LONG s_nWakeTaken = 0;        // wakeup packets taken off it
static volatile LONG s_nLoops = 0;            // outer wait cycles
static volatile LONG s_nDrained = 0;          // completions taken by the inner loop
static volatile LONG s_nOuter = 0;            // completions taken by the outer wait
static volatile LONG s_nRequested = 0;        // producer-side: requests enqueued
static volatile LONG s_bStopProducers = 0;    // not eMule's: m_Run is written by the I/O thread itself
static volatile LONG s_dwLastProgress = 0;

// Ledger findings. Any of these above zero is a result on its own, stall or no
// stall: they are things that cannot happen on a correct implementation.
static volatile LONG s_nBogus = 0;            // a packet for a request we do not have outstanding
static volatile LONG s_nKeyBad = 0;           // completion key is not the one we associated
static volatile LONG s_nShort = 0;            // fewer bytes than asked, inside the file
static volatile LONG s_nCorrupt = 0;          // the bytes are not the ones at that offset
static volatile LONG s_nOrphan = 0;           // eMule's own: a failed completion dropped by the drain loop
static volatile LONG s_nIoFailed = 0;         // ReadFile refused the request

static void CompletionRoutine(DWORD dwBytes, OverlappedIo_Struct *pIo, ULONG_PTR key)
{
	if (pIo < s_pPool || pIo >= s_pPool + s_opt.nPending || ::InterlockedCompareExchange(&pIo->nState, 2, 1) != 1) {
		::InterlockedIncrement(&s_nBogus);
		return;
	}
	if (key != (ULONG_PTR)&s_stFile)
		::InterlockedIncrement(&s_nKeyBad);
	if (dwBytes != pIo->dwLength)
		::InterlockedIncrement(&s_nShort);
	else if (CheckPattern(pIo->pBuffer, pIo->uStartOffset, dwBytes) >= 0)
		::InterlockedIncrement(&s_nCorrupt);

	::InterlockedIncrement(&s_nCompleted);
	::InterlockedExchange(&s_dwLastProgress, (LONG)::GetTickCount());
	::InterlockedDecrement(&s_nInFlight);
	::InterlockedExchange(&pIo->nState, 0);
}

// eMule's StartCreateNextBlockPackage, reduced to what it does with the port:
// take requests off the list and turn them into overlapped reads.
static void StartReads()
{
	for (;;) {
		int nSlot = -1;
		for (int i = 0; i < s_opt.nPending; ++i)
			if (s_pPool[i].nState == 0) {
				nSlot = i;
				break;
			}
		if (nSlot < 0)
			break;

		::EnterCriticalSection(&s_csTodo);
		const bool bHave = !s_listTodo.IsEmpty();
		const ULONGLONG uOff = bHave ? s_listTodo.RemoveHead() : 0;
		::LeaveCriticalSection(&s_csTodo);
		if (!bHave)
			break;

		OverlappedIo_Struct *pIo = &s_pPool[nSlot];
		::ZeroMemory(&pIo->oOverlap, sizeof(OVERLAPPED));
		*(ULONGLONG*)&pIo->oOverlap.Offset = uOff;
		pIo->pFile = &s_stFile;
		pIo->uStartOffset = uOff;
		pIo->dwLength = (DWORD)s_opt.nBlockKB * 1024;
		pIo->nState = 1;
		::InterlockedIncrement(&s_nInFlight);

		if (!::ReadFile(s_stFile.hFile, pIo->pBuffer, pIo->dwLength, NULL, (LPOVERLAPPED)pIo)) {
			const DWORD dwError = ::GetLastError();
			if (dwError != ERROR_IO_PENDING) {
				::InterlockedIncrement(&s_nIoFailed);
				::InterlockedDecrement(&s_nInFlight);
				::InterlockedExchange(&pIo->nState, 0);
				Log("ReadFile refused offset %I64u err=%u", uOff, dwError);
				break;
			}
		}
		::InterlockedIncrement(&s_nIssued);
		::InterlockedExchange(&s_dwLastProgress, (LONG)::GetTickCount());
	}
}

// The specimen. Structurally identical to CUploadDiskIOThread::RunInternal and
// CPartFileWriteThread::RunInternal - same order, same conditions, same two
// GetQueuedCompletionStatus calls.
static UINT AFX_CDECL IoThreadFunc(LPVOID)
{
	DWORD dwBytes = 0;
	ULONG_PTR completionKey = 0;
	OverlappedIo_Struct *pCurIO = NULL;
	s_Run = RUN_IDLE;

	while (s_Run
		&& ::GetQueuedCompletionStatus(s_hPort, &dwBytes, &completionKey, (LPOVERLAPPED*)&pCurIO, INFINITE)
		&& completionKey)
	{
		::InterlockedIncrement(&s_nLoops);
		if (completionKey == WAKEUP)
			::InterlockedIncrement(&s_nWakeTaken);
		else
			::InterlockedIncrement(&s_nOuter);

		s_Run = RUN_WORK;
		StartReads();
		::InterlockedExchange8(&s_bNewData, 0);

		//check completed I/O
		do {
			if (!completionKey)
				break;
			if (completionKey != WAKEUP) { //ignore wakeups
				::InterlockedIncrement(&s_nDrained);
				CompletionRoutine(dwBytes, pCurIO, completionKey);
			}
			pCurIO = NULL;
			if (!::GetQueuedCompletionStatus(s_hPort, &dwBytes, &completionKey, (LPOVERLAPPED*)&pCurIO, 0))
				break;
			if (completionKey == WAKEUP)
				::InterlockedIncrement(&s_nWakeTaken);
		} while (true);

		// eMule's own hazard, reproduced so that it is measured and not
		// assumed: the drain stops at the first packet it could not take, and
		// if that packet carried a failed I/O its request stays outstanding
		// forever.
		if (pCurIO != NULL)
			::InterlockedIncrement(&s_nOrphan);

		if (!completionKey) //thread termination
			break;
		s_Run = RUN_IDLE;
		if (::InterlockedExchange8(&s_bNewData, 0) && s_nInFlight == 0) {
			::InterlockedIncrement(&s_nPosted);
			::PostQueuedCompletionStatus(s_hPort, 0, WAKEUP, NULL);
		}
	}
	s_Run = RUN_STOP;
	return 0;
}

// The upload queue's side of the conversation: put work on the list and say so.
static UINT AFX_CDECL ProducerFunc(LPVOID)
{
	unsigned uSeed = ::GetCurrentThreadId();
	const ULONGLONG uBlocks = s_stFile.uSize / ((ULONGLONG)s_opt.nBlockKB * 1024);

	while (!s_bStopProducers) {
		uSeed = uSeed * 1103515245u + 12345u;
		const ULONGLONG uOff = (ULONGLONG)((uSeed >> 8) % (unsigned)uBlocks) * ((ULONGLONG)s_opt.nBlockKB * 1024);

		::EnterCriticalSection(&s_csTodo);
		const bool bRoom = s_listTodo.GetCount() < s_opt.nPending * 4;
		if (bRoom)
			s_listTodo.AddTail(uOff);
		::LeaveCriticalSection(&s_csTodo);

		if (bRoom) {
			::InterlockedIncrement(&s_nRequested);
			if (s_opt.nMode == MODE_STRICT) {
				// No merging, no flag, no reading of the thread's state: one
				// request, one packet, both counted. Whatever goes missing
				// here went missing in the port.
				::InterlockedIncrement(&s_nPosted);
				::PostQueuedCompletionStatus(s_hPort, 0, WAKEUP, NULL);
			} else {
				// CUploadDiskIOThread::WakeUpCall, verbatim - including the
				// two unsynchronized reads it makes of the I/O thread's state.
				if (s_Run == RUN_IDLE && s_nInFlight == 0) {
					::InterlockedIncrement(&s_nPosted);
					::PostQueuedCompletionStatus(s_hPort, 0, WAKEUP, NULL);
				} else
					::InterlockedExchange8(&s_bNewData, 1);
			}
		}
		if (s_opt.nDelayMs > 0)
			::Sleep(s_opt.nDelayMs);
		else if (!bRoom)
			::Sleep(1);
	}
	return 0;
}

// What state was the thread left in, and does one hand-posted packet undo it.
// The same question wsrepro asks of the listening thread, and the same three
// possible answers.
static void Autopsy()
{
	::EnterCriticalSection(&s_csTodo);
	const INT_PTR nTodo = s_listTodo.GetCount();
	::LeaveCriticalSection(&s_csTodo);
	const LONG nInFlight = s_nInFlight;
	const LONG nIssuedBefore = s_nIssued;
	const LONG nCompletedBefore = s_nCompleted;
	const char cNewData = s_bNewData;

	const bool bAlive = s_pIoThread && s_pIoThread->m_hThread
		&& ::WaitForSingleObject(s_pIoThread->m_hThread, 0) == WAIT_TIMEOUT;

	Log("autopsy: thread=%s run=%ld todo=%d inflight=%ld newdata=%d issued=%ld completed=%ld posted=%ld taken=%ld"
		, bAlive ? "alive" : "gone", s_Run, (int)nTodo, nInFlight, (int)cNewData
		, nIssuedBefore, nCompletedBefore, s_nPosted, s_nWakeTaken);

	if (!bAlive) {
		Log("VERDICT: the I/O thread exited - this is not the parked-thread shape");
		return;
	}

	::InterlockedIncrement(&s_nPosted);
	::PostQueuedCompletionStatus(s_hPort, 0, WAKEUP, NULL);
	::Sleep(1000);

	const bool bResumed = (s_nIssued > nIssuedBefore) || (s_nCompleted > nCompletedBefore);
	if (!bResumed) {
		Log("VERDICT: parked, and a hand-posted packet did not resume it - the thread is");
		Log("VERDICT: blocked somewhere other than GetQueuedCompletionStatus.");
		return;
	}

	if (nInFlight > 0 && s_nCompleted > nCompletedBefore) {
		Log("VERDICT: LOST COMPLETION. %ld requests were outstanding and their packets were", nInFlight);
		Log("VERDICT: never delivered to the waiting thread; one hand-posted packet made it");
		Log("VERDICT: find them. The port held completions that the INFINITE wait did not");
		Log("VERDICT: return for.");
		return;
	}

	if (s_opt.nMode == MODE_EMULE && cNewData && nInFlight == 0) {
		Log("VERDICT: eMule's own race, not the platform. WakeUpCall() found the thread");
		Log("VERDICT: busy and only set m_bNewData; the thread had already tested and");
		Log("VERDICT: cleared that flag, and parked. Nothing was lost in the port - the");
		Log("VERDICT: two unsynchronized accesses crossed. Expect this on Windows too.");
		return;
	}

	Log("VERDICT: LOST WAKEUP. The thread was parked with %d requests on the list and", (int)nTodo);
	Log("VERDICT: nothing outstanding; the wakeups posted for them were accepted by");
	Log("VERDICT: PostQueuedCompletionStatus and never came back out. One more packet,");
	Log("VERDICT: posted by hand, restarted it.");
}

static int RunStress()
{
	::ZeroMemory(&s_stFile, sizeof s_stFile);
	if (!CreateTestFile(s_stFile, s_opt.nFileMB)) {
		Log("could not create the test file, err=%u", ::GetLastError());
		return 2;
	}

	s_hPort = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
	if (!s_hPort) {
		Log("CreateIoCompletionPort failed err=%u", ::GetLastError());
		DeleteTestFile(s_stFile);
		return 2;
	}
	if (s_hPort != ::CreateIoCompletionPort(s_stFile.hFile, s_hPort, (ULONG_PTR)&s_stFile, 0)) {
		Log("association failed err=%u", ::GetLastError());
		DeleteTestFile(s_stFile);
		return 2;
	}

	s_pPool = new OverlappedIo_Struct[s_opt.nPending];
	for (int i = 0; i < s_opt.nPending; ++i) {
		::ZeroMemory(&s_pPool[i], sizeof(OverlappedIo_Struct));
		s_pPool[i].nSlot = i;
		s_pPool[i].pBuffer = new BYTE[(size_t)s_opt.nBlockKB * 1024];
	}

	// eMule starts these with AfxBeginThread from the constructor of a
	// CWinThread-derived class; suspended-then-resumed only so the handle
	// survives the thread for the autopsy.
	s_pIoThread = AfxBeginThread(IoThreadFunc, NULL, THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
	if (!s_pIoThread) {
		Log("AfxBeginThread failed");
		return 2;
	}
	s_pIoThread->m_bAutoDelete = FALSE;
	s_pIoThread->ResumeThread();

	for (int i = 0; i < 200 && s_Run == RUN_STOP; ++i)
		::Sleep(10);
	if (s_Run == RUN_STOP) {
		Log("the I/O thread did not come up");
		return 2;
	}

	::InterlockedExchange(&s_dwLastProgress, (LONG)::GetTickCount());
	for (int i = 0; i < s_opt.nProducers; ++i) {
		CWinThread *pT = AfxBeginThread(ProducerFunc, NULL);
		if (!pT)
			Log("producer %d did not start", i);
	}

	const DWORD dwStart = ::GetTickCount();
	DWORD dwLastReport = dwStart;
	bool bStalled = false;

	while (::GetTickCount() - dwStart < (DWORD)s_opt.nDurationS * 1000) {
		::Sleep(200);
		const DWORD dwNow = ::GetTickCount();

		::EnterCriticalSection(&s_csTodo);
		const INT_PTR nTodo = s_listTodo.GetCount();
		::LeaveCriticalSection(&s_csTodo);

		const DWORD dwSince = dwNow - (DWORD)s_dwLastProgress;
		if (nTodo > 0 && dwSince > (DWORD)s_opt.nStallMs) {
			Log("STALL: no progress for %ums with %d requests waiting and %ld outstanding"
				, dwSince, (int)nTodo, s_nInFlight);
			bStalled = true;
			break;
		}

		if (dwNow - dwLastReport >= 5000) {
			dwLastReport = dwNow;
			Log("issued=%ld completed=%ld inflight=%ld todo=%d loops=%ld posted=%ld taken=%ld"
				, s_nIssued, s_nCompleted, s_nInFlight, (int)nTodo, s_nLoops, s_nPosted, s_nWakeTaken);
		}
	}

	int nExit = 0;
	if (bStalled) {
		Autopsy();
		nExit = 1;
	}

	// Stop producing, let the port quiesce, and only then compare the books:
	// while producers are running, posted and taken are expected to differ by
	// whatever is still in the queue.
	::InterlockedExchange(&s_bStopProducers, 1);
	::Sleep(1500);

	const LONG nLostWakeups = s_nPosted - s_nWakeTaken;
	const LONG nLostCompletions = s_nIssued - s_nCompleted;

	Log("totals: requested=%ld issued=%ld completed=%ld posted=%ld taken=%ld loops=%ld outer=%ld drained=%ld"
		, s_nRequested, s_nIssued, s_nCompleted, s_nPosted, s_nWakeTaken, s_nLoops, s_nOuter, s_nDrained);

	// The ledger. Reported whether or not anything stalled, because a packet
	// delivered to the wrong key or a block read from the wrong offset is a
	// finding that a stress run can easily survive.
	Probe("ledger", "mode=%s bogus=%ld keybad=%ld short=%ld corrupt=%ld orphan=%ld iofail=%ld"
		, s_pszModeNames[s_opt.nMode], s_nBogus, s_nKeyBad, s_nShort, s_nCorrupt, s_nOrphan, s_nIoFailed);

	if (s_nBogus || s_nKeyBad || s_nCorrupt || s_nShort)
		nExit = 1;

	if (!bStalled) {
		if (s_opt.nMode == MODE_STRICT && (nLostWakeups > 0 || nLostCompletions > 0)) {
			Log("VERDICT: PACKETS LOST without a stall: %ld of %ld wakeups and %ld of %ld"
				, nLostWakeups, s_nPosted, nLostCompletions, s_nIssued);
			Log("VERDICT: completions never came back out of the port.");
			nExit = 1;
		} else
			Log("VERDICT: no stall in %ds with mode=%s (%ld reads completed, %ld wait cycles, %ld wakeups)"
				, s_opt.nDurationS, s_pszModeNames[s_opt.nMode], s_nCompleted, s_nLoops, s_nWakeTaken);
	}

	// eMule's EndThread: the key-0 packet is what actually ends the loop.
	s_Run = RUN_STOP;
	::PostQueuedCompletionStatus(s_hPort, 0, 0, NULL);
	if (s_pIoThread && s_pIoThread->m_hThread)
		::WaitForSingleObject(s_pIoThread->m_hThread, 5000);

	for (int i = 0; i < s_opt.nPending; ++i)
		delete[] s_pPool[i].pBuffer;
	delete[] s_pPool;
	::CloseHandle(s_hPort);
	DeleteTestFile(s_stFile);
	return nExit;
}

/////////////////////////////////////////////////////////////////////////////
// Mode "bigoff": the 64-bit offset
//
// eMule stores the offset by writing a uint64 over the OVERLAPPED's two DWORDs
// (*(uint64*)&pOverlappedRead->oOverlap.Offset = currentblock->StartOffset) and
// its part files routinely go past 4 GB. If the high half were ever dropped,
// every read above 4 GB would silently return the block 4 GB lower down - the
// data would look valid, and the file would be corrupt.

static bool BigIo(HANDLE hPort, HANDLE hFile, bool bWrite, ULONGLONG uOff, DWORD dwLen, BYTE *pBuf, DWORD &rdwBytes)
{
	OverlappedIo_Struct stIo;
	::ZeroMemory(&stIo, sizeof stIo);
	*(ULONGLONG*)&stIo.oOverlap.Offset = uOff;

	const BOOL bRet = bWrite
		? ::WriteFile(hFile, pBuf, dwLen, NULL, (LPOVERLAPPED)&stIo)
		: ::ReadFile(hFile, pBuf, dwLen, NULL, (LPOVERLAPPED)&stIo);
	if (!bRet && ::GetLastError() != ERROR_IO_PENDING)
		return false;

	DWORD dwBytes = 0;
	ULONG_PTR key = 0;
	LPOVERLAPPED pOv = NULL;
	if (!::GetQueuedCompletionStatus(hPort, &dwBytes, &key, &pOv, 5000) || pOv != (LPOVERLAPPED)&stIo)
		return false;
	rdwBytes = dwBytes;
	return true;
}

static int RunBigOffset()
{
	TCHAR szPath[MAX_PATH];
	if (!MakeTempPath(szPath)) {
		Log("no temporary path");
		return 2;
	}

	HANDLE h = ::CreateFile(szPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		Log("CreateFile failed err=%u", ::GetLastError());
		return 2;
	}
	DWORD dwRet = 0;
	const BOOL bSparse = ::DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwRet, NULL);
	LARGE_INTEGER liSize;
	liSize.QuadPart = (LONGLONG)s_opt.nBigGB << 30;
	const BOOL bSized = ::SetFilePointerEx(h, liSize, NULL, FILE_BEGIN) && ::SetEndOfFile(h);
	::CloseHandle(h);
	Probe("bigoff-file", "sparse=%d sized=%d gb=%d", bSparse != 0, bSized != 0, s_opt.nBigGB);
	if (!bSized) {
		::DeleteFile(szPath);
		return 2;
	}

	h = ::CreateFile(szPath, GENERIC_READ | GENERIC_WRITE
		, FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE
		, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	HANDLE hPort = h == INVALID_HANDLE_VALUE ? NULL : ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
	if (!hPort || ::CreateIoCompletionPort(h, hPort, (ULONG_PTR)h, 0) != hPort) {
		Log("could not associate the sparse file, err=%u", ::GetLastError());
		if (h != INVALID_HANDLE_VALUE)
			::CloseHandle(h);
		::DeleteFile(szPath);
		return 2;
	}

	const ULONGLONG u4G = 0x100000000ull;
	struct { const char *pszName; ULONGLONG uOff; DWORD dwLen; } aCases[] =
	{
		{ "bigoff-below-4g",   u4G - 65536,                 4096 },
		{ "bigoff-straddle",   u4G - 2048,                  4096 },
		{ "bigoff-at-4g",      u4G,                         4096 },
		{ "bigoff-above-4g",   u4G + 1048576,               4096 },
		{ "bigoff-near-end",   ((ULONGLONG)s_opt.nBigGB << 30) - 65536, 4096 }
	};

	int nExit = 0;
	BYTE *pWrite = new BYTE[65536];
	BYTE *pRead = new BYTE[65536];
	for (int i = 0; i < (int)_countof(aCases); ++i) {
		const ULONGLONG uOff = aCases[i].uOff;
		const DWORD dwLen = aCases[i].dwLen;
		FillPattern(pWrite, uOff, dwLen);
		::memset(pRead, 0xCC, dwLen);

		DWORD dwWrote = 0, dwRead = 0;
		const bool bW = BigIo(hPort, h, true, uOff, dwLen, pWrite, dwWrote);
		const bool bR = bW && BigIo(hPort, h, false, uOff, dwLen, pRead, dwRead);
		const LONG nBad = (bR && dwRead == dwLen) ? CheckPattern(pRead, uOff, dwLen) : 0;

		Probe(aCases[i].pszName, "off=0x%I64x wrote=%s read=%s data=%s"
			, uOff
			, bW ? (dwWrote == dwLen ? "full" : "short") : "FAILED"
			, bR ? (dwRead == dwLen ? "full" : "short") : "FAILED"
			, (bR && dwRead == dwLen) ? (nBad < 0 ? "ok" : "WRONG") : "n/a");
		if (!bW || !bR || dwWrote != dwLen || dwRead != dwLen || nBad >= 0)
			nExit = 1;
	}

	// The one failure a byte-for-byte check cannot see on its own: if the high
	// half of the offset were dropped on BOTH the write and the read, every
	// check above would still pass while the data sat 4 GB lower down. So look
	// there: the block written at 4 GB must NOT have landed at 0.
	{
		::memset(pRead, 0xCC, 4096);
		DWORD dwRead = 0;
		const bool bR = BigIo(hPort, h, false, 0, 4096, pRead, dwRead);
		const bool bAliased = bR && dwRead == 4096 && CheckPattern(pRead, u4G, 4096) < 0;
		Probe("bigoff-alias", "read=%s aliased=%s", bR ? "ok" : "FAILED", bAliased ? "YES-4GB-DROPPED" : "no");
		if (bAliased)
			nExit = 1;
	}

	LARGE_INTEGER liFinal;
	liFinal.QuadPart = 0;
	::GetFileSizeEx(h, &liFinal);
	Probe("bigoff-size", "asked=%I64d got=%s", liSize.QuadPart, liFinal.QuadPart == liSize.QuadPart ? "same" : "DIFFERENT");

	delete[] pWrite;
	delete[] pRead;
	::CloseHandle(h);
	::CloseHandle(hPort);
	::DeleteFile(szPath);
	return nExit;
}

/////////////////////////////////////////////////////////////////////////////

static void Usage()
{
	printf("iocprepro - eMule's completion-port loop, on its own\n"
		"\n"
		"  --mode <contract|strict|emule|bigoff>  what to run (default contract)\n"
		"  --pending <n>      overlapped reads in flight at once (default 32)\n"
		"  --producers <n>    threads asking for blocks (default 4)\n"
		"  --delay-ms <n>     pause between requests, per producer (default 0)\n"
		"  --block-kb <n>     size of one read (default 64)\n"
		"  --file-mb <n>      size of the test file (default 64)\n"
		"  --stall-ms <n>     no progress for this long with work waiting = stall (default 5000)\n"
		"  --duration-s <n>   how long to run the stress modes (default 60)\n"
		"  --big-gb <n>       bigoff: how far out the sparse file goes (default 5)\n"
		"\n"
		"Exit code 1 means something was found: a stall, a lost packet, or a\n"
		"ledger entry that cannot happen on a correct implementation.\n");
}

static bool ParseArgs(int argc, char *argv[])
{
	for (int i = 1; i < argc; ++i) {
		const char *p = argv[i];
		const bool bHasVal = (i + 1 < argc);
		if (!strcmp(p, "--mode") && bHasVal) {
			const char *pszVal = argv[++i];
			int nFound = -1;
			for (int m = 0; m < (int)_countof(s_pszModeNames); ++m)
				if (!strcmp(pszVal, s_pszModeNames[m]))
					nFound = m;
			if (nFound < 0) {
				printf("unknown mode: %s\n", pszVal);
				return false;
			}
			s_opt.nMode = nFound;
		} else if (!strcmp(p, "--pending") && bHasVal)
			s_opt.nPending = atoi(argv[++i]);
		else if (!strcmp(p, "--producers") && bHasVal)
			s_opt.nProducers = atoi(argv[++i]);
		else if (!strcmp(p, "--delay-ms") && bHasVal)
			s_opt.nDelayMs = atoi(argv[++i]);
		else if (!strcmp(p, "--block-kb") && bHasVal)
			s_opt.nBlockKB = atoi(argv[++i]);
		else if (!strcmp(p, "--file-mb") && bHasVal)
			s_opt.nFileMB = atoi(argv[++i]);
		else if (!strcmp(p, "--stall-ms") && bHasVal)
			s_opt.nStallMs = atoi(argv[++i]);
		else if (!strcmp(p, "--duration-s") && bHasVal)
			s_opt.nDurationS = atoi(argv[++i]);
		else if (!strcmp(p, "--big-gb") && bHasVal)
			s_opt.nBigGB = atoi(argv[++i]);
		else {
			Usage();
			return false;
		}
	}
	if (s_opt.nPending < 1 || s_opt.nBlockKB < 1 || s_opt.nFileMB < 2 || s_opt.nBigGB < 5) {
		printf("a value is out of range (--big-gb must be at least 5, the file at least 2 MB)\n");
		return false;
	}
	if ((ULONGLONG)s_opt.nFileMB * 1024 < (ULONGLONG)s_opt.nBlockKB * 4) {
		printf("the test file is too small for that block size\n");
		return false;
	}
	return true;
}

int main(int argc, char *argv[])
{
	::InitializeCriticalSection(&s_csLog);
	::InitializeCriticalSection(&s_csTodo);

	if (!AfxWinInit(::GetModuleHandle(NULL), NULL, ::GetCommandLine(), 0)) {
		printf("AfxWinInit failed\n");
		return 2;
	}
	if (!ParseArgs(argc, argv))
		return 2;

	Log("start: mode=%s pending=%d producers=%d delay=%dms block=%dKB file=%dMB duration=%ds"
		, s_pszModeNames[s_opt.nMode], s_opt.nPending, s_opt.nProducers
		, s_opt.nDelayMs, s_opt.nBlockKB, s_opt.nFileMB, s_opt.nDurationS);

	int nExit;
	switch (s_opt.nMode) {
	case MODE_CONTRACT:
		nExit = RunContract();
		break;
	case MODE_BIGOFF:
		nExit = RunBigOffset();
		break;
	default:
		nExit = RunStress();
		break;
	}

	Log("exit=%d", nExit);
	::DeleteCriticalSection(&s_csTodo);
	::DeleteCriticalSection(&s_csLog);
	return nExit;
}
