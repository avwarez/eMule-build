// wsrepro.cpp - a standalone reproducer for the eMule web-interface listener freeze.
//
// WHY THIS EXISTS
// ---------------
// eMule's web interface (srchybrid/WebSocket.cpp) stops answering after some
// hours: connections pile up in the kernel backlog and the process issues no
// accept() at all, while the rest of eMule keeps running. The listening thread
// waits on an auto-reset event armed with WSAEventSelect(FD_ACCEPT) and never
// calls WSAEnumNetworkEvents - so if one FD_ACCEPT edge is ever lost, nothing
// can restore it: the wait never returns, accept() is never called, and only
// accept() re-arms the notification. A closed circle.
//
// What this program cannot answer by reading the source is WHOSE defect that
// is: eMule's pattern, or the Winsock implementation it happens to run on
// (this machine runs it under Wine). So this replicates the listener exactly -
// same constructs, same MFC threading, same call sequence - with nothing else
// around it, and hammers it. If it freezes here, the pattern is at fault on
// this platform and the platform can be told so with a 200-line test case
// instead of a 200,000-line application.
//
// WHAT IT DOES
// ------------
// One listening thread (the specimen, see WsReproListeningFunc), one short
// worker thread per accepted connection (as eMule does), N client threads
// hammering the port, and a monitor that watches for the exact symptom
// observed in the field: connections established by the client but never
// accepted by the server.
//
// When the symptom appears, the monitor performs an autopsy that distinguishes
// the two possible states the field evidence could not separate:
//   - the listening thread is parked in WaitForMultipleObjects and the
//     FD_ACCEPT notification was lost  -> SetEvent alone resumes accepting;
//   - the listening thread is dead or stuck elsewhere -> it does not.
//
// The --mode switch selects which variant of the listener is under test, so
// the same harness measures both the defect and each candidate remedy.

#include <afxwin.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

// MFC console application: AfxWinInit below needs an application object to
// exist, and CWinThread needs MFC's module state, which that object sets up.
CWinApp theApp;

/////////////////////////////////////////////////////////////////////////////
// Options

enum EMode
{
	MODE_EMULE = 0,      // eMule's listener, reproduced call for call
	MODE_ENUM,           // + WSAEnumNetworkEvents after the wait (the documented pattern)
	MODE_CLEARINHERIT,   // + WSAEventSelect(hAccepted, NULL, 0) right after accept()
	MODE_TIMEOUT,        // eMule's listener, but the wait has a timeout instead of INFINITE
	MODE_ASYNCSELECT,    // the OTHER notification path in eMule: WSAAsyncSelect + a message loop
	MODE_POLL            // no notification at all: level-triggered select()
};

static const char *const s_pszModeNames[] = { "emule", "enum", "clearinherit", "timeout", "asyncselect", "poll" };

struct SOptions
{
	int   nMode;
	u_short nPort;
	int   nHammers;      // parallel client threads
	int   nDelayMs;      // per-client pause between connections
	int   nStallMs;      // no accept for this long, with clients waiting -> freeze
	int   nDurationS;    // give up looking for the freeze after this long
	int   nClientWaitMs; // how long a client waits for its reply before giving up
};

static SOptions s_opt = { MODE_EMULE, 4711, 16, 0, 5000, 300, 3000 };

/////////////////////////////////////////////////////////////////////////////
// Shared state

static HANDLE  s_hTerminate = NULL;
static HANDLE  s_hListenEvent = NULL;          // the specimen's event, kept reachable for the autopsy
static SOCKET  s_hListenSocket = INVALID_SOCKET;
static CWinThread *s_pSocketThread = NULL;

static volatile LONG s_nAccepted = 0;          // server side: accept() returned a socket
static volatile LONG s_nConnected = 0;         // client side: connect() returned
static volatile LONG s_nServed = 0;            // client side: a reply came back
static volatile LONG s_nClientTimeout = 0;     // client side: connected, no reply
static volatile LONG s_nClientFailed = 0;      // client side: connect() itself failed
static volatile LONG s_nWaitReturns = 0;       // listener: wakeups from the wait
static volatile LONG s_nDrainEmpty = 0;        // listener: wakeups that accepted nothing
static volatile LONG s_nConnLive = 0;
static volatile LONG s_dwLastAccept = 0;       // GetTickCount of the last accept()
static volatile LONG s_bListening = 0;
static volatile LONG s_nSelectMissed = 0;      // poll mode: select() said "nothing" with clients waiting

