// dnsrepro.cpp - a standalone specimen for eMule's asynchronous name
// resolution and its two UDP sockets.
//
// WHY THIS EXISTS
// ---------------
// The sixth and last specimen of the dual bench, and it closes the one
// suspicion left on the map: eMule resolves server names with
//
//     pDNSReq->m_hDNSTask = WSAAsyncGetHostByName(m_hWndResolveMessage
//         , WM_DNSLOOKUPDONE, pszHostAddressA
//         , pDNSReq->m_DnsHostBuffer, sizeof pDNSReq->m_DnsHostBuffer);
//                                                        UDPSocket.cpp:794
//
// The answer does not come back from the call. It comes back as a WINDOW
// MESSAGE, posted to a window of eMule's, carrying the error in the high word
// of lParam - the same notification machinery that lost FD_ACCEPT edges under
// Wine (see wsrepro/). And the answer is written into a buffer that belongs to
// the request object, whose destructor reads:
//
//     ~SServerDNSRequest()
//     {
//         if (m_hDNSTask)
//             WSACancelAsyncRequest(m_hDNSTask);
//         ...                                            UDPSocket.cpp:84
//
// so the buffer goes away as soon as the cancel returns. If a cancelled request
// can still be written to - as the overlapped send in ovrepro/ turned out to be
// - that is a write into freed memory, and it arrives from the network.
//
// The two UDP sockets are the other half: SendTo and ReceiveFrom, with
// eMule's receive loop treating a zero-length datagram as nothing to do and a
// SOCKET_ERROR as a logged failure it carries on from.
//
// WHAT IT MEASURES
// ---------------
//   --mode contract  a fixed table of deterministic probes: the shape of the
//                    answer, what a too-small buffer does, what a cancel does,
//                    datagram boundaries, truncation, and what a datagram sent
//                    to nobody does to the next receive. Timestamp-free PROBE
//                    lines for the workflow to DIFF between Windows and Wine.
//   --mode stress    many outstanding resolutions at once, over and over,
//                    counting the answers. A message that never arrives leaves
//                    eMule's request on a list for good, and this is the shape
//                    that has already failed once on this platform.
//
// "localhost" is used throughout the stress mode on purpose: it is answered
// from the hosts file, so what is being measured is the delivery of the answer
// and not the speed of somebody's resolver.

#include <afxwin.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

CWinApp theApp;

// UDPSocket.cpp uses WM_DNSLOOKUPDONE; any private message does the same job.
#define WM_DNSDONE (WM_USER + 0x100)

/////////////////////////////////////////////////////////////////////////////
// Options

enum EMode
{
	MODE_CONTRACT = 0,
	MODE_STRESS
};

static const char *const s_pszModeNames[] = { "contract", "stress" };

struct SOptions
{
	int nMode;
	int nConcurrent;   // resolutions outstanding at once
	int nRounds;       // stress: how many times that many
	int nWaitMs;       // how long an answer is waited for
	u_short nPort;     // UDP probes
};

static SOptions s_opt = { MODE_CONTRACT, 8, 200, 5000, 4796 };

/////////////////////////////////////////////////////////////////////////////
// Output

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
// The request, laid out as eMule's is: the buffer the answer is written into
// belongs to the object, and the object is deleted after the cancel.

#define MAX_REQUESTS 64

struct DnsRequest
{
	HANDLE hTask;
	char   szBuffer[MAXGETHOSTSTRUCT];
	int    nError;
	int    nBufLen;
	bool   bAnswered;
};

static DnsRequest s_aReqs[MAX_REQUESTS];
static int        s_nReqs = 0;
static HWND       s_hWnd = NULL;
static volatile LONG s_nAnswers = 0;

