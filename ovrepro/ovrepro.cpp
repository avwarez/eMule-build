// ovrepro.cpp - a standalone specimen for eMule's overlapped send.
//
// WHY THIS EXISTS
// ---------------
// eMule uploads through overlapped WSASend. Each socket keeps one outstanding
// send (srchybrid/EMSocket.cpp, CEMSocket::SendOv), whose WSAOVERLAPPED carries
// an event SHARED BY EVERY SOCKET - the throttler's m_eventSocketAvailable -
// and a flag, m_bPendingSendOv, that stays raised until somebody collects the
// completion with WSAGetOverlappedResult.
//
// In the field, sockets are found with the completion ready and the flag still
// raised - 53 episodes measured, 58% of them ending with the upload slot dead.
// Whose defect that is has never been established, and there are two candidates
// that the running application cannot tell apart:
//
//   the platform - a completion that the implementation never reports, the
//   overlapped-send version of the lost FD_ACCEPT edge that wsrepro/ found;
//
//   eMule - because the ONLY collector, CEMSocket::IsBusyExtensiveCheck(), is
//   reached from exactly one place (UploadBandwidthThrottler.cpp:393), and that
//   place is guarded twice over: the socket must be inside the active window
//   (the first few slots) AND it must have something queued. Every other path
//   in the throttler asks IsBusyQuickCheck(), which only reads the raised flag
//   and skips the socket. So a send that finishes while its queue is empty, on
//   a socket outside the window, has nobody left to collect it - and the flag
//   that hides it is the same flag that keeps the collector away.
//
// This program is that arrangement with nothing else around it: real sockets, a
// real peer that stops reading, the same shared event, the same two gates, and
// a ledger. Run on Windows it says whether eMule's logic alone is enough to
// kill a slot; run under Wine with the same binary it says whether the platform
// adds anything of its own.
//
// WHAT IT MEASURES
// ---------------
//   --mode contract  a fixed table of deterministic probes of the overlapped
//                    send contract, printed as timestamp-free PROBE lines for
//                    the workflow to DIFF between Windows and Wine. Among them
//                    the one eMule's cleanup path depends on: whether CancelIo
//                    called from another thread cancels anything.
//   --mode emule     the throttler's two gates, reproduced. A slot whose
//                    completion is ready and uncollected is an episode; one
//                    that never comes back is a dead slot.
//   --mode poll      every socket with an outstanding send is collected every
//                    loop, whatever its queue and wherever it sits. This is the
//                    proposed correction; here it is the control experiment.
//
// Built and run by .github/workflows/build-ovrepro.yml on the Windows runner
// and, with the very same x64 binary, under Wine on the Linux runner.

#include <afxwin.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

// MFC console application: AfxWinInit needs an application object, and
// CWinThread needs the module state it sets up. eMule's throttler is a
// CWinThread and its sockets are MFC-derived, so the specimen lives there too.
CWinApp theApp;

/////////////////////////////////////////////////////////////////////////////
// Options

enum EMode
{
	MODE_CONTRACT = 0,
	MODE_EMULE,
	MODE_POLL
};

static const char *const s_pszModeNames[] = { "contract", "emule", "poll" };

struct SOptions
{
	int nMode;
	u_short nPort;
	int nSlots;        // upload slots
	int nWindow;       // how many of them the collector is allowed to look at
	int nBlockKB;      // size of one queued block
	int nLoopMs;       // throttler loop period
	int nPeerReadKB;   // how much the peer takes per turn, per connection
	int nPeerStallMs;  // how long the peer stops reading, periodically
	int nStuckMs;      // a ready-but-uncollected send older than this is a dead slot
	int nDurationS;
	int nSndBufKB;     // SO_SNDBUF on the senders, to make a pending send easy to get
};

static SOptions s_opt = { MODE_CONTRACT, 4788, 8, 3, 64, 10, 32, 400, 5000, 60, 8 };

/////////////////////////////////////////////////////////////////////////////
// Output. Log() for a human reading one run, Probe() for the machine comparing
// two - so Probe carries nothing that can differ between two correct runs.

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
	printf("PROBE %-22s ", pszName);
	va_list args;
	va_start(args, pszFmt);
	vprintf(pszFmt, args);
	va_end(args);
	printf("\n");
	fflush(stdout);
	::LeaveCriticalSection(&s_csLog);
}

/////////////////////////////////////////////////////////////////////////////
// Shared state

static HANDLE s_hTerminate = NULL;
// eMule's m_eventSocketAvailable: ONE event for every socket in the throttler.
// MFC's CEvent defaults to auto-reset, so N completions can collapse into one
// wake - which is why the throttler polls rather than trusting it.
static HANDLE s_hSocketAvailable = NULL;

static SOCKET s_hListen = INVALID_SOCKET;

// The upload slot. Same members as the part of CEMSocket that matters here.
struct Slot
{
	SOCKET        hSocket;
	WSAOVERLAPPED ov;
	bool          bPendingOv;        // m_bPendingSendOv
	DWORD         dwHanded;          // bytes given to the outstanding send
	DWORD         dwPendingSince;    // m_dwSendBlockedSince
	DWORD         dwReadySince;      // m_dwDiagOvReadySince - monitor only, never collects
	volatile LONG nQueued;           // bytes waiting to be sent: "has queues"
	ULONGLONG     uHandedTotal;
	ULONGLONG     uDoneTotal;
	LONG          nEpisodes;         // times a ready completion was found uncollected
	DWORD         dwWorstMs;
	bool          bDead;             // still uncollected when the run ended
	BYTE         *pBuf;
	CRITICAL_SECTION cs;             // sendLocker
};

static Slot *s_pSlots = NULL;

static volatile LONG s_nSends = 0;          // WSASend calls that went out
static volatile LONG s_nImmediate = 0;      // ... and returned 0 straight away
static volatile LONG s_nPending = 0;        // ... and returned WSA_IO_PENDING
static volatile LONG s_nCollected = 0;      // completions taken by the collector
static volatile LONG s_nShort = 0;          // completions carrying fewer bytes than handed
static volatile LONG s_nSendErrors = 0;
static volatile LONG s_nLoops = 0;
static volatile LONG s_nWaitWoke = 0;       // waits ended by the shared event
static volatile LONG s_nWaitTimeout = 0;
static volatile LONG s_nEpisodesOpen = 0;
static volatile LONG s_nEpisodesTotal = 0;
static volatile LONG s_nBlocked = 0;        // loops a slot could not be given work because busy
static volatile LONG64 s_uPeerRecv = 0;     // bytes the peer actually read
// Not eMule's: the throttler has to be stopped before the peers close their
// sockets, or a send caught by the shutdown is counted as a failure of its own.
static volatile LONG s_bStopThrottler = 0;