static HWND    s_hNotifyWnd = NULL;            // asyncselect mode: the helper window, as CAsyncSocketEx has
#define WM_WSREPRO_NOTIFY (WM_APP + 1)

static CRITICAL_SECTION s_csLog;

static void Log(const char *pszFmt, ...)
{
	char szBuf[1024];
	va_list args;
	va_start(args, pszFmt);
	_vsnprintf_s(szBuf, sizeof szBuf, _TRUNCATE, pszFmt, args);
	va_end(args);
	::EnterCriticalSection(&s_csLog);
	printf("WSREPRO|%8u|%s\n", ::GetTickCount(), szBuf);
	fflush(stdout);
	::LeaveCriticalSection(&s_csLog);
}

/////////////////////////////////////////////////////////////////////////////
// The per-connection worker - eMule's WebSocketAcceptedFunc, reduced to its
// socket handling: same event, same WSAEventSelect mask, same wait loop, same
// WSAEnumNetworkEvents drain. It answers with a fixed HTTP reply and leaves.

struct SocketData
{
	SOCKET hSocket;
	in_addr incomingaddr;
};

static UINT AFX_CDECL WsReproAcceptedFunc(LPVOID pD)
{
	const SocketData *pData = static_cast<SocketData*>(pD);
	SOCKET hSocket = pData->hSocket;
	delete pData;

	::InterlockedIncrement(&s_nConnLive);

	HANDLE hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
	if (hEvent) {
		if (!WSAEventSelect(hSocket, hEvent, FD_READ | FD_CLOSE | FD_WRITE)) {
			HANDLE pWait[] = { hEvent, s_hTerminate };
			bool bValid = true;
			bool bAnswered = false;
			int nHeaderMatch = 0;
			while (bValid && !bAnswered && WAIT_OBJECT_0 == ::WaitForMultipleObjects(DWORD(_countof(pWait)), pWait, FALSE, 10000)) {
				while (bValid) {
					WSANETWORKEVENTS stEvents;
					if (WSAEnumNetworkEvents(hSocket, NULL, &stEvents)) {
						bValid = false;
						break;
					}
					if (!stEvents.lNetworkEvents)
						break;
					if (FD_READ & stEvents.lNetworkEvents)
						for (;;) {
							char pBuf[0x1000];
							int nRes = recv(hSocket, pBuf, sizeof pBuf, 0);
							if (nRes <= 0) {
								if (nRes < 0 && WSAEWOULDBLOCK != WSAGetLastError())
									bValid = false;
								break;
							}
							// end of the request header: "\r\n\r\n"
							for (int i = 0; i < nRes; ++i) {
								const char c = pBuf[i];
								if ((nHeaderMatch % 2 == 0 && c == '\r') || (nHeaderMatch % 2 == 1 && c == '\n'))
									++nHeaderMatch;
								else
									nHeaderMatch = (c == '\r') ? 1 : 0;
								if (nHeaderMatch == 4)
									bAnswered = true;
							}
						}
					if (FD_CLOSE & stEvents.lNetworkEvents)
						bValid = false;
				}
			}
			if (bAnswered) {
				static const char szReply[] =
					"HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
				send(hSocket, szReply, (int)(sizeof szReply - 1), 0);
			}
		}
		VERIFY(::CloseHandle(hEvent));
	}
	shutdown(hSocket, SD_BOTH);
	closesocket(hSocket);
	::InterlockedDecrement(&s_nConnLive);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// The specimen - eMule's WebSocketListeningFunc.
//
// Everything outside the "#mode" guards is the original sequence: WSASocket,
// bind, listen(SOMAXCONN), an AUTO-RESET event created already signalled,
// WSAEventSelect(FD_ACCEPT), and a wait/drain loop that never asks Winsock
// which events actually fired.

// The drain: accept() until WSAEWOULDBLOCK, a worker thread per connection.
// Shared by both listeners below so that the only thing that differs between
// them is how the arrival of a connection is announced.
static int DrainAccepts(SOCKET hSocket)
{
	int nThisRound = 0;
	for (;;) {
		SOCKADDR_IN their_addr;
		int sin_size = (int)sizeof(SOCKADDR_IN);

		SOCKET hAccepted = accept(hSocket, (LPSOCKADDR)&their_addr, &sin_size);
		if (INVALID_SOCKET == hAccepted) {
			const int nErr = ::WSAGetLastError();
			if (nErr != WSAEWOULDBLOCK)
				Log("accept FAILED err=%d", nErr);
			break;
		}
		++nThisRound;
		::InterlockedIncrement(&s_nAccepted);
		::InterlockedExchange(&s_dwLastAccept, (LONG)::GetTickCount());

		if (s_opt.nMode == MODE_CLEARINHERIT) {
			// An accepted socket inherits the listening socket's event
			// association. Break that link here, before the worker thread
			// re-selects the socket onto an event of its own.
			WSAEventSelect(hAccepted, NULL, 0);
			u_long ulNonBlock = 1;
			ioctlsocket(hAccepted, FIONBIO, &ulNonBlock);
		}

		SocketData *pData = new SocketData;
		pData->hSocket = hAccepted;
		pData->incomingaddr = their_addr.sin_addr;
		// same construct as eMule: not CreateThread, not AfxBeginThread
		CWinThread *pAcceptThread = new CWinThread(WsReproAcceptedFunc, (LPVOID)pData);
		if (!pAcceptThread->CreateThread()) {
			Log("CreateThread FAILED err=%u live=%ld", ::GetLastError(), s_nConnLive);
			delete pData;
			delete pAcceptThread;
			closesocket(hAccepted);
		}
	}
	if (!nThisRound)
		::InterlockedIncrement(&s_nDrainEmpty);
	return nThisRound;
}

// The second listener: the notification path every OTHER socket in eMule uses.
// CAsyncSocketEx creates a hidden helper window and calls WSAAsyncSelect, so a
// connection arrives as a window message rather than as a signalled event. The
// question this answers is whether the lost notification is specific to
// WSAEventSelect or common to both paths - which is the difference between one
// intervention point in eMule and a great many.
static LRESULT CALLBACK WsReproWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_WSREPRO_NOTIFY) {
		if (WSAGETSELECTEVENT(lParam) == FD_ACCEPT) {
			::InterlockedIncrement(&s_nWaitReturns);
			if (WSAGETSELECTERROR(lParam))
				Log("FD_ACCEPT message carried err=%d", WSAGETSELECTERROR(lParam));
			else
				DrainAccepts((SOCKET)wParam);
		}
		return 0;
	}
	return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static void RunAsyncSelectListener(SOCKET hSocket)
{
	WNDCLASS wc = {};
	wc.lpfnWndProc = WsReproWndProc;
	wc.hInstance = ::GetModuleHandle(NULL);
	wc.lpszClassName = _T("WsReproNotifyWnd");
	if (!::RegisterClass(&wc)) {
		Log("RegisterClass FAILED err=%u", ::GetLastError());
		return;
	}
	HWND hWnd = ::CreateWindowEx(0, _T("WsReproNotifyWnd"), NULL, 0, 0, 0, 0, 0
		, HWND_MESSAGE, NULL, ::GetModuleHandle(NULL), NULL);
	if (!hWnd) {
		Log("CreateWindowEx FAILED err=%u", ::GetLastError());
		return;
	}
	s_hNotifyWnd = hWnd;

	if (WSAAsyncSelect(hSocket, hWnd, WM_WSREPRO_NOTIFY, FD_ACCEPT)) {
		Log("WSAAsyncSelect FAILED err=%d", ::WSAGetLastError());
		::DestroyWindow(hWnd);
		s_hNotifyWnd = NULL;
		return;
	}

	::InterlockedExchange(&s_dwLastAccept, (LONG)::GetTickCount());
	::InterlockedExchange(&s_bListening, 1);
	Log("listener ready on port %u, mode=asyncselect (WSAAsyncSelect + message loop)", s_opt.nPort);

	// A message pump that can also be woken by the terminate event, which is
	// what a GUI thread's wait looks like.
	for (;;) {
		const DWORD dwRes = ::MsgWaitForMultipleObjects(1, &s_hTerminate, FALSE, INFINITE, QS_ALLINPUT);
		if (dwRes == WAIT_OBJECT_0)
			break;
		if (dwRes != WAIT_OBJECT_0 + 1)
			break;
		MSG msg;
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}
	Log("listener loop END (asyncselect)");
	s_hNotifyWnd = NULL;
	::DestroyWindow(hWnd);
}