static LRESULT CALLBACK DnsWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_DNSDONE) {
		for (int i = 0; i < s_nReqs; ++i)
			if (s_aReqs[i].hTask == (HANDLE)wParam) {
				s_aReqs[i].nError = WSAGETASYNCERROR(lParam);
				s_aReqs[i].nBufLen = WSAGETASYNCBUFLEN(lParam);
				s_aReqs[i].bAnswered = true;
				::InterlockedIncrement(&s_nAnswers);
				break;
			}
		return 0;
	}
	return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static bool CreateMessageWindow()
{
	WNDCLASS wc;
	::ZeroMemory(&wc, sizeof wc);
	wc.lpfnWndProc = DnsWndProc;
	wc.hInstance = ::GetModuleHandle(NULL);
	wc.lpszClassName = _T("DnsReproWindow");
	if (!::RegisterClass(&wc))
		return false;
	// Message-only, as the helper windows in CAsyncSocketEx are.
	s_hWnd = ::CreateWindowEx(0, _T("DnsReproWindow"), NULL, 0, 0, 0, 0, 0
		, HWND_MESSAGE, NULL, ::GetModuleHandle(NULL), NULL);
	return s_hWnd != NULL;
}

// eMule's message loop is the application's; here it is this, and it must keep
// running for an answer to be able to arrive at all.
static void PumpFor(DWORD dwMs)
{
	const DWORD dwEnd = ::GetTickCount() + dwMs;
	for (;;) {
		MSG msg;
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
		if ((LONG)(dwEnd - ::GetTickCount()) <= 0)
			break;
		::Sleep(1);
	}
}

// Pumps until every outstanding request has been answered, or time runs out.
// Returns how many were answered.
static int PumpUntilAnswered(int nExpected, DWORD dwMs)
{
	const DWORD dwEnd = ::GetTickCount() + dwMs;
	for (;;) {
		MSG msg;
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
		int nDone = 0;
		for (int i = 0; i < s_nReqs; ++i)
			nDone += (int)s_aReqs[i].bAnswered;
		if (nDone >= nExpected || (LONG)(dwEnd - ::GetTickCount()) <= 0)
			return nDone;
		::Sleep(1);
	}
}

static void ResetRequests()
{
	s_nReqs = 0;
	::ZeroMemory(s_aReqs, sizeof s_aReqs);
}

// Starts one, exactly as UDPSocket.cpp:794 does.
static int StartResolve(const char *pszName, int nBufBytes)
{
	if (s_nReqs >= MAX_REQUESTS)
		return -1;
	DnsRequest &r = s_aReqs[s_nReqs];
	::memset(r.szBuffer, 0xCC, sizeof r.szBuffer);   // poisoned, so a late write shows
	r.bAnswered = false;
	r.nError = -1;
	r.nBufLen = 0;
	r.hTask = ::WSAAsyncGetHostByName(s_hWnd, WM_DNSDONE, pszName, r.szBuffer
		, nBufBytes > 0 ? nBufBytes : (int)sizeof r.szBuffer);
	return r.hTask ? s_nReqs++ : -1;
}

// Is the answer in the buffer the thing eMule then reads - a hostent whose
// internal pointers point inside that same buffer?
static bool HostentLooksRight(const DnsRequest &r, bool &rbLoopback)
{
	rbLoopback = false;
	const hostent *pHost = (const hostent*)r.szBuffer;
	if (pHost->h_addrtype != AF_INET || pHost->h_length != 4)
		return false;
	if (pHost->h_addr_list == NULL || pHost->h_addr_list[0] == NULL)
		return false;
	// Every pointer must land inside the caller's buffer: that is the whole
	// contract of this call, and the reason the buffer has to outlive it.
	const char *pBase = r.szBuffer;
	const char *pEnd = r.szBuffer + sizeof r.szBuffer;
	const char *pList = (const char*)pHost->h_addr_list;
	const char *pAddr = pHost->h_addr_list[0];
	if (pList < pBase || pList >= pEnd || pAddr < pBase || pAddr >= pEnd)
		return false;
	ULONG uAddr;
	::memcpy(&uAddr, pAddr, 4);
	rbLoopback = (uAddr == htonl(INADDR_LOOPBACK));
	return true;
}

/////////////////////////////////////////////////////////////////////////////
// UDP helpers: the two sockets eMule listens on, reduced to a pair

static SOCKET MakeUdp(u_short nPort)
{
	SOCKET h = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (h == INVALID_SOCKET)
		return INVALID_SOCKET;
	SOCKADDR_IN stAddr;
	::ZeroMemory(&stAddr, sizeof stAddr);
	stAddr.sin_family = AF_INET;
	stAddr.sin_port = htons(nPort);
	stAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (nPort && bind(h, (LPSOCKADDR)&stAddr, sizeof stAddr)) {
		closesocket(h);
		return INVALID_SOCKET;
	}
	u_long ulNonBlock = 1;
	ioctlsocket(h, FIONBIO, &ulNonBlock);
	return h;
}