/////////////////////////////////////////////////////////////////////////////
// The peer: accepts every connection and reads from it, sometimes stopping.
// A peer that stops reading is what makes a send go pending, which is the whole
// point - eMule's sends go pending because the client at the other end is slow.

static UINT AFX_CDECL PeerReaderFunc(LPVOID pParam)
{
	SOCKET hSocket = (SOCKET)(UINT_PTR)pParam;
	const int nChunk = s_opt.nPeerReadKB * 1024;
	char *pBuf = new char[nChunk];
	DWORD dwNextStall = ::GetTickCount() + 1000;

	for (;;) {
		if (::WaitForSingleObject(s_hTerminate, 0) != WAIT_TIMEOUT)
			break;
		const DWORD dwNow = ::GetTickCount();
		if (s_opt.nPeerStallMs > 0 && dwNow >= dwNextStall) {
			// Stop reading for a while: the sender's window fills, its send goes
			// pending, and it stays pending until this thread comes back.
			::Sleep(s_opt.nPeerStallMs);
			dwNextStall = ::GetTickCount() + 1000;
			continue;
		}
		const int nRes = recv(hSocket, pBuf, nChunk, 0);
		if (nRes > 0) {
			::InterlockedExchangeAdd64(&s_uPeerRecv, nRes);
			continue;
		}
		if (nRes == 0)
			break;
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			break;
		::Sleep(1);
	}
	delete[] pBuf;
	closesocket(hSocket);
	return 0;
}

static UINT AFX_CDECL PeerAcceptFunc(LPVOID)
{
	for (;;) {
		if (::WaitForSingleObject(s_hTerminate, 0) != WAIT_TIMEOUT)
			break;
		fd_set stRead;
		FD_ZERO(&stRead);
		FD_SET(s_hListen, &stRead);
		timeval stTimeout = { 0, 200 * 1000 };
		const int iReady = select(0, &stRead, NULL, NULL, &stTimeout);
		if (iReady == SOCKET_ERROR)
			break;
		if (iReady == 0)
			continue;

		SOCKET hAccepted = accept(s_hListen, NULL, NULL);
		if (hAccepted == INVALID_SOCKET)
			continue;

		const int nRcvBuf = s_opt.nSndBufKB * 1024;
		setsockopt(hAccepted, SOL_SOCKET, SO_RCVBUF, (const char*)&nRcvBuf, sizeof nRcvBuf);
		u_long ulNonBlock = 1;
		ioctlsocket(hAccepted, FIONBIO, &ulNonBlock);

		CWinThread *pT = new CWinThread(PeerReaderFunc, (LPVOID)(UINT_PTR)hAccepted);
		if (!pT->CreateThread()) {
			delete pT;
			closesocket(hAccepted);
		}
	}
	return 0;
}

static bool StartPeer()
{
	s_hListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s_hListen == INVALID_SOCKET)
		return false;

	SOCKADDR_IN stAddr;
	::ZeroMemory(&stAddr, sizeof stAddr);
	stAddr.sin_family = AF_INET;
	stAddr.sin_port = htons(s_opt.nPort);
	stAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s_hListen, (LPSOCKADDR)&stAddr, sizeof stAddr) || listen(s_hListen, SOMAXCONN))
		return false;

	CWinThread *pT = new CWinThread(PeerAcceptFunc, NULL);
	return pT->CreateThread() != FALSE;
}

// One connected socket, set up the way eMule's upload sockets are by the time
// they reach SendOv: connected, non-blocking, with a send buffer small enough
// that a peer which stops reading blocks the sender quickly.
static SOCKET ConnectToPeer()
{
	SOCKET hSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (hSocket == INVALID_SOCKET)
		return INVALID_SOCKET;

	const int nSndBuf = s_opt.nSndBufKB * 1024;
	setsockopt(hSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&nSndBuf, sizeof nSndBuf);

	SOCKADDR_IN stAddr;
	::ZeroMemory(&stAddr, sizeof stAddr);
	stAddr.sin_family = AF_INET;
	stAddr.sin_port = htons(s_opt.nPort);
	stAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(hSocket, (LPSOCKADDR)&stAddr, sizeof stAddr)) {
		closesocket(hSocket);
		return INVALID_SOCKET;
	}
	u_long ulNonBlock = 1;
	ioctlsocket(hSocket, FIONBIO, &ulNonBlock);
	return hSocket;
}

/////////////////////////////////////////////////////////////////////////////
// The slot, reproduced from CEMSocket
//
// Four functions and their exact relationship: the flag, the only collector,
// the cheap check that gates every other path, and the cleanup.

static void NoteEpisodeEnd(Slot &r)
{
	if (r.dwReadySince != 0) {
		const DWORD dwLasted = ::GetTickCount() - r.dwReadySince;
		if (dwLasted > r.dwWorstMs)
			r.dwWorstMs = dwLasted;
		r.dwReadySince = 0;
		::InterlockedDecrement(&s_nEpisodesOpen);
	}
}

// CEMSocket::CleanUpOverlappedSendOperation, including the loop that follows
// CancelIo - five tries, twenty milliseconds apart, and then it gives up.
static void CleanUpOverlappedSendOperation(Slot &r, bool bCancel)
{
	if (r.bPendingOv) {
		NoteEpisodeEnd(r);
		r.bPendingOv = false;
		r.dwPendingSince = 0;
		if (bCancel && ::CancelIo((HANDLE)r.hSocket))
			for (int i = 5; --i >= 0;) {
				DWORD dwTransferred, dwFlags;
				if (::WSAGetOverlappedResult(r.hSocket, &r.ov, &dwTransferred, FALSE, &dwFlags))
					break;
				if (::WSAGetLastError() != WSA_IO_INCOMPLETE)
					break;
				::Sleep(20);
			}
	}
}