// The third listener: no notification mechanism at all. select() is
// LEVEL-triggered - it answers "is there a connection waiting right now",
// computed from the socket's current state, not from an edge delivered once.
// There is nothing to lose and nothing to re-arm, so the failure mode that
// kills the other two cannot exist here by construction.
//
// The 1-second timeout is not what makes it work, and the harness proves that
// rather than assuming it: every time select() reports nothing while clients
// are known to be waiting, s_nSelectMissed counts it. If that number stays at
// zero, the timeout never had to rescue anything.
static void RunPollListener(SOCKET hSocket)
{
	u_long ulNonBlock = 1;
	if (ioctlsocket(hSocket, FIONBIO, &ulNonBlock)) {
		Log("ioctlsocket(FIONBIO) FAILED err=%d", ::WSAGetLastError());
		return;
	}

	::InterlockedExchange(&s_dwLastAccept, (LONG)::GetTickCount());
	::InterlockedExchange(&s_bListening, 1);
	Log("listener ready on port %u, mode=poll (level-triggered select)", s_opt.nPort);

	while (::WaitForSingleObject(s_hTerminate, 0) == WAIT_TIMEOUT) {
		fd_set stRead;
		FD_ZERO(&stRead);
		FD_SET(hSocket, &stRead);
		timeval tv = { 1, 0 };

		const int nRes = select(0, &stRead, NULL, NULL, &tv);
		if (nRes > 0) {
			::InterlockedIncrement(&s_nWaitReturns);
			DrainAccepts(hSocket);
		} else if (nRes == 0) {
			if (s_nConnected > s_nAccepted)
				::InterlockedIncrement(&s_nSelectMissed);
		} else {
			Log("select FAILED err=%d", ::WSAGetLastError());
			break;
		}
	}
	Log("listener loop END (poll), select said nothing with clients waiting %ld times", s_nSelectMissed);
}