// Waits for a datagram the way eMule's OnReceive is entered: with something to
// read. Returns what ReceiveFrom would return, and the error beside it.
static int RecvFromWait(SOCKET h, char *pBuf, int nLen, SOCKADDR_IN *pFrom, int *pErr, DWORD dwMs)
{
	const DWORD dwEnd = ::GetTickCount() + dwMs;
	for (;;) {
		int nFromLen = (int)sizeof(SOCKADDR_IN);
		::SetLastError(0);
		::WSASetLastError(0);
		const int nRes = recvfrom(h, pBuf, nLen, 0, (LPSOCKADDR)pFrom, &nFromLen);
		if (nRes >= 0) {
			*pErr = 0;
			return nRes;
		}
		*pErr = ::WSAGetLastError();
		if (*pErr != WSAEWOULDBLOCK)
			return SOCKET_ERROR;
		if ((LONG)(dwEnd - ::GetTickCount()) <= 0)
			return SOCKET_ERROR;
		::Sleep(1);
	}
}

/////////////////////////////////////////////////////////////////////////////
// Mode "contract"

static int RunContract()
{
	int nExit = 0;

	// 1. The ordinary case, and the shape of what comes back. eMule reads the
	//    buffer as a hostent whose pointers point back into it.
	{
		ResetRequests();
		const int nIdx = StartResolve("localhost", 0);
		const int nDone = (nIdx >= 0) ? PumpUntilAnswered(1, s_opt.nWaitMs) : 0;
		bool bLoopback = false;
		const bool bShape = (nDone == 1 && s_aReqs[0].nError == 0) && HostentLooksRight(s_aReqs[0], bLoopback);
		Probe("dns-localhost", "started=%d answered=%d err=%d shape=%d loopback=%d"
			, nIdx >= 0, nDone, nDone ? s_aReqs[0].nError : -1, bShape, bLoopback);
		if (nIdx >= 0 && nDone != 1)
			nExit = 1;
	}

	// 2. A buffer too small for the answer. The size that WOULD have been
	//    needed comes back in the same message, and eMule's buffer is fixed at
	//    MAXGETHOSTSTRUCT - so this is the path it takes if that is ever not
	//    enough.
	{
		ResetRequests();
		const int nIdx = StartResolve("localhost", 16);
		const int nDone = (nIdx >= 0) ? PumpUntilAnswered(1, s_opt.nWaitMs) : 0;
		Probe("dns-small-buffer", "answered=%d err=%s needed=%s"
			, nDone
			, (nDone && s_aReqs[0].nError == WSAENOBUFS) ? "enobufs"
				: (nDone ? "other" : "none")
			, (nDone && s_aReqs[0].nBufLen > 16) ? "reported" : "no");
	}

	// 3. A name that cannot exist. .invalid is reserved for exactly this.
	{
		ResetRequests();
		const int nIdx = StartResolve("this-name-does-not-exist.invalid", 0);
		const int nDone = (nIdx >= 0) ? PumpUntilAnswered(1, s_opt.nWaitMs) : 0;
		Probe("dns-not-found", "started=%d answered=%d err=%s"
			, nIdx >= 0, nDone
			, (nDone && s_aReqs[0].nError == WSAHOST_NOT_FOUND) ? "host-not-found"
				: (nDone ? "other" : "none"));
	}

	// 4. The one that matters. eMule cancels a pending resolution in the
	//    destructor of the object that owns the answer buffer, and then frees
	//    it. So: does the cancel take, does a message still arrive afterwards,
	//    and - the question with teeth - is the buffer still written to?
	{
		ResetRequests();
		const int nIdx = StartResolve("this-name-does-not-exist-either.invalid", 0);
		int nCancel = -1;
		bool bLate = false, bTouched = false;
		if (nIdx >= 0) {
			nCancel = ::WSACancelAsyncRequest(s_aReqs[0].hTask);
			// Give the answer every chance to turn up after the cancel.
			PumpFor(3000);
			bLate = s_aReqs[0].bAnswered;
			for (size_t i = 0; i < sizeof s_aReqs[0].szBuffer; ++i)
				if ((BYTE)s_aReqs[0].szBuffer[i] != 0xCC) {
					bTouched = true;
					break;
				}
		}
		Probe("dns-cancel", "cancelled=%s late-message=%d buffer-written=%d"
			, nCancel == 0 ? "yes" : (nCancel < 0 ? "not-started" : "NO")
			, bLate, bTouched);
		if (bTouched)
			Log("note: the answer buffer was written after the request was cancelled");
	}

	// 4b. The same question asked properly. The probe above cancels the lookup
	//     of a name that does not resolve, so there was never an answer to be
	//     written and "the buffer was not touched" proves very little. This one
	//     cancels a lookup that DOES resolve, re-poisons the buffer AFTER the
	//     cancel has returned, and repeats it enough times to catch the race
	//     from both sides. A byte that changes after that is a write into
	//     memory eMule has already freed.
	{
		int nLate = 0, nWritten = 0, nTries = 0;
		for (int i = 0; i < 50; ++i) {
			ResetRequests();
			if (StartResolve("localhost", 0) < 0)
				continue;
			++nTries;
			::WSACancelAsyncRequest(s_aReqs[0].hTask);
			::memset(s_aReqs[0].szBuffer, 0xCC, sizeof s_aReqs[0].szBuffer);
			PumpFor(50);
			if (s_aReqs[0].bAnswered)
				++nLate;
			for (size_t j = 0; j < sizeof s_aReqs[0].szBuffer; ++j)
				if ((BYTE)s_aReqs[0].szBuffer[j] != 0xCC) {
					++nWritten;
					break;
				}
		}
		Probe("dns-cancel-race", "late-messages=%s written-after-cancel=%s"
			, nLate == 0 ? "none" : (nLate == nTries ? "all" : "some")
			, nWritten ? "YES" : "no");
		Log("note: dns-cancel-race - %d tries, %d late messages, %d buffers written after the cancel"
			, nTries, nLate, nWritten);
		if (nWritten)
			nExit = 1;
	}

	// 5. Many at once - the shape that failed on this platform before. Every
	//    one of them must produce exactly one message.
	{
		ResetRequests();
		int nStarted = 0;
		for (int i = 0; i < s_opt.nConcurrent; ++i)
			if (StartResolve("localhost", 0) >= 0)
				++nStarted;
		const int nDone = PumpUntilAnswered(nStarted, s_opt.nWaitMs);
		Probe("dns-many", "started=%d answered=%s", nStarted, nDone == nStarted ? "all" : "MISSING");
		if (nDone != nStarted)
			nExit = 1;
	}

	// 6. The synchronous form, which ServerConnect.cpp still uses.
	{
		const hostent *pHost = gethostbyname("localhost");
		bool bOk = false;
		if (pHost && pHost->h_addrtype == AF_INET && pHost->h_length == 4
			&& pHost->h_addr_list && pHost->h_addr_list[0])
		{
			ULONG uAddr;
			::memcpy(&uAddr, pHost->h_addr_list[0], 4);
			bOk = (uAddr == htonl(INADDR_LOOPBACK));
		}
		Probe("gethostbyname", "loopback=%d", bOk);
	}

	// 7. And the modern one, used by the socket layer.
	{
		ADDRINFOA hints;
		::ZeroMemory(&hints, sizeof hints);
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		PADDRINFOA pRes = NULL;
		const int nRet = getaddrinfo("localhost", NULL, &hints, &pRes);
		bool bLoopback = false;
		int nCount = 0;
		for (PADDRINFOA p = pRes; p; p = p->ai_next) {
			++nCount;
			if (p->ai_family == AF_INET && p->ai_addr
				&& ((SOCKADDR_IN*)p->ai_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
				bLoopback = true;
		}
		if (pRes)
			freeaddrinfo(pRes);
		Probe("getaddrinfo", "ok=%d any=%d loopback=%d", nRet == 0, nCount > 0, bLoopback);
	}

	// 8. The two UDP sockets: a datagram out and the same datagram in, with the
	//    sender's address on it.
	{
		SOCKET hA = MakeUdp(s_opt.nPort);
		SOCKET hB = MakeUdp((u_short)(s_opt.nPort + 1));
		if (hA != INVALID_SOCKET && hB != INVALID_SOCKET) {
			SOCKADDR_IN stTo;
			::ZeroMemory(&stTo, sizeof stTo);
			stTo.sin_family = AF_INET;
			stTo.sin_port = htons(s_opt.nPort);
			stTo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			char szOut[1400];
			::memset(szOut, 'Q', sizeof szOut);
			const int nSent = sendto(hB, szOut, (int)sizeof szOut, 0, (LPSOCKADDR)&stTo, sizeof stTo);

			char szIn[5000];
			SOCKADDR_IN stFrom;
			::ZeroMemory(&stFrom, sizeof stFrom);
			int nErr = 0;
			const int nGot = RecvFromWait(hA, szIn, (int)sizeof szIn, &stFrom, &nErr, 2000);
			Probe("udp-roundtrip", "sent=%s received=%s from=%s"
				, nSent == (int)sizeof szOut ? "full" : "WRONG"
				, nGot == (int)sizeof szOut ? "full" : (nGot == SOCKET_ERROR ? "NONE" : "WRONG")
				, (nGot > 0 && stFrom.sin_port == htons((u_short)(s_opt.nPort + 1))) ? "right-port" : "WRONG");
			if (nGot != (int)sizeof szOut)
				nExit = 1;

			// A datagram bigger than the buffer offered for it. eMule's own
			// buffer is 5000 bytes and it treats SOCKET_ERROR as a failure to
			// log and carry on from.
			const int nSent2 = sendto(hB, szOut, 1400, 0, (LPSOCKADDR)&stTo, sizeof stTo);
			char szSmall[500];
			int nErr2 = 0;
			const int nGot2 = RecvFromWait(hA, szSmall, (int)sizeof szSmall, &stFrom, &nErr2, 2000);
			Probe("udp-truncated", "sent=%d result=%s err=%s"
				, nSent2 == 1400
				, nGot2 == SOCKET_ERROR ? "error" : (nGot2 == 500 ? "truncated-ok" : "other")
				, nGot2 == SOCKET_ERROR ? (nErr2 == WSAEMSGSIZE ? "msgsize" : "other") : "none");

			// A datagram with nothing in it: eMule reads length 0 and returns.
			const int nSent3 = sendto(hB, szOut, 0, 0, (LPSOCKADDR)&stTo, sizeof stTo);
			int nErr3 = 0;
			const int nGot3 = RecvFromWait(hA, szIn, (int)sizeof szIn, &stFrom, &nErr3, 2000);
			Probe("udp-empty", "sent=%d received=%s", nSent3 == 0
				, nGot3 == 0 ? "zero-length" : (nGot3 == SOCKET_ERROR ? "NONE" : "other"));

			closesocket(hA);
			closesocket(hB);
		} else
			Probe("udp-roundtrip", "skipped");
	}

	// 9. A datagram sent to a port nobody is listening on. On Windows the ICMP
	//    that comes back turns into an error on the NEXT receive of the sending
	//    socket - a behaviour with no equivalent on a Unix socket unless it is
	//    asked for.
	{
		SOCKET hSender = MakeUdp((u_short)(s_opt.nPort + 2));
		if (hSender != INVALID_SOCKET) {
			SOCKADDR_IN stTo;
			::ZeroMemory(&stTo, sizeof stTo);
			stTo.sin_family = AF_INET;
			stTo.sin_port = htons((u_short)(s_opt.nPort + 40));   // nobody there
			stTo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			char szOut[64];
			::memset(szOut, 'Z', sizeof szOut);
			sendto(hSender, szOut, (int)sizeof szOut, 0, (LPSOCKADDR)&stTo, sizeof stTo);
			::Sleep(200);
			char szIn[256];
			SOCKADDR_IN stFrom;
			int nErr = 0;
			const int nGot = RecvFromWait(hSender, szIn, (int)sizeof szIn, &stFrom, &nErr, 500);
			Probe("udp-unreachable", "next-receive=%s"
				, nGot >= 0 ? "data"
					: (nErr == WSAECONNRESET ? "connreset"
					: (nErr == WSAEWOULDBLOCK ? "nothing" : "other")));
			closesocket(hSender);
		} else
			Probe("udp-unreachable", "skipped");
	}

	return nExit;
}

/////////////////////////////////////////////////////////////////////////////
// Mode "stress": many answers, over and over

static int RunStress()
{
	int nStartedTotal = 0, nAnsweredTotal = 0, nRoundsShort = 0;
	DWORD dwWorstMs = 0;

	for (int nRound = 0; nRound < s_opt.nRounds; ++nRound) {
		ResetRequests();
		int nStarted = 0;
		for (int i = 0; i < s_opt.nConcurrent; ++i)
			if (StartResolve("localhost", 0) >= 0)
				++nStarted;

		const DWORD dwT0 = ::GetTickCount();
		const int nDone = PumpUntilAnswered(nStarted, s_opt.nWaitMs);
		const DWORD dwTook = ::GetTickCount() - dwT0;
		if (dwTook > dwWorstMs)
			dwWorstMs = dwTook;

		nStartedTotal += nStarted;
		nAnsweredTotal += nDone;
		if (nDone != nStarted) {
			++nRoundsShort;
			Log("round %d: %d started, %d answered after %ums", nRound, nStarted, nDone, dwTook);
		}
	}

	Log("totals: %d resolutions started, %d answered, worst round %ums"
		, nStartedTotal, nAnsweredTotal, dwWorstMs);
	Probe("stress", "started=%d missing=%d rounds-short=%d"
		, nStartedTotal, nStartedTotal - nAnsweredTotal, nRoundsShort);

	if (nAnsweredTotal != nStartedTotal) {
		Log("VERDICT: %d of %d answers never arrived. The request stays on eMule's list"
			, nStartedTotal - nAnsweredTotal, nStartedTotal);
		Log("VERDICT: for good, and the packets queued behind it are never sent.");
		return 1;
	}
	Log("VERDICT: every one of %d answers arrived (worst round %ums)", nStartedTotal, dwWorstMs);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////

static void Usage()
{
	printf("dnsrepro - eMule's asynchronous name resolution and UDP sockets\n"
		"\n"
		"  --mode <contract|stress>  what to run (default contract)\n"
		"  --concurrent <n>   resolutions outstanding at once (default 8)\n"
		"  --rounds <n>       stress: how many times that many (default 200)\n"
		"  --wait-ms <n>      how long an answer is waited for (default 5000)\n"
		"  --port <n>         base loopback port for the UDP probes (default 4796)\n"
		"\n"
		"Exit code 1 means something was found.\n");
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
		} else if (!strcmp(p, "--concurrent") && bHasVal)
			s_opt.nConcurrent = atoi(argv[++i]);
		else if (!strcmp(p, "--rounds") && bHasVal)
			s_opt.nRounds = atoi(argv[++i]);
		else if (!strcmp(p, "--wait-ms") && bHasVal)
			s_opt.nWaitMs = atoi(argv[++i]);
		else if (!strcmp(p, "--port") && bHasVal)
			s_opt.nPort = (u_short)atoi(argv[++i]);
		else {
			Usage();
			return false;
		}
	}
	if (s_opt.nConcurrent < 1 || s_opt.nConcurrent > MAX_REQUESTS) {
		printf("--concurrent must be between 1 and %d\n", MAX_REQUESTS);
		return false;
	}
	return s_opt.nRounds > 0 && s_opt.nWaitMs > 0;
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
	if (!CreateMessageWindow()) {
		printf("could not create the message window, err=%u\n", ::GetLastError());
		return 2;
	}

	Log("start: mode=%s concurrent=%d rounds=%d", s_pszModeNames[s_opt.nMode], s_opt.nConcurrent, s_opt.nRounds);

	const int nExit = (s_opt.nMode == MODE_CONTRACT) ? RunContract() : RunStress();

	Log("exit=%d", nExit);
	::DestroyWindow(s_hWnd);
	::WSACleanup();
	::DeleteCriticalSection(&s_csLog);
	return nExit;
}