// CEMSocket::IsBusyExtensiveCheck - the only place in the whole program that
// takes a finished send off a socket.
static bool IsBusyExtensiveCheck(Slot &r)
{
	::EnterCriticalSection(&r.cs);
	bool bBusy;
	if (!r.bPendingOv)
		bBusy = false;
	else {
		DWORD dwTransferred = 0, dwFlags = 0;
		if (::WSAGetOverlappedResult(r.hSocket, &r.ov, &dwTransferred, FALSE, &dwFlags)) {
			if (dwTransferred != r.dwHanded)
				::InterlockedIncrement(&s_nShort);
			r.uDoneTotal += dwTransferred;
			::InterlockedIncrement(&s_nCollected);
			CleanUpOverlappedSendOperation(r, false);
			bBusy = false;
		} else if (::WSAGetLastError() == WSA_IO_INCOMPLETE)
			bBusy = true;
		else {
			CleanUpOverlappedSendOperation(r, true);
			bBusy = false;
		}
	}
	::LeaveCriticalSection(&r.cs);
	return bBusy;
}

// CEMSocket::IsBusyQuickCheck - "won't always deliver the proper result but
// doesn't need locks or function calls". Every path in the throttler except one
// asks this, and it can only ever read the raised flag.
static bool IsBusyQuickCheck(const Slot &r)
{
	return r.bPendingOv;
}

// CEMSocket::SendOv, reduced to what it does to the socket: collect first, then
// hand a vector of buffers to one overlapped WSASend carrying the shared event.
static void SendOv(Slot &r)
{
	::EnterCriticalSection(&r.cs);
	if (!IsBusyExtensiveCheck(r) && r.nQueued > 0) {
		const DWORD dwSend = (DWORD)min(r.nQueued, (LONG)s_opt.nBlockKB * 1024);

		// eMule always sends a vector: the buffers of several queued packets at
		// once, so that one call ships them all without moving memory.
		WSABUF aBuffer[4];
		const DWORD dwPiece = dwSend / 4;
		for (int i = 0; i < 4; ++i) {
			aBuffer[i].buf = (CHAR*)r.pBuf + (size_t)i * dwPiece;
			aBuffer[i].len = (i == 3) ? dwSend - dwPiece * 3 : dwPiece;
		}

		::memset(&r.ov, 0, sizeof(WSAOVERLAPPED));
		r.ov.hEvent = s_hSocketAvailable;
		r.bPendingOv = true;
		r.dwPendingSince = ::GetTickCount();
		r.dwHanded = dwSend;
		::InterlockedIncrement(&s_nSends);

		// Note the NULL where the byte count would go: legal only because an
		// overlapped structure is passed, and the reason eMule never learns how
		// much an immediately-completed send actually took.
		if (::WSASend(r.hSocket, aBuffer, 4, NULL, 0, &r.ov, NULL) == 0) {
			::InterlockedIncrement(&s_nImmediate);
			r.uHandedTotal += dwSend;
			r.uDoneTotal += dwSend;
			::InterlockedExchangeAdd(&r.nQueued, -(LONG)dwSend);
			CleanUpOverlappedSendOperation(r, false);
		} else {
			const int nError = ::WSAGetLastError();
			if (nError == WSA_IO_PENDING) {
				::InterlockedIncrement(&s_nPending);
				r.uHandedTotal += dwSend;
				::InterlockedExchangeAdd(&r.nQueued, -(LONG)dwSend);
			} else {
				::InterlockedIncrement(&s_nSendErrors);
				CleanUpOverlappedSendOperation(r, false);
			}
		}
	}
	::LeaveCriticalSection(&r.cs);
}

// CEMSocket::DbgProbeUncollectedSend: asks whether the completion is there
// WITHOUT taking it, so that watching the fault cannot also cure it.
static void ProbeSlot(Slot &r)
{
	::EnterCriticalSection(&r.cs);
	if (!r.bPendingOv)
		r.dwReadySince = 0;
	else {
		DWORD dwTransferred = 0, dwFlags = 0;
		if (!::WSAGetOverlappedResult(r.hSocket, &r.ov, &dwTransferred, FALSE, &dwFlags)) {
			if (::WSAGetLastError() == WSA_IO_INCOMPLETE)
				r.dwReadySince = 0;   // still with the network: the ordinary case
		} else if (r.dwReadySince == 0) {
			r.dwReadySince = ::GetTickCount();
			++r.nEpisodes;
			::InterlockedIncrement(&s_nEpisodesTotal);
			::InterlockedIncrement(&s_nEpisodesOpen);
		} else {
			const DWORD dwLasted = ::GetTickCount() - r.dwReadySince;
			if (dwLasted > r.dwWorstMs)
				r.dwWorstMs = dwLasted;
		}
	}
	::LeaveCriticalSection(&r.cs);
}

/////////////////////////////////////////////////////////////////////////////
// The throttler: UploadBandwidthThrottler::RunInternal, reduced to its two
// gates and its wait.

static UINT AFX_CDECL ThrottlerFunc(LPVOID)
{
	const int nWindow = (s_opt.nMode == MODE_POLL)
		? s_opt.nSlots
		: min(s_opt.nSlots, s_opt.nWindow);

	while (!s_bStopThrottler) {
		::InterlockedIncrement(&s_nLoops);
		::ResetEvent(s_hSocketAvailable);

		// The collector's two gates. In the throttler they read:
		//     for (i = window; --i >= 0;)
		//         if (pSocket->HasQueues())
		//             nBusy += pSocket->IsBusyExtensiveCheck();
		// Outside the window, or with an empty queue, nobody asks.
		for (int i = 0; i < nWindow; ++i) {
			Slot &r = s_pSlots[i];
			if (s_opt.nMode == MODE_POLL || r.nQueued > 0)
				IsBusyExtensiveCheck(r);
		}

		// Every sending path gates on the cheap check, which can only see the
		// raised flag - so a socket whose completion was never collected is
		// skipped here for as long as that lasts.
		for (int i = 0; i < s_opt.nSlots; ++i) {
			Slot &r = s_pSlots[i];
			if (IsBusyQuickCheck(r)) {
				if (r.nQueued > 0)
					::InterlockedIncrement(&s_nBlocked);
				continue;
			}
			SendOv(r);
		}

		if (::WaitForSingleObject(s_hSocketAvailable, s_opt.nLoopMs) == WAIT_OBJECT_0)
			::InterlockedIncrement(&s_nWaitWoke);
		else
			::InterlockedIncrement(&s_nWaitTimeout);
	}
	return 0;
}