static UINT AFX_CDECL WsReproListeningFunc(LPVOID)
{
	SOCKET hSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
	if (INVALID_SOCKET == hSocket) {
		Log("listener WSASocket FAILED err=%d", ::WSAGetLastError());
		return 1;
	}
	s_hListenSocket = hSocket;

	SOCKADDR_IN stAddr;
	stAddr.sin_family = AF_INET;
	stAddr.sin_port = htons(s_opt.nPort);
	stAddr.sin_addr.s_addr = INADDR_ANY;

	if (bind(hSocket, (LPSOCKADDR)&stAddr, sizeof stAddr) || listen(hSocket, SOMAXCONN)) {
		Log("listener bind/listen FAILED err=%d port=%u", ::WSAGetLastError(), s_opt.nPort);
		closesocket(hSocket);
		return 1;
	}

	if (s_opt.nMode == MODE_ASYNCSELECT || s_opt.nMode == MODE_POLL) {
		if (s_opt.nMode == MODE_POLL)
			RunPollListener(hSocket);
		else
			RunAsyncSelectListener(hSocket);
		::InterlockedExchange(&s_bListening, 0);
		closesocket(hSocket);
		s_hListenSocket = INVALID_SOCKET;
		return 0;
	}

	HANDLE hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
	if (hEvent) {
		s_hListenEvent = hEvent;
		if (!WSAEventSelect(hSocket, hEvent, FD_ACCEPT)) {
			HANDLE pWait[] = { hEvent, s_hTerminate };
			const DWORD dwWaitMs = (s_opt.nMode == MODE_TIMEOUT) ? 1000 : INFINITE;
			DWORD dwRes;

			::InterlockedExchange(&s_dwLastAccept, (LONG)::GetTickCount());
			::InterlockedExchange(&s_bListening, 1);
			Log("listener ready on port %u, mode=%s, wait=%s"
				, s_opt.nPort, s_pszModeNames[s_opt.nMode]
				, (dwWaitMs == INFINITE) ? "INFINITE" : "1000ms");

			for (;;) {
				dwRes = ::WaitForMultipleObjects(DWORD(_countof(pWait)), pWait, FALSE, dwWaitMs);
				if (dwRes == WAIT_OBJECT_0 + 1)
					break;                                   // terminate
				if (dwRes != WAIT_OBJECT_0 && dwRes != WAIT_TIMEOUT)
					break;                                   // failed
				if (dwRes == WAIT_TIMEOUT && s_opt.nMode != MODE_TIMEOUT)
					break;                                   // cannot happen with INFINITE
				::InterlockedIncrement(&s_nWaitReturns);

				if (s_opt.nMode == MODE_ENUM) {
					// The documented pattern, and the one eMule itself uses for
					// connections thirty lines higher up in the same file: ask
					// which events fired, which is also what re-syncs the event.
					WSANETWORKEVENTS stEvents;
					if (WSAEnumNetworkEvents(hSocket, NULL, &stEvents))
						break;
					if (!(stEvents.lNetworkEvents & FD_ACCEPT))
						continue;
					if (stEvents.iErrorCode[FD_ACCEPT_BIT])
						Log("listener FD_ACCEPT carried err=%d", stEvents.iErrorCode[FD_ACCEPT_BIT]);
				}

				DrainAccepts(hSocket);
			}
			Log("listener loop END, wait=%u err=%u", dwRes, ::GetLastError());
		} else
			Log("listener WSAEventSelect FAILED err=%d", ::WSAGetLastError());
		s_hListenEvent = NULL;
		VERIFY(::CloseHandle(hEvent));
	}
	::InterlockedExchange(&s_bListening, 0);
	closesocket(hSocket);
	s_hListenSocket = INVALID_SOCKET;
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// The hammer - one client thread, connecting in a loop

static UINT AFX_CDECL WsReproHammerFunc(LPVOID)
{
	while (::WaitForSingleObject(s_hTerminate, 0) == WAIT_TIMEOUT) {
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (INVALID_SOCKET == s) {
			::InterlockedIncrement(&s_nClientFailed);
			::Sleep(50);
			continue;
		}
		SOCKADDR_IN sa;
		sa.sin_family = AF_INET;
		sa.sin_port = htons(s_opt.nPort);
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (connect(s, (LPSOCKADDR)&sa, sizeof sa)) {
			::InterlockedIncrement(&s_nClientFailed);
			closesocket(s);
			::Sleep(10);
			continue;
		}
		::InterlockedIncrement(&s_nConnected);

		DWORD dwTimeout = (DWORD)s_opt.nClientWaitMs;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&dwTimeout, sizeof dwTimeout);

		static const char szReq[] = "GET / HTTP/1.0\r\nHost: wsrepro\r\n\r\n";
		send(s, szReq, (int)(sizeof szReq - 1), 0);

		char szBuf[256];
		const int nRes = recv(s, szBuf, sizeof szBuf, 0);
		if (nRes > 0)
			::InterlockedIncrement(&s_nServed);
		else
			::InterlockedIncrement(&s_nClientTimeout);

		closesocket(s);
		if (s_opt.nDelayMs)
			::Sleep((DWORD)s_opt.nDelayMs);
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Autopsy - run once, only after the symptom has already appeared, so nothing
// here can perturb the run that produced it.

static bool AcceptsResume(DWORD dwMs)
{
	const LONG nBefore = s_nAccepted;
	const DWORD dwStart = ::GetTickCount();
	while (::GetTickCount() - dwStart < dwMs) {
		if (s_nAccepted != nBefore)
			return true;
		::Sleep(50);
	}
	return false;
}

static void Autopsy()
{
	Log("--- autopsy ---");

	const DWORD dwThread = (s_pSocketThread && s_pSocketThread->m_hThread)
		? ::WaitForSingleObject(s_pSocketThread->m_hThread, 0) : WAIT_FAILED;
	Log("listening thread: %s (WaitForSingleObject=%u)"
		, (dwThread == WAIT_TIMEOUT) ? "ALIVE" : (dwThread == WAIT_OBJECT_0 ? "DEAD" : "UNKNOWN")
		, dwThread);

	if (dwThread == WAIT_OBJECT_0) {
		Log("VERDICT: the listening thread exited - this is not the lost-notification shape");
		return;
	}

	if (s_opt.nMode == MODE_ASYNCSELECT) {
		// The equivalent kick for the message path - and not a hypothetical one:
		// this is what eMule's CListenSocket::ReStartListening() already does when
		// it calls OnAccept(0) by hand.
		Log("probe: post the FD_ACCEPT notification by hand");
		if (s_hNotifyWnd
			&& ::PostMessage(s_hNotifyWnd, WM_WSREPRO_NOTIFY, (WPARAM)s_hListenSocket, (LPARAM)MAKELONG(FD_ACCEPT, 0))
			&& AcceptsResume(3000))
		{
			Log("VERDICT: LOST NOTIFICATION on the WSAAsyncSelect path too. The message");
			Log("VERDICT: announcing the connection was never posted; posting it by hand");
			Log("VERDICT: emptied the backlog.");
			return;
		}
		Log("VERDICT: the message loop is running but the hand-posted notification did");
		Log("VERDICT: not resume accepting - it is blocked somewhere else.");
		return;
	}

	Log("probe 1: SetEvent on the listener's own event");
	if (s_hListenEvent && ::SetEvent(s_hListenEvent) && AcceptsResume(3000)) {
		Log("VERDICT: LOST NOTIFICATION. The thread was parked in the wait with a full");
		Log("VERDICT: backlog; one SetEvent resumed accepting. The FD_ACCEPT edge that");
		Log("VERDICT: should have signalled the event was never delivered.");
		return;
	}

	Log("probe 2: re-arm with WSAEventSelect(FD_ACCEPT), then SetEvent");
	if (s_hListenSocket != INVALID_SOCKET && s_hListenEvent
		&& !WSAEventSelect(s_hListenSocket, s_hListenEvent, FD_ACCEPT)
		&& ::SetEvent(s_hListenEvent) && AcceptsResume(3000))
	{
		Log("VERDICT: LOST NOTIFICATION, and the event alone was not enough - the");
		Log("VERDICT: FD_ACCEPT interest itself had to be re-armed on the socket.");
		return;
	}

	Log("VERDICT: the listening thread is alive but neither probe resumed accepting -");
	Log("VERDICT: it is blocked somewhere other than the wait, or accept() itself refuses.");
}

/////////////////////////////////////////////////////////////////////////////

static void Usage()
{
	printf(
		"wsrepro - reproducer for the eMule web-interface listener freeze\n"
		"\n"
		"  --mode <emule|enum|clearinherit|timeout|asyncselect|poll>  listener variant (default emule)\n"
		"  --port <n>          listening port (default 4711)\n"
		"  --hammers <n>       parallel client threads (default 16)\n"
		"  --delay-ms <n>      pause between a client's connections (default 0)\n"
		"  --stall-ms <n>      no accept for this long, with clients waiting -> freeze (default 5000)\n"
		"  --duration-s <n>    stop looking for the freeze after this long (default 300)\n"
		"  --client-wait-ms <n>  how long a client waits for its reply (default 3000)\n"
		"\n"
		"Exit code: 0 = no freeze observed, 1 = freeze observed, 2 = could not start\n");
}

static bool ParseArgs(int argc, char *argv[])
{
	for (int i = 1; i < argc; ++i) {
		const char *p = argv[i];
		const bool bHasVal = (i + 1 < argc);
		if (!strcmp(p, "--help") || !strcmp(p, "-h")) {
			Usage();
			return false;
		}
		if (!strcmp(p, "--mode") && bHasVal) {
			const char *v = argv[++i];
			int j = 0;
			for (; j < (int)_countof(s_pszModeNames); ++j)
				if (!strcmp(v, s_pszModeNames[j])) {
					s_opt.nMode = j;
					break;
				}
			if (j == (int)_countof(s_pszModeNames)) {
				printf("unknown mode: %s\n", v);
				return false;
			}
		} else if (!strcmp(p, "--port") && bHasVal)
			s_opt.nPort = (u_short)atoi(argv[++i]);
		else if (!strcmp(p, "--hammers") && bHasVal)
			s_opt.nHammers = atoi(argv[++i]);
		else if (!strcmp(p, "--delay-ms") && bHasVal)
			s_opt.nDelayMs = atoi(argv[++i]);
		else if (!strcmp(p, "--stall-ms") && bHasVal)
			s_opt.nStallMs = atoi(argv[++i]);
		else if (!strcmp(p, "--duration-s") && bHasVal)
			s_opt.nDurationS = atoi(argv[++i]);
		else if (!strcmp(p, "--client-wait-ms") && bHasVal)
			s_opt.nClientWaitMs = atoi(argv[++i]);
		else {
			printf("unknown argument: %s\n", p);
			Usage();
			return false;
		}
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
	if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
		printf("WSAStartup failed\n");
		return 2;
	}

	Log("start: mode=%s port=%u hammers=%d delay=%dms stall=%dms duration=%ds"
		, s_pszModeNames[s_opt.nMode], s_opt.nPort, s_opt.nHammers
		, s_opt.nDelayMs, s_opt.nStallMs, s_opt.nDurationS);

	s_hTerminate = CreateEvent(NULL, TRUE, FALSE, NULL);   // manual reset, as eMule's
	if (!s_hTerminate) {
		Log("CreateEvent failed err=%u", ::GetLastError());
		return 2;
	}

	// same construct as eMule's StartSockets: a CWinThread whose handle stays
	// valid after the thread ends, so it can be asked whether it is still alive
	s_pSocketThread = new CWinThread(WsReproListeningFunc, NULL);
	s_pSocketThread->m_bAutoDelete = FALSE;
	if (!s_pSocketThread->CreateThread()) {
		Log("listener CreateThread failed err=%u", ::GetLastError());
		return 2;
	}

	for (int i = 0; i < 200 && !s_bListening; ++i)
		::Sleep(25);
	if (!s_bListening) {
		Log("listener did not come up");
		::SetEvent(s_hTerminate);
		return 2;
	}

	for (int i = 0; i < s_opt.nHammers; ++i) {
		CWinThread *pT = new CWinThread(WsReproHammerFunc, NULL);
		if (!pT->CreateThread()) {
			Log("hammer %d CreateThread failed err=%u", i, ::GetLastError());
			delete pT;
		}
	}

	const DWORD dwStart = ::GetTickCount();
	DWORD dwLastReport = dwStart;
	bool bFrozen = false;

	while (::GetTickCount() - dwStart < (DWORD)s_opt.nDurationS * 1000) {
		::Sleep(200);

		const DWORD dwNow = ::GetTickCount();
		const LONG nAccepted = s_nAccepted;
		const LONG nConnected = s_nConnected;
		const DWORD dwSinceAccept = dwNow - (DWORD)s_dwLastAccept;

		// The field symptom, exactly: the client's connect() succeeds because the
		// kernel completes the handshake into the backlog, and the server never
		// takes it out of there.
		if (dwSinceAccept > (DWORD)s_opt.nStallMs && nConnected > nAccepted && s_nClientTimeout > 0) {
			Log("FREEZE: no accept for %ums; connected=%ld accepted=%ld (%ld waiting in the backlog)"
				, dwSinceAccept, nConnected, nAccepted, nConnected - nAccepted);
			bFrozen = true;
			break;
		}

		if (dwNow - dwLastReport >= 5000) {
			dwLastReport = dwNow;
			Log("accepted=%ld connected=%ld served=%ld timeout=%ld cfail=%ld wakeups=%ld empty=%ld live=%ld"
				, nAccepted, nConnected, s_nServed, s_nClientTimeout, s_nClientFailed
				, s_nWaitReturns, s_nDrainEmpty, s_nConnLive);
		}
	}

	Log("accepted=%ld connected=%ld served=%ld timeout=%ld cfail=%ld wakeups=%ld empty=%ld live=%ld"
		, s_nAccepted, s_nConnected, s_nServed, s_nClientTimeout, s_nClientFailed
		, s_nWaitReturns, s_nDrainEmpty, s_nConnLive);

	int nExit = 0;
	if (bFrozen) {
		Autopsy();
		nExit = 1;
	} else
		// accepted/wakeups are part of the verdict on purpose: a run that could not
		// get connections through (client-side port exhaustion, say) also reports "no
		// freeze", and the only thing separating it from a real clean run is how many
		// arm/drain cycles the listener actually went through.
		Log("VERDICT: no freeze in %ds with mode=%s (%ld accepted, %ld wait cycles, %ld empty drains, %ld failed connects, %ld missed readiness)"
			, s_opt.nDurationS, s_pszModeNames[s_opt.nMode], s_nAccepted, s_nWaitReturns, s_nDrainEmpty, s_nClientFailed, s_nSelectMissed);

	::SetEvent(s_hTerminate);
	::Sleep(500);
	if (s_pSocketThread && s_pSocketThread->m_hThread)
		::WaitForSingleObject(s_pSocketThread->m_hThread, 2000);

	WSACleanup();
	::DeleteCriticalSection(&s_csLog);
	return nExit;
}