// The disk I/O thread's side: blocks arrive for a slot, then stop for a while.
// The gap is what matters - a send that finishes while the queue is empty is
// the one nobody comes back for.
static UINT AFX_CDECL FeederFunc(LPVOID)
{
	unsigned uSeed = ::GetCurrentThreadId();
	while (::WaitForSingleObject(s_hTerminate, 0) == WAIT_TIMEOUT) {
		for (int i = 0; i < s_opt.nSlots; ++i) {
			uSeed = uSeed * 1103515245u + 12345u;
			if (s_pSlots[i].nQueued == 0 && (uSeed >> 16) % 3 == 0)
				::InterlockedExchange(&s_pSlots[i].nQueued, (LONG)s_opt.nBlockKB * 1024);
		}
		::Sleep(10);
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Mode "contract": deterministic probes
//
// Every one answers a question eMule's code already assumes the answer to.

struct PairSockets
{
	SOCKET hListen;
	SOCKET hSender;
	SOCKET hPeer;
};

static bool MakePair(PairSockets &r, u_short nPort)
{
	r.hListen = r.hSender = r.hPeer = INVALID_SOCKET;
	r.hListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (r.hListen == INVALID_SOCKET)
		return false;

	SOCKADDR_IN stAddr;
	::ZeroMemory(&stAddr, sizeof stAddr);
	stAddr.sin_family = AF_INET;
	stAddr.sin_port = htons(nPort);
	stAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	// On the LISTENING socket, so the accepted one inherits them: set after
	// accept() the receive size can arrive too late to hold down the window,
	// and then the peer quietly swallows megabytes it was never meant to take.
	const int nInherit = s_opt.nSndBufKB * 1024;
	setsockopt(r.hListen, SOL_SOCKET, SO_RCVBUF, (const char*)&nInherit, sizeof nInherit);
	setsockopt(r.hListen, SOL_SOCKET, SO_SNDBUF, (const char*)&nInherit, sizeof nInherit);
	if (bind(r.hListen, (LPSOCKADDR)&stAddr, sizeof stAddr) || listen(r.hListen, 4))
		return false;

	r.hSender = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (r.hSender == INVALID_SOCKET)
		return false;
	const int nBuf = s_opt.nSndBufKB * 1024;
	setsockopt(r.hSender, SOL_SOCKET, SO_SNDBUF, (const char*)&nBuf, sizeof nBuf);
	if (connect(r.hSender, (LPSOCKADDR)&stAddr, sizeof stAddr))
		return false;

	r.hPeer = accept(r.hListen, NULL, NULL);
	if (r.hPeer == INVALID_SOCKET)
		return false;
	setsockopt(r.hPeer, SOL_SOCKET, SO_RCVBUF, (const char*)&nBuf, sizeof nBuf);
	u_long ulNonBlock = 1;
	ioctlsocket(r.hPeer, FIONBIO, &ulNonBlock);
	ioctlsocket(r.hSender, FIONBIO, &ulNonBlock);
	return true;
}

static void ClosePair(PairSockets &r)
{
	if (r.hSender != INVALID_SOCKET)
		closesocket(r.hSender);
	if (r.hPeer != INVALID_SOCKET)
		closesocket(r.hPeer);
	if (r.hListen != INVALID_SOCKET)
		closesocket(r.hListen);
	r.hListen = r.hSender = r.hPeer = INVALID_SOCKET;
}

// Reads whatever is there, for at most so long. Returns the byte count.
static ULONGLONG DrainPeer(SOCKET hPeer, DWORD dwMs)
{
	ULONGLONG uTotal = 0;
	char *pBuf = new char[64 * 1024];
	const DWORD dwEnd = ::GetTickCount() + dwMs;
	while ((LONG)(dwEnd - ::GetTickCount()) > 0) {
		const int nRes = recv(hPeer, pBuf, 64 * 1024, 0);
		if (nRes > 0) {
			uTotal += (ULONGLONG)nRes;
			continue;
		}
		if (nRes == 0 || ::WSAGetLastError() != WSAEWOULDBLOCK)
			break;
		::Sleep(1);
	}
	delete[] pBuf;
	return uTotal;
}

static UINT AFX_CDECL DrainPeerThreadFunc(LPVOID pParam)
{
	DrainPeer((SOCKET)(UINT_PTR)pParam, 5000);
	return 0;
}

static SOCKET s_hCancelTarget = INVALID_SOCKET;
static volatile LONG s_bCancelResult = 0;

static UINT AFX_CDECL CancelFromAnotherThread(LPVOID)
{
	::InterlockedExchange(&s_bCancelResult, ::CancelIo((HANDLE)s_hCancelTarget) ? 1 : 0);
	return 0;
}

// Leaves an outstanding send on the socket, and says how much it took to get
// one. How much is not a constant: a peer that has stopped reading still has a
// receive window, and the sending stack still has a buffer, so the first sends
// are simply swallowed and complete at once. This keeps sending until one of
// them cannot be, which is the state every probe below is about.
//
// Returns the number of attempts, 0 if none of them went pending, -1 on error.
// The same WSAOVERLAPPED is reused for each attempt: only the last one is still
// owned by the system when this returns.
static int IssuePendingSend(SOCKET hSender, WSAOVERLAPPED &rOv, HANDLE hEvent, BYTE *pBuf, DWORD dwLen)
{
	for (int i = 1; i <= 16; ++i) {
		::memset(&rOv, 0, sizeof(WSAOVERLAPPED));
		rOv.hEvent = hEvent;
		WSABUF stBuf;
		stBuf.buf = (CHAR*)pBuf;
		stBuf.len = dwLen;
		if (::WSASend(hSender, &stBuf, 1, NULL, 0, &rOv, NULL) != 0)
			return (::WSAGetLastError() == WSA_IO_PENDING) ? i : -1;
		// Completed at once: the data is gone from our hands, so go again.
	}
	return 0;
}

static int RunContract()
{
	const DWORD dwBig = 4 * 1024 * 1024;   // far past any send and receive buffer
	BYTE *pBig = new BYTE[dwBig];
	::memset(pBig, 0x5A, dwBig);
	HANDLE hEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);   // auto-reset, as eMule's
	int nExit = 0;

	// 1. A small send on an idle socket: the path eMule takes most often.
	{
		PairSockets stPair;
		if (MakePair(stPair, s_opt.nPort)) {
			WSAOVERLAPPED stOv;
			::memset(&stOv, 0, sizeof stOv);
			stOv.hEvent = hEvent;
			::ResetEvent(hEvent);
			WSABUF stBuf;
			stBuf.buf = (CHAR*)pBig;
			stBuf.len = 1024;
			const int nRet = ::WSASend(stPair.hSender, &stBuf, 1, NULL, 0, &stOv, NULL);
			const int nErr = ::WSAGetLastError();
			const bool bSignalled = ::WaitForSingleObject(hEvent, 1000) == WAIT_OBJECT_0;
			DWORD dwTransferred = 0, dwFlags = 0;
			const BOOL bRes = ::WSAGetOverlappedResult(stPair.hSender, &stOv, &dwTransferred, FALSE, &dwFlags);
			Probe("send-small", "accepted=%d event=%s result=%s bytes=%s"
				, nRet == 0 || nErr == WSA_IO_PENDING
				, bSignalled ? "signalled" : "NOT-SIGNALLED"
				, bRes ? "TRUE" : "FALSE"
				, dwTransferred == 1024 ? "full" : "WRONG");
			Log("note: send-small returned %s", nRet == 0 ? "immediately" : "pending");
			DrainPeer(stPair.hPeer, 200);
			ClosePair(stPair);
		} else
			Probe("send-small", "skipped");
	}

	// 2 and 3. A send the peer refuses to take, and the same send once it does.
	//    This is the ordinary life of an eMule upload socket.
	{
		PairSockets stPair;
		if (MakePair(stPair, s_opt.nPort)) {
			WSAOVERLAPPED stOv;
			::ResetEvent(hEvent);
			const int nAttempts = IssuePendingSend(stPair.hSender, stOv, hEvent, pBig, dwBig);
			Log("note: send-blocked needed %d sends of %uKB to leave one outstanding", nAttempts, dwBig / 1024);
			if (nAttempts <= 0) {
				Probe("send-blocked", "pending=0 attempts=exhausted");
				Probe("send-completes", "skipped");
			} else {
				DWORD dwTransferred = 0, dwFlags = 0;
				const BOOL bEarly = ::WSAGetOverlappedResult(stPair.hSender, &stOv, &dwTransferred, FALSE, &dwFlags);
				const int nEarlyErr = ::WSAGetLastError();
				const bool bEarlySignal = ::WaitForSingleObject(hEvent, 200) == WAIT_OBJECT_0;
				Probe("send-blocked", "pending=1 result=%s err=%u event=%s"
					, bEarly ? "TRUE" : "FALSE", bEarly ? 0 : nEarlyErr
					, bEarlySignal ? "SIGNALLED-EARLY" : "quiet");

				const ULONGLONG uRead = DrainPeer(stPair.hPeer, 5000);
				const bool bSignalled = ::WaitForSingleObject(hEvent, 2000) == WAIT_OBJECT_0;
				const BOOL bRes = ::WSAGetOverlappedResult(stPair.hSender, &stOv, &dwTransferred, FALSE, &dwFlags);
				Probe("send-completes", "event=%s result=%s bytes=%s peer=%s"
					, bSignalled ? "signalled" : "NOT-SIGNALLED"
					, bRes ? "TRUE" : "FALSE"
					, dwTransferred == dwBig ? "full" : "WRONG"
					, uRead >= dwBig ? "all" : "partial");
				if (!bRes || dwTransferred != dwBig)
					nExit = 1;
			}
			ClosePair(stPair);
		} else
			Probe("send-blocked", "skipped");
	}

	// 4. The question eMule's cleanup path rests on. CancelIo cancels the I/O
	//    issued BY THE CALLING THREAD; eMule calls it from whichever thread is
	//    tearing the socket down, which is not always the one that issued the
	//    send - and then frees the WSABUFs the send is still using.
	{
		PairSockets stPair;
		if (MakePair(stPair, s_opt.nPort)) {
			WSAOVERLAPPED stOv;
			::ResetEvent(hEvent);
			const int nAttempts = IssuePendingSend(stPair.hSender, stOv, hEvent, pBig, dwBig);
			s_hCancelTarget = stPair.hSender;
			s_bCancelResult = -1;
			CWinThread *pT = new CWinThread(CancelFromAnotherThread, NULL);
			pT->m_bAutoDelete = FALSE;
			if (nAttempts <= 0) {
				Probe("cancelio-other-thread", "no-pending-send");
			} else if (pT->CreateThread()) {
				::WaitForSingleObject(pT->m_hThread, 2000);
				::Sleep(100);
				DWORD dwTransferred = 0, dwFlags = 0;
				const BOOL bRes = ::WSAGetOverlappedResult(stPair.hSender, &stOv, &dwTransferred, FALSE, &dwFlags);
				const int nErr = ::WSAGetLastError();
				Probe("cancelio-other-thread", "returned=%ld cancelled=%s"
					, s_bCancelResult
					, bRes ? "no-completed" : (nErr == WSA_OPERATION_ABORTED ? "YES" : (nErr == WSA_IO_INCOMPLETE ? "no-still-pending" : "other")));
			} else
				Probe("cancelio-other-thread", "skipped");
			delete pT;
			DrainPeer(stPair.hPeer, 2000);
			ClosePair(stPair);
		} else
			Probe("cancelio-other-thread", "skipped");
	}

	// 5. The same call from the thread that issued the send.
	{
		PairSockets stPair;
		if (MakePair(stPair, s_opt.nPort)) {
			WSAOVERLAPPED stOv;
			::ResetEvent(hEvent);
			if (IssuePendingSend(stPair.hSender, stOv, hEvent, pBig, dwBig) <= 0)
				Probe("cancelio-same-thread", "no-pending-send");
			else {
				const BOOL bCancel = ::CancelIo((HANDLE)stPair.hSender);
				::Sleep(100);
				DWORD dwTransferred = 0, dwFlags = 0;
				const BOOL bRes = ::WSAGetOverlappedResult(stPair.hSender, &stOv, &dwTransferred, FALSE, &dwFlags);
				const int nErr = ::WSAGetLastError();
				Probe("cancelio-same-thread", "returned=%d cancelled=%s"
					, bCancel != 0
					, bRes ? "no-completed" : (nErr == WSA_OPERATION_ABORTED ? "YES" : (nErr == WSA_IO_INCOMPLETE ? "no-still-pending" : "other")));
			}
			DrainPeer(stPair.hPeer, 2000);
			ClosePair(stPair);
		} else
			Probe("cancelio-same-thread", "skipped");
	}

	// 6. One event, two sockets - eMule's arrangement exactly. Two completions
	//    on one auto-reset event cannot satisfy two waits, which is why the
	//    throttler polls instead of trusting it; the probe records what the
	//    implementation actually does.
	{
		PairSockets stA, stB;
		if (MakePair(stA, s_opt.nPort) && MakePair(stB, (u_short)(s_opt.nPort + 1))) {
			WSAOVERLAPPED stOvA, stOvB;
			::ResetEvent(hEvent);
			const int nA = IssuePendingSend(stA.hSender, stOvA, hEvent, pBig, dwBig);
			const int nB = IssuePendingSend(stB.hSender, stOvB, hEvent, pBig, dwBig);
			if (nA <= 0 || nB <= 0)
				Log("note: shared-event could not leave both sends outstanding (%d, %d)", nA, nB);
			DrainPeer(stA.hPeer, 3000);
			DrainPeer(stB.hPeer, 3000);
			int nSatisfied = 0;
			for (int i = 0; i < 3; ++i)
				if (::WaitForSingleObject(hEvent, 500) == WAIT_OBJECT_0)
					++nSatisfied;
			DWORD dwT = 0, dwF = 0;
			const BOOL bA = ::WSAGetOverlappedResult(stA.hSender, &stOvA, &dwT, FALSE, &dwF);
			const BOOL bB = ::WSAGetOverlappedResult(stB.hSender, &stOvB, &dwT, FALSE, &dwF);
			Probe("shared-event", "waits-satisfied=%d both-collectable=%d", nSatisfied, bA && bB);
			ClosePair(stA);
			ClosePair(stB);
		} else
			Probe("shared-event", "skipped");
	}

	// 7. The waiting form of the same call. eMule never uses it, but if it
	//    disagreed with the non-waiting one that would be worth knowing.
	{
		PairSockets stPair;
		if (MakePair(stPair, s_opt.nPort)) {
			WSAOVERLAPPED stOv;
			::ResetEvent(hEvent);
			const int nAttempts = IssuePendingSend(stPair.hSender, stOv, hEvent, pBig, dwBig);
			if (nAttempts <= 0)
				Log("note: getov-wait has no outstanding send to wait for");
			CWinThread *pT = new CWinThread(DrainPeerThreadFunc, (LPVOID)(UINT_PTR)stPair.hPeer);
			pT->CreateThread();
			DWORD dwTransferred = 0, dwFlags = 0;
			const DWORD dwT0 = ::GetTickCount();
			const BOOL bRes = ::WSAGetOverlappedResult(stPair.hSender, &stOv, &dwTransferred, TRUE, &dwFlags);
			const DWORD dwWaited = ::GetTickCount() - dwT0;
			Probe("getov-wait", "result=%s bytes=%s waited=%s"
				, bRes ? "TRUE" : "FALSE"
				, dwTransferred == dwBig ? "full" : "WRONG"
				, dwWaited > 5000 ? "TOO-LONG" : "bounded");
			::Sleep(200);
			ClosePair(stPair);
		} else
			Probe("getov-wait", "skipped");
	}

	// 8. The socket closed under a send that is still outstanding - which is
	//    what happens when a client disconnects mid-upload.
	{
		PairSockets stPair;
		if (MakePair(stPair, s_opt.nPort)) {
			WSAOVERLAPPED stOv;
			::ResetEvent(hEvent);
			if (IssuePendingSend(stPair.hSender, stOv, hEvent, pBig, dwBig) <= 0)
				Log("note: closed-under-send has no outstanding send to close under");
			const SOCKET hClosed = stPair.hSender;
			closesocket(hClosed);
			stPair.hSender = INVALID_SOCKET;
			::Sleep(100);
			DWORD dwTransferred = 0, dwFlags = 0;
			const BOOL bRes = ::WSAGetOverlappedResult(hClosed, &stOv, &dwTransferred, FALSE, &dwFlags);
			const int nErr = ::WSAGetLastError();
			Probe("closed-under-send", "result=%s err=%s"
				, bRes ? "TRUE" : "FALSE"
				, bRes ? "none" : (nErr == WSAENOTSOCK ? "notsock" : (nErr == WSA_OPERATION_ABORTED ? "aborted" : "other")));
			ClosePair(stPair);
		} else
			Probe("closed-under-send", "skipped");
	}

	// 9. The vector form eMule always uses: several buffers, one call.
	{
		PairSockets stPair;
		if (MakePair(stPair, s_opt.nPort)) {
			BYTE aData[4][256];
			WSABUF aBuffer[4];
			for (int i = 0; i < 4; ++i) {
				::memset(aData[i], (BYTE)('A' + i), sizeof aData[i]);
				aBuffer[i].buf = (CHAR*)aData[i];
				aBuffer[i].len = (ULONG)sizeof aData[i];
			}
			WSAOVERLAPPED stOv;
			::memset(&stOv, 0, sizeof stOv);
			stOv.hEvent = hEvent;
			::ResetEvent(hEvent);
			const int nRet = ::WSASend(stPair.hSender, aBuffer, 4, NULL, 0, &stOv, NULL);
			const bool bAccepted = (nRet == 0 || ::WSAGetLastError() == WSA_IO_PENDING);
			::WaitForSingleObject(hEvent, 1000);
			DWORD dwTransferred = 0, dwFlags = 0;
			const BOOL bRes = ::WSAGetOverlappedResult(stPair.hSender, &stOv, &dwTransferred, FALSE, &dwFlags);

			char szGot[1100];
			int nGot = 0;
			const DWORD dwEnd = ::GetTickCount() + 2000;
			while (nGot < 1024 && (LONG)(dwEnd - ::GetTickCount()) > 0) {
				const int nRes = recv(stPair.hPeer, szGot + nGot, 1024 - nGot, 0);
				if (nRes > 0)
					nGot += nRes;
				else if (nRes == 0 || ::WSAGetLastError() != WSAEWOULDBLOCK)
					break;
				else
					::Sleep(1);
			}
			bool bOrder = (nGot == 1024);
			for (int i = 0; bOrder && i < 1024; ++i)
				bOrder = (szGot[i] == 'A' + i / 256);
			Probe("multi-buffer", "accepted=%d result=%s bytes=%s peer-order=%s"
				, bAccepted, bRes ? "TRUE" : "FALSE"
				, dwTransferred == 1024 ? "full" : "WRONG"
				, bOrder ? "ok" : "WRONG");
			if (!bOrder)
				nExit = 1;
			ClosePair(stPair);
		} else
			Probe("multi-buffer", "skipped");
	}

	::CloseHandle(hEvent);
	delete[] pBig;
	return nExit;
}

/////////////////////////////////////////////////////////////////////////////
// Modes "emule" and "poll": the arrangement under load

static int RunStress()
{
	if (!StartPeer()) {
		Log("could not start the peer, err=%u", ::WSAGetLastError());
		return 2;
	}

	s_pSlots = new Slot[s_opt.nSlots];
	for (int i = 0; i < s_opt.nSlots; ++i) {
		Slot &r = s_pSlots[i];
		::ZeroMemory(&r, sizeof(Slot));
		::InitializeCriticalSection(&r.cs);
		r.pBuf = new BYTE[(size_t)s_opt.nBlockKB * 1024];
		::memset(r.pBuf, 'a' + i % 26, (size_t)s_opt.nBlockKB * 1024);
		r.hSocket = ConnectToPeer();
		if (r.hSocket == INVALID_SOCKET) {
			Log("slot %d could not connect, err=%u", i, ::WSAGetLastError());
			return 2;
		}
	}
	Log("%d slots connected, collector window = %d", s_opt.nSlots
		, s_opt.nMode == MODE_POLL ? s_opt.nSlots : min(s_opt.nSlots, s_opt.nWindow));

	CWinThread *pFeeder = new CWinThread(FeederFunc, NULL);
	pFeeder->CreateThread();
	CWinThread *pThrottler = new CWinThread(ThrottlerFunc, NULL);
	pThrottler->m_bAutoDelete = FALSE;
	if (!pThrottler->CreateThread()) {
		Log("throttler did not start");
		return 2;
	}

	const DWORD dwStart = ::GetTickCount();
	DWORD dwLastReport = dwStart;
	while (::GetTickCount() - dwStart < (DWORD)s_opt.nDurationS * 1000) {
		::Sleep(100);
		for (int i = 0; i < s_opt.nSlots; ++i)
			ProbeSlot(s_pSlots[i]);

		const DWORD dwNow = ::GetTickCount();
		if (dwNow - dwLastReport >= 5000) {
			dwLastReport = dwNow;
			Log("sends=%ld immediate=%ld pending=%ld collected=%ld episodes=%ld open=%ld blocked=%ld peer=%I64uKB"
				, s_nSends, s_nImmediate, s_nPending, s_nCollected
				, s_nEpisodesTotal, s_nEpisodesOpen, s_nBlocked, (ULONGLONG)s_uPeerRecv / 1024);
		}
	}

	::InterlockedExchange(&s_bStopThrottler, 1);
	if (pThrottler->m_hThread)
		::WaitForSingleObject(pThrottler->m_hThread, 5000);

	// A slot still holding an uncollected completion when the run ends, with
	// work waiting behind it, is a dead upload slot: nothing in the arrangement
	// will ever look at it again.
	int nDead = 0;
	ULONGLONG uHanded = 0, uDone = 0;
	DWORD dwWorst = 0;
	for (int i = 0; i < s_opt.nSlots; ++i) {
		Slot &r = s_pSlots[i];
		ProbeSlot(r);
		uHanded += r.uHandedTotal;
		uDone += r.uDoneTotal;
		if (r.dwWorstMs > dwWorst)
			dwWorst = r.dwWorstMs;
		const bool bDead = r.bPendingOv && r.dwReadySince != 0
			&& ::GetTickCount() - r.dwReadySince >= (DWORD)s_opt.nStuckMs;
		r.bDead = bDead;
		if (bDead)
			++nDead;
		if (r.nEpisodes > 0)
			Log("slot %d: episodes=%ld worst=%ums queued=%ld pending=%d%s"
				, i, r.nEpisodes, r.dwWorstMs, r.nQueued, (int)r.bPendingOv, bDead ? "  DEAD" : "");
	}

	Log("totals: sends=%ld immediate=%ld pending=%ld collected=%ld short=%ld errors=%ld loops=%ld woke=%ld timeout=%ld"
		, s_nSends, s_nImmediate, s_nPending, s_nCollected, s_nShort, s_nSendErrors
		, s_nLoops, s_nWaitWoke, s_nWaitTimeout);
	Log("bytes: handed=%I64uKB collected=%I64uKB peer=%I64uKB"
		, uHanded / 1024, uDone / 1024, (ULONGLONG)s_uPeerRecv / 1024);

	// The ledger: what cannot happen on a correct implementation, whatever the
	// program above it does.
	Probe("ledger", "mode=%s short=%ld senderr=%ld", s_pszModeNames[s_opt.nMode], s_nShort, s_nSendErrors);
	// The finding: how long a finished send went uncollected, and how many
	// slots never came back. In poll mode both must be zero - nothing there
	// can leave a completion behind - so a non-zero pair is the platform's.
	Probe("stuck", "mode=%s episodes=%ld dead=%d", s_pszModeNames[s_opt.nMode], s_nEpisodesTotal, nDead);

	int nExit = 0;
	if (s_nShort || s_nSendErrors)
		nExit = 1;

	if (s_opt.nMode == MODE_POLL) {
		if (nDead > 0 || s_nEpisodesTotal > 0) {
			Log("VERDICT: THE PLATFORM. Every socket with an outstanding send was collected");
			Log("VERDICT: every loop, so nothing here could leave a completion behind - and");
			Log("VERDICT: %ld were found ready and uncollected anyway, %d of them permanently."
				, s_nEpisodesTotal, nDead);
			nExit = 1;
		} else
			Log("VERDICT: no stuck send in %ds when every socket is collected (%ld sends, %ld collected)"
				, s_opt.nDurationS, s_nSends, s_nCollected);
	} else if (nDead > 0) {
		Log("VERDICT: eMule's arrangement alone kills upload slots. %d of %d slots ended", nDead, s_opt.nSlots);
		Log("VERDICT: holding a finished send that nobody will ever collect: outside the");
		Log("VERDICT: collector's window, or with an empty queue at the wrong moment, the");
		Log("VERDICT: only path to WSAGetOverlappedResult is closed by the very flag it");
		Log("VERDICT: would clear. Worst spell %ums. Compare with mode=poll, and with the", dwWorst);
		Log("VERDICT: other platform: if both agree, the defect is eMule's.");
		nExit = 1;
	} else if (s_nEpisodesTotal > 0) {
		Log("VERDICT: %ld sends were found finished but uncollected (worst %ums), and every"
			, s_nEpisodesTotal, dwWorst);
		Log("VERDICT: one of them was collected in the end - here the arrangement is slow,");
		Log("VERDICT: not fatal.");
	} else
		Log("VERDICT: no stuck send in %ds with mode=%s (%ld sends, %ld collected)"
			, s_opt.nDurationS, s_pszModeNames[s_opt.nMode], s_nSends, s_nCollected);

	return nExit;
}

/////////////////////////////////////////////////////////////////////////////

static void Usage()
{
	printf("ovrepro - eMule's overlapped send, on its own\n"
		"\n"
		"  --mode <contract|emule|poll>  what to run (default contract)\n"
		"  --port <n>         loopback port to use (default 4788)\n"
		"  --slots <n>        upload slots (default 8)\n"
		"  --window <n>       how many of them the collector may look at (default 3)\n"
		"  --block-kb <n>     size of one queued block (default 64)\n"
		"  --loop-ms <n>      throttler loop period (default 10)\n"
		"  --peer-read-kb <n> how much the peer takes per turn (default 32)\n"
		"  --peer-stall-ms <n> how long the peer stops reading, each second (default 400)\n"
		"  --stuck-ms <n>     uncollected for this long at the end = dead slot (default 5000)\n"
		"  --duration-s <n>   how long to run (default 60)\n"
		"  --sndbuf-kb <n>    SO_SNDBUF on the senders (default 8)\n"
		"\n"
		"Exit code 1 means something was found: a dead slot, a stuck completion\n"
		"where there can be none, or a send that did not transfer what it was given.\n");
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
		} else if (!strcmp(p, "--port") && bHasVal)
			s_opt.nPort = (u_short)atoi(argv[++i]);
		else if (!strcmp(p, "--slots") && bHasVal)
			s_opt.nSlots = atoi(argv[++i]);
		else if (!strcmp(p, "--window") && bHasVal)
			s_opt.nWindow = atoi(argv[++i]);
		else if (!strcmp(p, "--block-kb") && bHasVal)
			s_opt.nBlockKB = atoi(argv[++i]);
		else if (!strcmp(p, "--loop-ms") && bHasVal)
			s_opt.nLoopMs = atoi(argv[++i]);
		else if (!strcmp(p, "--peer-read-kb") && bHasVal)
			s_opt.nPeerReadKB = atoi(argv[++i]);
		else if (!strcmp(p, "--peer-stall-ms") && bHasVal)
			s_opt.nPeerStallMs = atoi(argv[++i]);
		else if (!strcmp(p, "--stuck-ms") && bHasVal)
			s_opt.nStuckMs = atoi(argv[++i]);
		else if (!strcmp(p, "--duration-s") && bHasVal)
			s_opt.nDurationS = atoi(argv[++i]);
		else if (!strcmp(p, "--sndbuf-kb") && bHasVal)
			s_opt.nSndBufKB = atoi(argv[++i]);
		else {
			Usage();
			return false;
		}
	}
	if (s_opt.nSlots < 1 || s_opt.nWindow < 1 || s_opt.nBlockKB < 4 || s_opt.nSndBufKB < 1) {
		printf("a value is out of range\n");
		return false;
	}
	return true;
}

int main(int argc, char *argv[])
{
	::InitializeCriticalSection(&s_csLog);

	if (!AfxWinInit(::GetModuleHandle(NULL), NULL, ::GetCommandLine(), 0)) {
		printf("AfxWinInit failed\n");
		return 2;
	}
	if (!ParseArgs(argc, argv))
		return 2;

	WSADATA wsa;
	if (::WSAStartup(MAKEWORD(2, 2), &wsa)) {
		printf("WSAStartup failed\n");
		return 2;
	}

	Log("start: mode=%s port=%u slots=%d window=%d block=%dKB loop=%dms peer-stall=%dms duration=%ds"
		, s_pszModeNames[s_opt.nMode], s_opt.nPort, s_opt.nSlots, s_opt.nWindow
		, s_opt.nBlockKB, s_opt.nLoopMs, s_opt.nPeerStallMs, s_opt.nDurationS);

	s_hTerminate = ::CreateEvent(NULL, TRUE, FALSE, NULL);
	// eMule's m_eventSocketAvailable is an MFC CEvent left at its default,
	// which is auto-reset.
	s_hSocketAvailable = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!s_hTerminate || !s_hSocketAvailable) {
		Log("CreateEvent failed err=%u", ::GetLastError());
		return 2;
	}

	const int nExit = (s_opt.nMode == MODE_CONTRACT) ? RunContract() : RunStress();

	::SetEvent(s_hTerminate);
	::Sleep(300);
	Log("exit=%d", nExit);
	::WSACleanup();
	::DeleteCriticalSection(&s_csLog);
	return nExit;
}
