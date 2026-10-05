// Winsock - WS2_32 / WSOCK32 - and IPHLPAPI.
//
// Everything runs on loopback. The asynchronous-notification tests record
// the sequence of WM_ messages / network events a socket receives after each
// step, because that sequence is what CAsyncSocketEx (and through it every
// eMule connection) is driven by.
#include "harness.h"
#include <iphlpapi.h>
#include <icmpapi.h>
#include <mstcpip.h>
#include <stdio.h>
#include <algorithm>

static void Startup()
{
	WSADATA wd;
	::WSAStartup(MAKEWORD(2, 2), &wd);
}

static u_short Port(SOCKET s)
{
	sockaddr_in a = {};
	int n = sizeof a;
	::getsockname(s, (sockaddr*)&a, &n);
	return ntohs(a.sin_port);
}

static sockaddr_in Loop(u_short port)
{
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(port);
	return a;
}

static SOCKET Listener()
{
	SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a = Loop(0);
	::bind(l, (sockaddr*)&a, sizeof a);
	::listen(l, SOMAXCONN);
	return l;
}

struct Pair { SOCKET l, c, s; };

static Pair MakePair()
{
	Pair p;
	p.l = Listener();
	p.c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a = Loop(Port(p.l));
	::connect(p.c, (sockaddr*)&a, sizeof a);
	p.s = ::accept(p.l, NULL, NULL);
	return p;
}

static void Close(Pair &p)
{
	for (SOCKET *s : {&p.l, &p.c, &p.s})
		if (*s != INVALID_SOCKET) {
			::closesocket(*s);
			*s = INVALID_SOCKET;
		}
}

static void Abort(SOCKET s)
{
	linger lg = {1, 0};
	::setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof lg);
	::closesocket(s);
}

static std::string WsaErr(int r)
{
	return r == SOCKET_ERROR ? Fmt("SOCKET_ERROR wsa=%d", ::WSAGetLastError()) : Fmt("%d", r);
}

static std::string EvName(long ev)
{
	switch (ev) {
	case FD_READ: return "READ";
	case FD_WRITE: return "WRITE";
	case FD_OOB: return "OOB";
	case FD_ACCEPT: return "ACCEPT";
	case FD_CONNECT: return "CONNECT";
	case FD_CLOSE: return "CLOSE";
	default: return Fmt("ev%lx", ev);
	}
}

// ---------------------------------------------------------------------------
// A hidden window receiving WSAAsyncSelect notifications.

#define WM_SOCK (WM_USER + 0x100)

struct Notif { int tag; long ev; int err; };
static std::vector<Notif> s_notif;
static std::vector<std::pair<WPARAM, LPARAM>> s_dns;
static SOCKET s_tagSock[8];

static LRESULT CALLBACK SockWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m >= WM_SOCK && m < WM_SOCK + 8) {
		s_notif.push_back(Notif{(int)(m - WM_SOCK), WSAGETSELECTEVENT(l), WSAGETSELECTERROR(l)});
		return 0;
	}
	if (m == WM_SOCK + 0x40) {
		s_dns.push_back(std::make_pair(w, l));
		return 0;
	}
	return ::DefWindowProcW(h, m, w, l);
}

static HWND SockWindow()
{
	static HWND h = NULL;
	if (!h) {
		WNDCLASSW wc = {0};
		wc.lpfnWndProc = SockWndProc;
		wc.hInstance = ::GetModuleHandleW(NULL);
		wc.lpszClassName = L"symrepro_sock";
		::RegisterClassW(&wc);
		h = ::CreateWindowW(L"symrepro_sock", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
	}
	return h;
}

// Pump for ms milliseconds and return what arrived, rendered.
static std::string Collect(DWORD ms, int storm = 20)
{
	s_notif.clear();
	PumpUntil([] { return false; }, ms);
	std::string r;
	int n = 0;
	for (size_t i = 0; i < s_notif.size(); ++i) {
		const Notif &x = s_notif[i];
		// Collapse runs of the same notification, so a storm shows as one
		// item with a count bucket rather than as a timing-dependent number.
		size_t j = i;
		while (j + 1 < s_notif.size() && s_notif[j + 1].tag == x.tag && s_notif[j + 1].ev == x.ev && s_notif[j + 1].err == x.err)
			++j;
		size_t run = j - i + 1;
		if (n++)
			r += " ";
		r += Fmt("%d:%s", x.tag, EvName(x.ev).c_str());
		if (x.err)
			r += Fmt("(%d)", x.err);
		if (run > 1)
			r += run > (size_t)storm ? Fmt("x>%d", storm) : Fmt("x%u", (unsigned)run);
		i = j;
	}
	return r.empty() ? "-" : r;
}

static void Async(SOCKET s, int tag, long ev)
{
	s_tagSock[tag] = s;
	::WSAAsyncSelect(s, SockWindow(), WM_SOCK + tag, ev);
}

static const long kAll = FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE;

// ---------------------------------------------------------------------------

TEST(net_Startup)
{
	WSADATA wd = {0};
	int r = ::WSAStartup(MAKEWORD(2, 2), &wd);
	out("WSAStartup", "2.2", "r=%d ver=%x high=%x maxsock=%u", r, wd.wVersion, wd.wHighVersion, wd.iMaxSockets);
	r = ::WSAStartup(MAKEWORD(1, 1), &wd);
	out("WSAStartup", "1.1", "r=%d ver=%x high=%x", r, wd.wVersion, wd.wHighVersion);
	r = ::WSAStartup(MAKEWORD(3, 0), &wd);
	out("WSAStartup", "3.0", "r=%d ver=%x", r, wd.wVersion);
	r = ::WSAStartup(MAKEWORD(0, 1), &wd);
	out("WSAStartup", "0.1", "r=%d ver=%x", r, wd.wVersion);
	r = ::WSAStartup(MAKEWORD(2, 2), NULL);
	out("WSAStartup", "nulldata", "r=%d", r);
	int n = 0;
	while (::WSACleanup() == 0 && n < 10)
		++n;
	out("WSACleanup", "count", "succeeded=%d lasterr=%d", n, ::WSAGetLastError());
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
	out("socket", "notinitialised", "%s wsa=%d", s == INVALID_SOCKET ? "INVALID" : "ok", ::WSAGetLastError());
	::WSASetLastError(12345);
	out("WSASetLastError", "roundtrip", "%d last=%lu", ::WSAGetLastError(), ::GetLastError());
	::SetLastError(777);
	out("WSAGetLastError", "fromSetLastError", "%d", ::WSAGetLastError());
}

TEST(net_ByteOrder)
{
	out("htonl", "x", "%08lx", ::htonl(0x01020304));
	out("htons", "x", "%04x", ::htons(0x0102));
	out("ntohl", "x", "%08lx", ::ntohl(0x01020304));
	out("ntohs", "x", "%04x", ::ntohs(0x0102));
	out("htonl", "zero.max", "%08lx %08lx", ::htonl(0), ::htonl(0xFFFFFFFF));
}

TEST(net_inet)
{
	Startup();
	// Every shape of address string eMule feeds inet_addr: server lists,
	// ed2k links, IP filter search box, bind address preference.
	static const char *const c[] = {"1.2.3.4", "255.255.255.255", "0.0.0.0", "1.2.3", "1.2", "1", "0x7f.1", "017.0.0.1",
		" 1.2.3.4", "1.2.3.4 ", "1.2.3.4x", "256.1.1.1", "", "1.2.3.4.5", "localhost", "1..2.3", "4294967295", "0x7f000001",
		"0xffffffff", "1.2.3.04", "08.1.1.1", "1.2.3.4\t", "\t1.2.3.4", "+1.2.3.4", "-1.2.3.4", "1.2.3.", ".1.2.3", "a.b.c.d",
		"1.2.65535", "1.16777215", "1.2.3.256", "127.1", "0", "00000000001.2.3.4"};
	for (const char *s : c) {
		unsigned long r = ::inet_addr(s);
		out("inet_addr", QA(s).c_str(), "%08lx", r);
	}
	out("inet_addr", "NULL", "%08lx", ::inet_addr(NULL));
	for (unsigned long v : {0x04030201ul, 0ul, 0xFFFFFFFFul, 0x0100007Ful, 0x000000FFul}) {
		in_addr a;
		a.s_addr = v;
		out("inet_ntoa", Fmt("%08lx", v).c_str(), "%s", QA(::inet_ntoa(a)).c_str());
	}
	in_addr a1, a2;
	a1.s_addr = 0x04030201;
	a2.s_addr = 0x08070605;
	char *p1 = ::inet_ntoa(a1);
	char *p2 = ::inet_ntoa(a2);
	out("inet_ntoa", "staticbuffer", "same=%d first_now=%s", p1 == p2, QA(p1).c_str());
	static const char *const p[] = {"1.2.3.4", "01.2.3.4", "1.2.3", "256.1.1.1", "0x1.2.3.4", " 1.2.3.4", "1.2.3.4 ", ""};
	for (const char *s : p) {
		in_addr a = {};
		int r = ::inet_pton(AF_INET, s, &a);
		out("inet_pton", Fmt("v4.%s", QA(s).c_str()).c_str(), "r=%d %08lx", r, a.s_addr);
	}
	static const char *const p6[] = {"::1", "::", "fe80::1", "::ffff:1.2.3.4", "1:2:3:4:5:6:7:8", "1::2::3", "fe80::1%1", "gggg::1"};
	for (const char *s : p6) {
		in6_addr a = {};
		int r = ::inet_pton(AF_INET6, s, &a);
		out("inet_pton", Fmt("v6.%s", QA(s).c_str()).c_str(), "r=%d %s", r, Hex(&a, sizeof a).c_str());
	}
	in_addr a = {};
	int r = ::inet_pton(12345, "1.2.3.4", &a);
	out("inet_pton", "badfamily", "r=%d wsa=%d", r, ::WSAGetLastError());
	sockaddr_in sa = Loop(4662);
	char host[NI_MAXHOST], serv[NI_MAXSERV];
	r = ::getnameinfo((sockaddr*)&sa, sizeof sa, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
	out("getnameinfo", "numeric", "r=%d %s %s", r, QA(host).c_str(), QA(serv).c_str());
	sa.sin_port = htons(80);
	r = ::getnameinfo((sockaddr*)&sa, sizeof sa, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST);
	out("getnameinfo", "service80", "r=%d %s", r, QA(serv).c_str());
	r = ::getnameinfo((sockaddr*)&sa, sizeof sa, host, sizeof host, NULL, 0, NI_NAMEREQD);
	out("getnameinfo", "~loopback.name", "r=%d %s", r, QA(host).c_str());
	r = ::getnameinfo((sockaddr*)&sa, 4, host, sizeof host, NULL, 0, NI_NUMERICHOST);
	out("getnameinfo", "shortlen", "r=%d", r);
}

TEST(net_resolve)
{
	Startup();
	char name[256] = "";
	int r = ::gethostname(name, sizeof name);
	out("gethostname", "ok", "r=%d nonempty=%d", r, name[0] != 0);
	out("gethostname", "~value", "%s", QA(name).c_str());
	char small[2];
	r = ::gethostname(small, sizeof small);
	out("gethostname", "small", "%s", WsaErr(r).c_str());
	r = ::gethostname(NULL, 10);
	out("gethostname", "null", "%s", WsaErr(r).c_str());
	struct { const char *n; const char *host; const char *port; int family, flags; } c[] = {
		{"localhost.v4", "localhost", NULL, AF_INET, 0}, {"literal", "127.0.0.1", "4662", AF_INET, 0},
		{"literal.numerichost", "127.0.0.1", NULL, AF_INET, AI_NUMERICHOST}, {"name.numerichost", "localhost", NULL, AF_INET, AI_NUMERICHOST},
		{"passive.null", NULL, "4662", AF_INET, AI_PASSIVE}, {"nullnull", NULL, NULL, AF_INET, 0}, {"empty", "", NULL, AF_INET, 0},
		{"shortform", "127.1", NULL, AF_INET, 0}, {"invalid", "nosuchhost.invalid", NULL, AF_INET, 0}, {"v6.loopback", "::1", NULL, AF_UNSPEC, 0},
		{"v6.on.v4", "::1", NULL, AF_INET, 0}, {"servicename", "127.0.0.1", "http", AF_INET, 0}, {"badport", "127.0.0.1", "99999", AF_INET, 0},
		{"space", " 127.0.0.1", NULL, AF_INET, 0}, {"trailingdot", "localhost.", NULL, AF_INET, 0}};
	for (auto &x : c) {
		addrinfo hints = {}, *res = NULL;
		hints.ai_family = x.family;
		hints.ai_flags = x.flags;
		hints.ai_socktype = SOCK_STREAM;
		r = ::getaddrinfo(x.host, x.port, &hints, &res);
		std::vector<std::string> items;
		bool hasLoop4 = false;
		for (addrinfo *a = res; a; a = a->ai_next) {
			char h[64] = "";
			char s[16] = "";
			::getnameinfo(a->ai_addr, (socklen_t)a->ai_addrlen, h, sizeof h, s, sizeof s, NI_NUMERICHOST | NI_NUMERICSERV);
			items.push_back(Fmt("%d/%d/%d:%s:%s", a->ai_family, a->ai_socktype, a->ai_protocol, h, s));
			hasLoop4 |= !strcmp(h, "127.0.0.1");
		}
		std::sort(items.begin(), items.end());	// an unordered set of answers
		std::string list;
		for (auto &i : items)
			list += " " + i;
		out("getaddrinfo", x.n, "r=%d n=%u loop4=%d first=%s", r, (unsigned)items.size(), hasLoop4, items.empty() ? "-" : items[0].c_str());
		out("getaddrinfo", (std::string("~") + x.n + ".list").c_str(), "%s", list.c_str());
		if (res)
			::freeaddrinfo(res);
	}
	::freeaddrinfo(NULL);
	out("freeaddrinfo", "null", "ok");
	// WSOCK32 gethostbyname (via a library) and the WS2_32 one.
	for (const char *h : {"localhost", "127.0.0.1", "nosuchhost.invalid", "", "1.2.3"}) {
		hostent *he = ::gethostbyname(h);
		int e = ::WSAGetLastError();
		std::string addrs;
		if (he)
			for (char **a = he->h_addr_list; *a; ++a)
				addrs += Fmt(" %08lx", *(u_long*)*a);
		out("gethostbyname", QA(h).c_str(), "%s wsa=%d len=%d", he ? "ok" : "NULL", he ? 0 : e, he ? he->h_length : 0);
		out("gethostbyname", (std::string("~") + h + ".addrs").c_str(), "%s", addrs.c_str());
	}
}

TEST(net_Wsock32)
{
	// WSOCK32!gethostbyname is in eMule's import table through a library.
	Startup();
	HMODULE h = ::LoadLibraryW(L"wsock32.dll");
	typedef hostent *(WINAPI *GHBN)(const char *);
	GHBN f = h ? (GHBN)::GetProcAddress(h, "gethostbyname") : NULL;
	out("gethostbyname", "wsock32.proc", "%s", f ? "ok" : "NULL");
	for (const char *n : {"localhost", "127.0.0.1", "nosuchhost.invalid"}) {
		hostent *he = f ? f(n) : NULL;
		out("gethostbyname", Fmt("wsock32.%s", n).c_str(), "%s wsa=%d first=%08lx", he ? "ok" : "NULL", he ? 0 : ::WSAGetLastError(),
			he && he->h_addr_list[0] ? *(u_long*)he->h_addr_list[0] : 0);
	}
}

TEST_T(net_AsyncGetHostByName, 60000)
{
	Startup();
	HWND w = SockWindow();
	static char buf1[MAXGETHOSTSTRUCT], buf2[MAXGETHOSTSTRUCT], buf3[MAXGETHOSTSTRUCT], tiny[20];
	struct { const char *n; const char *host; char *buf; int len; } c[] = {
		{"localhost", "localhost", buf1, MAXGETHOSTSTRUCT}, {"literal", "127.0.0.1", buf2, MAXGETHOSTSTRUCT},
		{"invalid", "nosuchhost.invalid", buf3, MAXGETHOSTSTRUCT}, {"tinybuffer", "localhost", tiny, sizeof tiny}};
	for (auto &x : c) {
		s_dns.clear();
		HANDLE h = ::WSAAsyncGetHostByName(w, WM_SOCK + 0x40, x.host, x.buf, x.len);
		bool got = PumpUntil([] { return !s_dns.empty(); }, 20000);
		std::string res = "none";
		if (got) {
			LPARAM l = s_dns[0].second;
			res = Fmt("err=%d buflen=%d handle_match=%d", WSAGETASYNCERROR(l), WSAGETASYNCBUFLEN(l), (HANDLE)s_dns[0].first == h);
			if (!WSAGETASYNCERROR(l)) {
				hostent *he = (hostent*)x.buf;
				res += Fmt(" first=%08lx", he->h_addr_list[0] ? *(u_long*)he->h_addr_list[0] : 0);
			}
		}
		out("WSAAsyncGetHostByName", x.n, "handle=%s %s", h ? "ok" : "NULL", res.c_str());
		PumpUntil([] { return false; }, 200);
		out("WSAAsyncGetHostByName", (std::string(x.n) + ".extra").c_str(), "%u", (unsigned)s_dns.size());
	}
	s_dns.clear();
	HANDLE h = ::WSAAsyncGetHostByName(NULL, WM_SOCK + 0x40, "localhost", buf1, MAXGETHOSTSTRUCT);
	out("WSAAsyncGetHostByName", "nullwindow", "%s wsa=%d", h ? "ok" : "NULL", h ? 0 : ::WSAGetLastError());
	int r = ::WSACancelAsyncRequest((HANDLE)(ULONG_PTR)0x4321);
	out("WSACancelAsyncRequest", "bogus", "%s", WsaErr(r).c_str());
}

TEST(net_TcpBasic)
{
	Startup();
	SOCKET l = Listener();
	out("listen", "port.nonzero", "%d", Port(l) != 0);
	Pair p;
	p.l = l;
	p.c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a = Loop(Port(l));
	int r = ::connect(p.c, (sockaddr*)&a, sizeof a);
	out("connect", "blocking", "%s", WsaErr(r).c_str());
	sockaddr_in pa = {};
	int plen = sizeof pa;
	p.s = ::accept(l, (sockaddr*)&pa, &plen);
	out("accept", "blocking", "%s len=%d family=%d addr=%08lx port_is_client=%d", p.s != INVALID_SOCKET ? "ok" : "INVALID", plen,
		pa.sin_family, pa.sin_addr.s_addr, ntohs(pa.sin_port) == Port(p.c));
	sockaddr_in na = {};
	int nlen = sizeof na;
	r = ::getpeername(p.c, (sockaddr*)&na, &nlen);
	out("getpeername", "client", "r=%d len=%d match=%d", r, nlen, na.sin_port == a.sin_port && na.sin_addr.s_addr == a.sin_addr.s_addr);
	nlen = 4;
	r = ::getpeername(p.c, (sockaddr*)&na, &nlen);
	out("getpeername", "shortlen", "%s", WsaErr(r).c_str());
	r = ::getpeername(l, (sockaddr*)&na, &(nlen = sizeof na));
	out("getpeername", "listener", "%s", WsaErr(r).c_str());
	r = ::getsockname(p.s, (sockaddr*)&na, &(nlen = sizeof na));
	out("getsockname", "accepted", "r=%d sameport_as_listener=%d", r, ntohs(na.sin_port) == Port(l));
	SOCKET un = ::socket(AF_INET, SOCK_STREAM, 0);
	r = ::getsockname(un, (sockaddr*)&na, &(nlen = sizeof na));
	out("getsockname", "unbound", "%s", WsaErr(r).c_str());
	r = ::getpeername(un, (sockaddr*)&na, &(nlen = sizeof na));
	out("getpeername", "unconnected", "%s", WsaErr(r).c_str());
	::closesocket(un);
	r = ::send(p.c, "hello world", 11, 0);
	out("send", "basic", "%s", WsaErr(r).c_str());
	::Sleep(50);
	u_long avail = 0;
	r = ::ioctlsocket(p.s, FIONREAD, &avail);
	out("ioctlsocket", "fionread", "r=%d avail=%lu", r, avail);
	char buf[64];
	r = ::recv(p.s, buf, 5, MSG_PEEK);
	out("recv", "peek", "%s %s", WsaErr(r).c_str(), QA(buf, r > 0 ? r : 0).c_str());
	r = ::recv(p.s, buf, 5, 0);
	out("recv", "partial", "%s %s", WsaErr(r).c_str(), QA(buf, r > 0 ? r : 0).c_str());
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("recv", "rest", "%s %s", WsaErr(r).c_str(), QA(buf, r > 0 ? r : 0).c_str());
	u_long nb = 1;
	r = ::ioctlsocket(p.s, FIONBIO, &nb);
	out("ioctlsocket", "fionbio", "%d", r);
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("recv", "nonblocking.empty", "%s", WsaErr(r).c_str());
	r = ::recv(p.s, buf, 0, 0);
	out("recv", "zerolen", "%s", WsaErr(r).c_str());
	r = ::ioctlsocket(p.s, 0x12345678, &nb);
	out("ioctlsocket", "badcmd", "%s", WsaErr(r).c_str());
	// Fill the send side until it would block.
	::ioctlsocket(p.c, FIONBIO, &nb);
	static char big[65536];
	long total = 0;
	for (int i = 0; i < 2000; ++i) {
		r = ::send(p.c, big, sizeof big, 0);
		if (r == SOCKET_ERROR)
			break;
		total += r;
	}
	out("send", "fill.stops_with", "%s", WsaErr(r).c_str());
	out("send", "~fill.bytes", "%ld", total);
	// Graceful half close.
	Close(p);
	p = MakePair();
	r = ::shutdown(p.c, SD_SEND);
	out("shutdown", "sd_send", "%d", r);
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("recv", "after.peer.sd_send", "%s", WsaErr(r).c_str());
	r = ::send(p.s, "back", 4, 0);
	out("send", "towards.halfclosed", "%s", WsaErr(r).c_str());
	r = ::recv(p.c, buf, sizeof buf, 0);
	out("recv", "after.own.sd_send", "%s", WsaErr(r).c_str());
	r = ::send(p.c, "x", 1, 0);
	out("send", "after.own.sd_send", "%s", WsaErr(r).c_str());
	Close(p);
	// Peer closes gracefully, then we send twice.
	p = MakePair();
	::closesocket(p.c);
	p.c = INVALID_SOCKET;
	::Sleep(100);
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("recv", "after.peer.close", "%s", WsaErr(r).c_str());
	r = ::send(p.s, "x", 1, 0);
	out("send", "after.peer.close.1", "%s", WsaErr(r).c_str());
	::Sleep(100);
	r = ::send(p.s, "x", 1, 0);
	out("send", "after.peer.close.2", "%s", WsaErr(r).c_str());
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("recv", "after.peer.close.and.rst", "%s", WsaErr(r).c_str());
	Close(p);
	// Peer aborts (linger 0 -> RST).
	p = MakePair();
	::send(p.c, "pending", 7, 0);
	Abort(p.c);
	p.c = INVALID_SOCKET;
	::Sleep(100);
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("recv", "after.peer.abort.with_unread", "%s", WsaErr(r).c_str());
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("recv", "after.peer.abort.again", "%s", WsaErr(r).c_str());
	r = ::send(p.s, "x", 1, 0);
	out("send", "after.peer.abort", "%s", WsaErr(r).c_str());
	Close(p);
	// Operations on a closed socket handle.
	SOCKET dead = ::socket(AF_INET, SOCK_STREAM, 0);
	::closesocket(dead);
	r = ::send(dead, "x", 1, 0);
	out("send", "closedhandle", "%s", WsaErr(r).c_str());
	r = ::closesocket(dead);
	out("closesocket", "twice", "%s", WsaErr(r).c_str());
	r = ::closesocket(INVALID_SOCKET);
	out("closesocket", "invalid", "%s", WsaErr(r).c_str());
}

TEST(net_ShutdownReceive)
{
	// CUpDownClient::Ban: ShutDown(receives) on a connected socket, then the
	// peer keeps sending. What do recv, send and the notifications do?
	Startup();
	char buf[256];
	// Synchronous view.
	Pair p = MakePair();
	int r = ::shutdown(p.s, SD_RECEIVE);
	out("shutdown", "sd_receive", "%d", r);
	r = ::send(p.c, "after", 5, 0);
	out("shutdown", "sd_receive.peer.send1", "%s", WsaErr(r).c_str());
	::Sleep(200);
	u_long avail = 99;
	::ioctlsocket(p.s, FIONREAD, &avail);
	out("shutdown", "sd_receive.fionread", "%lu", avail);
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("shutdown", "sd_receive.recv", "%s", WsaErr(r).c_str());
	r = ::send(p.s, "reply", 5, 0);
	out("shutdown", "sd_receive.own.send", "%s", WsaErr(r).c_str());
	::Sleep(100);
	r = ::send(p.c, "more", 4, 0);
	out("shutdown", "sd_receive.peer.send2", "%s", WsaErr(r).c_str());
	::Sleep(200);
	r = ::recv(p.c, buf, sizeof buf, 0);
	out("shutdown", "sd_receive.peer.recv", "%s", WsaErr(r).c_str());
	r = ::recv(p.c, buf, sizeof buf, 0);
	out("shutdown", "sd_receive.peer.recv2", "%s", WsaErr(r).c_str());
	fd_set rs;
	FD_ZERO(&rs);
	FD_SET(p.s, &rs);
	timeval tv = {0, 200000};
	r = ::select(0, &rs, NULL, NULL, &tv);
	out("shutdown", "sd_receive.select.readable", "%s", WsaErr(r).c_str());
	Close(p);

	// Data already queued before the shutdown.
	p = MakePair();
	::send(p.c, "queued", 6, 0);
	::Sleep(100);
	r = ::shutdown(p.s, SD_RECEIVE);
	out("shutdown", "sd_receive.withqueued", "%d", r);
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("shutdown", "sd_receive.withqueued.recv", "%s", WsaErr(r).c_str());
	Close(p);

	// The eMule shape: WSAAsyncSelect on the socket, ShutDown(receives),
	// peer sends. Count what the window receives in one second.
	p = MakePair();
	Async(p.s, 1, FD_READ | FD_WRITE | FD_CLOSE);
	out("WSAAsyncSelect", "sd_receive.initial", "%s", Collect(300).c_str());
	::shutdown(p.s, SD_RECEIVE);
	::send(p.c, "after-ban", 9, 0);
	out("WSAAsyncSelect", "sd_receive.peersends", "%s", Collect(1000).c_str());
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("WSAAsyncSelect", "sd_receive.recv_in_handler", "%s", WsaErr(r).c_str());
	out("WSAAsyncSelect", "sd_receive.after_recv", "%s", Collect(1000).c_str());
	::send(p.c, "again", 5, 0);
	out("WSAAsyncSelect", "sd_receive.peersends_again", "%s", Collect(1000).c_str());
	::closesocket(p.c);
	p.c = INVALID_SOCKET;
	out("WSAAsyncSelect", "sd_receive.peercloses", "%s", Collect(1000).c_str());
	Close(p);

	// 96c's change: drop FD_READ before shutting down receives.
	p = MakePair();
	Async(p.s, 2, FD_READ | FD_WRITE | FD_CLOSE);
	Collect(300);
	Async(p.s, 2, FD_CLOSE | FD_WRITE);
	::shutdown(p.s, SD_RECEIVE);
	::send(p.c, "after-ban", 9, 0);
	out("WSAAsyncSelect", "96c.peersends", "%s", Collect(1000).c_str());
	::closesocket(p.c);
	p.c = INVALID_SOCKET;
	out("WSAAsyncSelect", "96c.peercloses", "%s", Collect(1000).c_str());
	Close(p);

	// SD_BOTH and the peer's view.
	p = MakePair();
	r = ::shutdown(p.s, SD_BOTH);
	out("shutdown", "sd_both", "%d", r);
	r = ::recv(p.c, buf, sizeof buf, 0);
	out("shutdown", "sd_both.peer.recv", "%s", WsaErr(r).c_str());
	r = ::shutdown(p.s, 7);
	out("shutdown", "badhow", "%s", WsaErr(r).c_str());
	SOCKET un = ::socket(AF_INET, SOCK_STREAM, 0);
	r = ::shutdown(un, SD_SEND);
	out("shutdown", "notconnected", "%s", WsaErr(r).c_str());
	::closesocket(un);
	r = ::shutdown(p.l, SD_BOTH);
	out("shutdown", "listener", "%s", WsaErr(r).c_str());
	Close(p);
}

TEST(net_AsyncSelectSequences)
{
	// The notification grammar CAsyncSocketEx depends on.
	Startup();
	char buf[256];
	SOCKET l = Listener();
	Async(l, 0, FD_ACCEPT);
	SOCKET c = ::socket(AF_INET, SOCK_STREAM, 0);
	Async(c, 1, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
	sockaddr_in a = Loop(Port(l));
	int r = ::connect(c, (sockaddr*)&a, sizeof a);
	out("connect", "async", "%s", WsaErr(r).c_str());
	out("WSAAsyncSelect", "connect", "%s", Collect(500).c_str());
	SOCKET s = ::accept(l, NULL, NULL);
	out("WSAAsyncSelect", "after.accept", "%s", Collect(300).c_str());
	// The accepted socket inherits the listener's selection (FD_ACCEPT only):
	// eMule re-selects with FD_DEFAULT straight after accepting.
	Async(s, 2, FD_READ | FD_WRITE | FD_CLOSE);
	out("WSAAsyncSelect", "accepted.select", "%s", Collect(300).c_str());
	::send(c, "0123456789", 10, 0);
	out("WSAAsyncSelect", "data10", "%s", Collect(300).c_str());
	::send(c, "abc", 3, 0);
	out("WSAAsyncSelect", "moredata.noread", "%s", Collect(300).c_str());
	r = ::recv(s, buf, 4, 0);
	out("WSAAsyncSelect", "partialread", "r=%d %s", r, Collect(300).c_str());
	r = ::recv(s, buf, sizeof buf, 0);
	out("WSAAsyncSelect", "fullread", "r=%d %s", r, Collect(300).c_str());
	r = ::recv(s, buf, sizeof buf, 0);
	out("WSAAsyncSelect", "readempty", "%s %s", WsaErr(r).c_str(), Collect(300).c_str());
	// Re-select while writable and with data waiting.
	::send(c, "z", 1, 0);
	Collect(200);
	Async(s, 2, FD_READ | FD_WRITE | FD_CLOSE);
	out("WSAAsyncSelect", "reselect.writable.datawaiting", "%s", Collect(300).c_str());
	::recv(s, buf, sizeof buf, 0);
	Collect(200);
	// Fill until WOULDBLOCK, then let the peer drain: FD_WRITE must come.
	static char big[65536];
	for (int i = 0; i < 2000; ++i)
		if (::send(s, big, sizeof big, 0) == SOCKET_ERROR)
			break;
	out("WSAAsyncSelect", "wouldblock.err", "%d", ::WSAGetLastError());
	Collect(100);
	u_long nb = 1;
	::ioctlsocket(c, FIONBIO, &nb);
	// c's notifications are on tag 1; keep reading until it would block.
	for (int i = 0; i < 4000; ++i) {
		r = ::recv(c, big, sizeof big, 0);
		if (r <= 0) {
			::Sleep(5);
			r = ::recv(c, big, sizeof big, 0);
			if (r <= 0)
				break;
		}
	}
	std::string seq = Collect(500);
	// Only the server side's FD_WRITE is of interest here.
	out("WSAAsyncSelect", "drained.server_gets_write", "%d", seq.find("2:WRITE") != std::string::npos);
	// Cancel selection, then traffic: nothing must arrive.
	::WSAAsyncSelect(s, SockWindow(), 0, 0);
	::send(c, "q", 1, 0);
	out("WSAAsyncSelect", "cancelled", "%s", Collect(300).c_str());
	r = ::recv(s, buf, sizeof buf, 0);
	out("WSAAsyncSelect", "cancelled.stillnonblocking", "%s", WsaErr(r).c_str());
	nb = 0;
	r = ::ioctlsocket(s, FIONBIO, &nb);
	out("ioctlsocket", "fionbio.off.while.selected_cancelled", "%s", WsaErr(r).c_str());
	Async(s, 2, FD_READ | FD_CLOSE);
	nb = 0;
	r = ::ioctlsocket(s, FIONBIO, &nb);
	out("ioctlsocket", "fionbio.off.while.selected", "%s", WsaErr(r).c_str());
	Collect(200);
	// Graceful close by the peer with unread data on our side.
	::send(c, "last words", 10, 0);
	::Sleep(50);
	::closesocket(c);
	out("WSAAsyncSelect", "peerclose.with_unread", "%s", Collect(500).c_str());
	r = ::recv(s, buf, sizeof buf, 0);
	out("WSAAsyncSelect", "peerclose.read", "r=%d %s", r, Collect(300).c_str());
	r = ::recv(s, buf, sizeof buf, 0);
	out("WSAAsyncSelect", "peerclose.read_eof", "r=%d %s", r, Collect(300).c_str());
	::closesocket(s);
	// Abortive close by the peer.
	Async(l, 0, FD_ACCEPT);
	c = ::socket(AF_INET, SOCK_STREAM, 0);
	::connect(c, (sockaddr*)&a, sizeof a);
	Collect(200);
	s = ::accept(l, NULL, NULL);
	Async(s, 3, FD_READ | FD_WRITE | FD_CLOSE);
	Collect(200);
	Abort(c);
	out("WSAAsyncSelect", "peerabort", "%s", Collect(500).c_str());
	r = ::recv(s, buf, sizeof buf, 0);
	out("WSAAsyncSelect", "peerabort.recv", "%s", WsaErr(r).c_str());
	// Messages for a socket closed while they are queued.
	c = ::socket(AF_INET, SOCK_STREAM, 0);
	::connect(c, (sockaddr*)&a, sizeof a);
	Collect(200);
	SOCKET s2 = ::accept(l, NULL, NULL);
	Async(s2, 4, FD_READ | FD_WRITE | FD_CLOSE);
	::send(c, "x", 1, 0);
	::Sleep(100);
	::closesocket(s2);
	out("WSAAsyncSelect", "queued.then.closed", "%s", Collect(300).c_str());
	::closesocket(c);
	::closesocket(s);
	// Connect to a port nobody listens on.
	SOCKET d = ::socket(AF_INET, SOCK_STREAM, 0);
	Async(d, 5, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
	sockaddr_in dead = Loop(Port(l));
	::closesocket(l);
	DWORD t0 = ::GetTickCount();
	r = ::connect(d, (sockaddr*)&dead, sizeof dead);
	std::string ev = Collect(4000);
	out("WSAAsyncSelect", "connect.refused", "%s %s", WsaErr(r).c_str(), ev.c_str());
	out("WSAAsyncSelect", "~connect.refused.ms", "%lu", ::GetTickCount() - t0);
	r = ::connect(d, (sockaddr*)&dead, sizeof dead);
	out("connect", "again.after.refused", "%s", WsaErr(r).c_str());
	int soerr = -1, len = sizeof soerr;
	::getsockopt(d, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
	out("getsockopt", "so_error.after.refused", "%d", soerr);
	::closesocket(d);
	// WSAAsyncSelect on a socket that is not a socket.
	r = ::WSAAsyncSelect((SOCKET)0x1234, SockWindow(), WM_SOCK, FD_READ);
	out("WSAAsyncSelect", "notsocket", "%s", WsaErr(r).c_str());
	SOCKET t = ::socket(AF_INET, SOCK_STREAM, 0);
	r = ::WSAAsyncSelect(t, NULL, WM_SOCK, FD_READ);
	out("WSAAsyncSelect", "nullwindow", "%s", WsaErr(r).c_str());
	r = ::WSAAsyncSelect(t, SockWindow(), WM_SOCK, 0x7FFFFFFF);
	out("WSAAsyncSelect", "allbits", "%s", WsaErr(r).c_str());
	::closesocket(t);
}

TEST(net_EventSelect)
{
	// WebSocket.cpp:296-348 - WSAEventSelect(FD_READ|FD_CLOSE|FD_WRITE) on an
	// auto-reset event, WaitForMultipleObjects, then WSAEnumNetworkEvents with
	// a NULL event handle.
	Startup();
	Pair p = MakePair();
	HANDLE ev = ::CreateEventW(NULL, FALSE, TRUE, NULL);
	int r = ::WSAEventSelect(p.s, ev, FD_READ | FD_CLOSE | FD_WRITE);
	out("WSAEventSelect", "select", "%d", r);
	auto snap = [&](const char *cas) {
		DWORD w = ::WaitForSingleObject(ev, 300);
		WSANETWORKEVENTS ne = {};
		int e = ::WSAEnumNetworkEvents(p.s, NULL, &ne);
		std::string errs;
		for (int i = 0; i < FD_MAX_EVENTS; ++i)
			if (ne.lNetworkEvents & (1 << i))
				errs += Fmt(" %s=%d", EvName(1 << i).c_str(), ne.iErrorCode[i]);
		out("WSAEnumNetworkEvents", cas, "wait=%lu r=%d ev=%lx%s", w, e, ne.lNetworkEvents, errs.c_str());
	};
	snap("initial");
	snap("initial.again");
	::send(p.c, "12345", 5, 0);
	snap("data");
	char buf[64];
	::recv(p.s, buf, 2, 0);
	snap("partialread");
	::recv(p.s, buf, sizeof buf, 0);
	snap("fullread");
	::closesocket(p.c);
	p.c = INVALID_SOCKET;
	snap("peerclose");
	snap("peerclose.again");
	r = ::recv(p.s, buf, sizeof buf, 0);
	out("WSAEventSelect", "recv.after.close", "%s", WsaErr(r).c_str());
	// Non-blocking is implied by WSAEventSelect.
	Pair q = MakePair();
	::WSAEventSelect(q.s, ev, FD_READ);
	r = ::recv(q.s, buf, sizeof buf, 0);
	out("WSAEventSelect", "implies.nonblocking", "%s", WsaErr(r).c_str());
	u_long nb = 0;
	r = ::ioctlsocket(q.s, FIONBIO, &nb);
	out("WSAEventSelect", "fionbio.off.refused", "%s", WsaErr(r).c_str());
	r = ::WSAEnumNetworkEvents(q.s, (WSAEVENT)(ULONG_PTR)0x1234, NULL);
	out("WSAEnumNetworkEvents", "nullstruct", "%s", WsaErr(r).c_str());
	r = ::WSAEnumNetworkEvents((SOCKET)0x1234, NULL, (LPWSANETWORKEVENTS)buf);
	out("WSAEnumNetworkEvents", "notsocket", "%s", WsaErr(r).c_str());
	Close(q);
	Close(p);
	::CloseHandle(ev);
}

static int CALLBACK CondAccept(LPWSABUF caller, LPWSABUF callerData, LPQOS, LPQOS, LPWSABUF callee, LPWSABUF calleeData, GROUP *g, DWORD_PTR cb)
{
	sockaddr_in *a = caller ? (sockaddr_in*)caller->buf : NULL;
	out("WSAAccept", "condition.args", "caller_len=%lu family=%d addr=%08lx callerdata=%s callee_len=%lu calleedata=%s g=%s cb=%Iu",
		caller ? caller->len : 0, a ? a->sin_family : -1, a ? a->sin_addr.s_addr : 0, callerData ? (callerData->len ? "len" : "empty") : "NULL",
		callee ? callee->len : 0, calleeData ? (calleeData->len ? "len" : "empty") : "NULL", g ? "ptr" : "NULL", cb);
	return (int)(cb & 0xF);
}

TEST(net_WSAAccept)
{
	// ListenSocket.cpp:2026 - conditional accept, CF_REJECT for banned and
	// filtered IPs, and how the rejected peer sees it.
	Startup();
	char buf[16];
	for (DWORD_PTR verdict : {(DWORD_PTR)CF_ACCEPT, (DWORD_PTR)CF_REJECT, (DWORD_PTR)CF_DEFER, (DWORD_PTR)7}) {
		SOCKET l = Listener();
		SOCKET c = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a = Loop(Port(l));
		int cr = ::connect(c, (sockaddr*)&a, sizeof a);
		sockaddr_in pa = {};
		int plen = sizeof pa;
		SOCKET s = ::WSAAccept(l, (sockaddr*)&pa, &plen, CondAccept, verdict | 0x100);
		int e = ::WSAGetLastError();
		out("WSAAccept", Fmt("verdict%Iu", verdict).c_str(), "connect=%d result=%s wsa=%d plen=%d", cr, s != INVALID_SOCKET ? "socket" : "INVALID",
			s != INVALID_SOCKET ? 0 : e, plen);
		::Sleep(100);
		int r = ::send(c, "x", 1, 0);
		std::string sr = WsaErr(r);
		::Sleep(100);
		r = ::recv(c, buf, sizeof buf, 0);
		out("WSAAccept", Fmt("verdict%Iu.peer", verdict).c_str(), "send=%s recv=%s", sr.c_str(), WsaErr(r).c_str());
		// Is the connection still queued after a reject / defer?
		u_long nb = 1;
		::ioctlsocket(l, FIONBIO, &nb);
		SOCKET again = ::accept(l, NULL, NULL);
		out("WSAAccept", Fmt("verdict%Iu.requeued", verdict).c_str(), "%s", again != INVALID_SOCKET ? "yes" : Fmt("no wsa=%d", ::WSAGetLastError()).c_str());
		if (again != INVALID_SOCKET)
			::closesocket(again);
		if (s != INVALID_SOCKET)
			::closesocket(s);
		::closesocket(c);
		::closesocket(l);
	}
	SOCKET l = Listener();
	u_long nb = 1;
	::ioctlsocket(l, FIONBIO, &nb);
	SOCKET s = ::WSAAccept(l, NULL, NULL, CondAccept, 0);
	out("WSAAccept", "nonblocking.empty", "%s wsa=%d", s != INVALID_SOCKET ? "socket" : "INVALID", ::WSAGetLastError());
	s = ::accept(l, NULL, NULL);
	out("accept", "nonblocking.empty", "%s wsa=%d", s != INVALID_SOCKET ? "socket" : "INVALID", ::WSAGetLastError());
	SOCKET c = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a = Loop(Port(l));
	::connect(c, (sockaddr*)&a, sizeof a);
	::Sleep(100);
	sockaddr_in pa;
	int plen = 4;
	s = ::accept(l, (sockaddr*)&pa, &plen);
	out("accept", "shortaddrlen", "%s wsa=%d", s != INVALID_SOCKET ? "socket" : "INVALID", ::WSAGetLastError());
	if (s != INVALID_SOCKET)
		::closesocket(s);
	s = ::accept(l, NULL, NULL);
	out("accept", "after.shortaddrlen", "%s", s != INVALID_SOCKET ? "socket" : Fmt("INVALID wsa=%d", ::WSAGetLastError()).c_str());
	if (s != INVALID_SOCKET)
		::closesocket(s);
	::closesocket(c);
	SOCKET un = ::socket(AF_INET, SOCK_STREAM, 0);
	s = ::accept(un, NULL, NULL);
	out("accept", "notlistening", "%s wsa=%d", s != INVALID_SOCKET ? "socket" : "INVALID", ::WSAGetLastError());
	::closesocket(un);
	::closesocket(l);
}

TEST(net_SockOpts)
{
	Startup();
	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	auto geti = [&](int level, int opt, const char *name) {
		int v = -12345, len = sizeof v;
		int r = ::getsockopt(s, level, opt, (char*)&v, &len);
		return Fmt("%s=%s/%d/len%d", name, r ? Fmt("err%d", ::WSAGetLastError()).c_str() : "ok", v, len);
	};
	out("getsockopt", "tcp.defaults", "%s %s %s %s %s", geti(SOL_SOCKET, SO_TYPE, "type").c_str(), geti(SOL_SOCKET, SO_ERROR, "error").c_str(),
		geti(IPPROTO_TCP, TCP_NODELAY, "nodelay").c_str(), geti(SOL_SOCKET, SO_KEEPALIVE, "keepalive").c_str(),
		geti(SOL_SOCKET, SO_REUSEADDR, "reuse").c_str());
	out("getsockopt", "~tcp.buffers", "%s %s", geti(SOL_SOCKET, SO_RCVBUF, "rcv").c_str(), geti(SOL_SOCKET, SO_SNDBUF, "snd").c_str());
	linger lg = {7, 7};
	int len = sizeof lg;
	int r = ::getsockopt(s, SOL_SOCKET, SO_LINGER, (char*)&lg, &len);
	out("getsockopt", "linger.default", "r=%d onoff=%u time=%u", r, lg.l_onoff, lg.l_linger);
	for (int v : {0, 1, 4096, 8192, 65536, 1 << 20}) {
		::setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&v, sizeof v);
		int g = -1;
		len = sizeof g;
		::getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&g, &len);
		::setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof v);
		int g2 = -1;
		len = sizeof g2;
		::getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&g2, &len);
		out("setsockopt", Fmt("buffers.%d", v).c_str(), "snd=%d rcv=%d", g, g2);
	}
	BOOL on = TRUE;
	r = ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
	out("setsockopt", "nodelay", "r=%d %s", r, geti(IPPROTO_TCP, TCP_NODELAY, "nodelay").c_str());
	char one = 1;
	r = ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, 1);
	out("setsockopt", "nodelay.1byte", "%s", WsaErr(r).c_str());
	int v = 1;
	r = ::getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&v, &(len = 2));
	out("getsockopt", "shortlen", "%s len=%d", WsaErr(r).c_str(), len);
	r = ::setsockopt(s, SOL_SOCKET, 0x7777, (const char*)&v, sizeof v);
	out("setsockopt", "unknown", "%s", WsaErr(r).c_str());
	r = ::getsockopt(s, 0x7777, SO_TYPE, (char*)&v, &(len = sizeof v));
	out("getsockopt", "unknownlevel", "%s", WsaErr(r).c_str());
	// Pinger.cpp: SO_RCVTIMEO on a blocking socket.
	SOCKET u = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in a = Loop(0);
	::bind(u, (sockaddr*)&a, sizeof a);
	DWORD to = 300;
	r = ::setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
	char buf[16];
	DWORD t0 = ::GetTickCount();
	r = ::recv(u, buf, sizeof buf, 0);
	DWORD dt = ::GetTickCount() - t0;
	out("setsockopt", "rcvtimeo300", "%s waited_ok=%d", WsaErr(r).c_str(), dt >= 250 && dt < 2000);
	DWORD tv = 0;
	r = ::getsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, &(len = sizeof tv));
	out("getsockopt", "rcvtimeo", "r=%d %lu", r, tv);
	int ttl = 3;
	r = ::setsockopt(u, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof ttl);
	int ttl2 = 0;
	::getsockopt(u, IPPROTO_IP, IP_TTL, (char*)&ttl2, &(len = sizeof ttl2));
	out("setsockopt", "ip_ttl", "r=%d readback=%d", r, ttl2);
	ttl = 0;
	r = ::setsockopt(u, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof ttl);
	out("setsockopt", "ip_ttl.0", "%s", WsaErr(r).c_str());
	ttl = 300;
	r = ::setsockopt(u, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof ttl);
	out("setsockopt", "ip_ttl.300", "%s", WsaErr(r).c_str());
	::closesocket(u);
	::closesocket(s);
}

TEST(net_Bind)
{
	// eMule binds its TCP and UDP ports, optionally to a chosen address.
	Startup();
	auto tryBind = [](int type, u_long addr, u_short port, bool reuse, bool excl, int *err) {
		SOCKET s = ::socket(AF_INET, type, 0);
		BOOL on = TRUE;
		if (reuse)
			::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
		if (excl)
			::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof on);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = addr;
		a.sin_port = htons(port);
		int r = ::bind(s, (sockaddr*)&a, sizeof a);
		*err = r ? ::WSAGetLastError() : 0;
		return s;
	};
	int e;
	SOCKET first = tryBind(SOCK_STREAM, htonl(INADDR_LOOPBACK), 0, false, false, &e);
	u_short port = Port(first);
	::listen(first, 5);
	struct { const char *n; u_long addr; bool reuse, excl; } c[] = {
		{"same", htonl(INADDR_LOOPBACK), false, false}, {"same.reuse", htonl(INADDR_LOOPBACK), true, false},
		{"any", INADDR_ANY, false, false}, {"any.reuse", INADDR_ANY, true, false}, {"same.exclusive", htonl(INADDR_LOOPBACK), false, true}};
	for (auto &x : c) {
		SOCKET s = tryBind(SOCK_STREAM, x.addr, port, x.reuse, x.excl, &e);
		int le = 0;
		if (!e)
			le = ::listen(s, 5) ? ::WSAGetLastError() : 0;
		out("bind", Fmt("tcp.listening.%s", x.n).c_str(), "bind=%d listen=%d", e, le);
		::closesocket(s);
	}
	::closesocket(first);
	SOCKET ufirst = tryBind(SOCK_DGRAM, INADDR_ANY, 0, false, false, &e);
	u_short uport = Port(ufirst);
	for (auto &x : c) {
		SOCKET s = tryBind(SOCK_DGRAM, x.addr, uport, x.reuse, x.excl, &e);
		out("bind", Fmt("udp.%s", x.n).c_str(), "bind=%d", e);
		::closesocket(s);
	}
	::closesocket(ufirst);
	SOCKET s = tryBind(SOCK_STREAM, ::inet_addr("1.2.3.4"), 0, false, false, &e);
	out("bind", "foreignaddr", "%d", e);
	::closesocket(s);
	s = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a = Loop(0);
	::bind(s, (sockaddr*)&a, sizeof a);
	int r = ::bind(s, (sockaddr*)&a, sizeof a);
	out("bind", "twice", "%s", WsaErr(r).c_str());
	::closesocket(s);
	s = ::socket(AF_INET, SOCK_STREAM, 0);
	r = ::bind(s, (sockaddr*)&a, 4);
	out("bind", "shortlen", "%s", WsaErr(r).c_str());
	sockaddr_in6 a6 = {};
	a6.sin6_family = AF_INET6;
	r = ::bind(s, (sockaddr*)&a6, sizeof a6);
	out("bind", "wrongfamily", "%s", WsaErr(r).c_str());
	::closesocket(s);
	// TIME_WAIT: rebind a port just used by a closed connection.
	Pair p = MakePair();
	u_short cport = Port(p.c);
	::closesocket(p.c);
	p.c = INVALID_SOCKET;
	::Sleep(100);
	Close(p);
	s = tryBind(SOCK_STREAM, htonl(INADDR_LOOPBACK), cport, false, false, &e);
	out("bind", "timewait.port", "%d", e);
	::closesocket(s);
	r = ::listen(INVALID_SOCKET, 5);
	out("listen", "invalid", "%s", WsaErr(r).c_str());
	s = ::socket(AF_INET, SOCK_STREAM, 0);
	r = ::listen(s, 5);
	out("listen", "unbound", "%s", WsaErr(r).c_str());
	::closesocket(s);
	s = ::socket(AF_INET, SOCK_DGRAM, 0);
	::bind(s, (sockaddr*)&a, sizeof a);
	r = ::listen(s, 5);
	out("listen", "udp", "%s", WsaErr(r).c_str());
	::closesocket(s);
}

TEST(net_Udp)
{
	Startup();
	SOCKET a = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), b = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in la = Loop(0);
	::bind(a, (sockaddr*)&la, sizeof la);
	::bind(b, (sockaddr*)&la, sizeof la);
	sockaddr_in ta = Loop(Port(b));
	char buf[2048];
	int r = ::sendto(a, "datagram!", 9, 0, (sockaddr*)&ta, sizeof ta);
	out("sendto", "basic", "%s", WsaErr(r).c_str());
	sockaddr_in from = {};
	int flen = sizeof from;
	r = ::recvfrom(b, buf, sizeof buf, 0, (sockaddr*)&from, &flen);
	out("recvfrom", "basic", "%s from_ok=%d flen=%d", WsaErr(r).c_str(), ntohs(from.sin_port) == Port(a), flen);
	::sendto(a, "0123456789", 10, 0, (sockaddr*)&ta, sizeof ta);
	r = ::recvfrom(b, buf, 4, 0, (sockaddr*)&from, &flen);
	out("recvfrom", "truncated", "%s data=%s", WsaErr(r).c_str(), QA(buf, 4).c_str());
	u_long nb = 1;
	::ioctlsocket(b, FIONBIO, &nb);
	r = ::recvfrom(b, buf, sizeof buf, 0, (sockaddr*)&from, &flen);
	out("recvfrom", "truncated.rest_discarded", "%s", WsaErr(r).c_str());
	r = ::sendto(a, "", 0, 0, (sockaddr*)&ta, sizeof ta);
	out("sendto", "empty", "%s", WsaErr(r).c_str());
	::Sleep(50);
	r = ::recvfrom(b, buf, sizeof buf, 0, (sockaddr*)&from, &flen);
	out("recvfrom", "empty", "%s", WsaErr(r).c_str());
	::sendto(a, "x", 1, 0, (sockaddr*)&ta, sizeof ta);
	::sendto(a, "yy", 2, 0, (sockaddr*)&ta, sizeof ta);
	::Sleep(50);
	u_long avail = 0;
	::ioctlsocket(b, FIONREAD, &avail);
	out("ioctlsocket", "fionread.udp.twodatagrams", "%lu", avail);
	::recvfrom(b, buf, sizeof buf, 0, NULL, NULL);
	::recvfrom(b, buf, sizeof buf, 0, NULL, NULL);
	r = ::recvfrom(b, buf, sizeof buf, 0, (sockaddr*)&from, &(flen = 4));
	out("recvfrom", "nodata.shortfromlen", "%s", WsaErr(r).c_str());
	// ICMP port unreachable: send to a closed port, then receive.
	SOCKET dead = ::socket(AF_INET, SOCK_DGRAM, 0);
	::bind(dead, (sockaddr*)&la, sizeof la);
	sockaddr_in da = Loop(Port(dead));
	::closesocket(dead);
	::ioctlsocket(a, FIONBIO, &nb);
	r = ::sendto(a, "nobody", 6, 0, (sockaddr*)&da, sizeof da);
	out("sendto", "closedport", "%s", WsaErr(r).c_str());
	::Sleep(200);
	r = ::recvfrom(a, buf, sizeof buf, 0, (sockaddr*)&from, &(flen = sizeof from));
	out("recvfrom", "after.closedport", "%s", WsaErr(r).c_str());
	r = ::recvfrom(a, buf, sizeof buf, 0, (sockaddr*)&from, &(flen = sizeof from));
	out("recvfrom", "after.closedport.again", "%s", WsaErr(r).c_str());
	r = ::sendto(a, "again", 5, 0, (sockaddr*)&ta, sizeof ta);
	out("sendto", "after.closedport.stillworks", "%s", WsaErr(r).c_str());
	// The same through WSAAsyncSelect, as ClientUDPSocket sees it.
	Async(a, 6, FD_READ | FD_WRITE);
	Collect(200);
	::sendto(a, "nobody", 6, 0, (sockaddr*)&da, sizeof da);
	out("WSAAsyncSelect", "udp.closedport", "%s", Collect(500).c_str());
	r = ::recvfrom(a, buf, sizeof buf, 0, (sockaddr*)&from, &(flen = sizeof from));
	out("recvfrom", "udp.closedport.async", "%s", WsaErr(r).c_str());
	::sendto(b, "for-a", 5, 0, (sockaddr*)&(la = Loop(Port(a))), sizeof la);
	out("WSAAsyncSelect", "udp.datagram", "%s", Collect(300).c_str());
	r = ::recvfrom(a, buf, sizeof buf, 0, NULL, NULL);
	out("recvfrom", "udp.datagram.async", "%s", WsaErr(r).c_str());
	// Broadcast without SO_BROADCAST; oversized datagram.
	sockaddr_in bc = {};
	bc.sin_family = AF_INET;
	bc.sin_addr.s_addr = INADDR_BROADCAST;
	bc.sin_port = htons(9);
	r = ::sendto(b, "b", 1, 0, (sockaddr*)&bc, sizeof bc);
	out("sendto", "broadcast.nopermission", "%s", WsaErr(r).c_str());
	static char huge[70000];
	r = ::sendto(b, huge, sizeof huge, 0, (sockaddr*)&ta, sizeof ta);
	out("sendto", "oversize", "%s", WsaErr(r).c_str());
	r = ::sendto(b, "x", 1, 0, NULL, 0);
	out("sendto", "noaddr.unconnected", "%s", WsaErr(r).c_str());
	r = ::sendto(b, "x", 1, 0, (sockaddr*)&ta, 4);
	out("sendto", "shortaddr", "%s", WsaErr(r).c_str());
	::closesocket(a);
	::closesocket(b);
}

TEST(net_Select)
{
	// WebSocket.cpp:488 - select(0, readable, NULL, NULL, 250 ms).
	Startup();
	timeval tv = {0, 100000};
	int r = ::select(0, NULL, NULL, NULL, &tv);
	out("select", "nosets", "%s", WsaErr(r).c_str());
	fd_set rs;
	FD_ZERO(&rs);
	r = ::select(0, &rs, NULL, NULL, &tv);
	out("select", "emptyset", "%s", WsaErr(r).c_str());
	SOCKET l = Listener();
	FD_SET(l, &rs);
	DWORD t0 = ::GetTickCount();
	r = ::select(0, &rs, NULL, NULL, &tv);
	out("select", "listener.idle", "%s waited=%d", WsaErr(r).c_str(), ::GetTickCount() - t0 >= 80);
	SOCKET c = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a = Loop(Port(l));
	::connect(c, (sockaddr*)&a, sizeof a);
	FD_ZERO(&rs);
	FD_SET(l, &rs);
	r = ::select(0, &rs, NULL, NULL, &tv);
	out("select", "listener.pending", "%s isset=%d", WsaErr(r).c_str(), FD_ISSET(l, &rs));
	FD_ZERO(&rs);
	FD_SET((SOCKET)0x1234, &rs);
	r = ::select(0, &rs, NULL, NULL, &tv);
	out("select", "notsocket", "%s", WsaErr(r).c_str());
	timeval neg = {-1, 0};
	FD_ZERO(&rs);
	FD_SET(c, &rs);
	r = ::select(0, &rs, NULL, NULL, &neg);
	out("select", "negativetimeout", "%s", WsaErr(r).c_str());
	timeval bigusec = {0, 2000000};
	t0 = ::GetTickCount();
	FD_ZERO(&rs);
	FD_SET(c, &rs);
	r = ::select(0, &rs, NULL, NULL, &bigusec);
	out("select", "usec_over_1s", "%s", WsaErr(r).c_str());
	out("select", "~usec_over_1s.ms", "%lu", ::GetTickCount() - t0);
	::closesocket(c);
	// Non-blocking connect to a closed port: which set reports the failure?
	u_short dead = Port(l);
	::closesocket(l);
	c = ::socket(AF_INET, SOCK_STREAM, 0);
	u_long nb = 1;
	::ioctlsocket(c, FIONBIO, &nb);
	sockaddr_in da = Loop(dead);
	r = ::connect(c, (sockaddr*)&da, sizeof da);
	out("connect", "nonblocking.refused", "%s", WsaErr(r).c_str());
	fd_set ws, es;
	FD_ZERO(&ws);
	FD_ZERO(&es);
	FD_SET(c, &ws);
	FD_SET(c, &es);
	timeval five = {5, 0};
	r = ::select(0, NULL, &ws, &es, &five);
	out("select", "connect.refused.sets", "%s write=%d except=%d", WsaErr(r).c_str(), FD_ISSET(c, &ws), FD_ISSET(c, &es));
	::closesocket(c);
}

TEST(net_Overlapped)
{
	// EMSocket / EncryptedStreamSocket: WSASend with an OVERLAPPED,
	// WSAGetOverlappedResult(fWait = FALSE), CancelIo on the socket handle.
	Startup();
	Pair p = MakePair();
	SOCKET s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	out("WSASocketW", "overlapped", "%s", s != INVALID_SOCKET ? "ok" : "INVALID");
	::closesocket(s);
	s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);	// WebSocket.cpp:444
	out("WSASocketW", "flags0", "%s", s != INVALID_SOCKET ? "ok" : "INVALID");
	::closesocket(s);
	s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_UDP, NULL, 0, 0);
	out("WSASocketW", "mismatch", "%s wsa=%d", s != INVALID_SOCKET ? "ok" : "INVALID", ::WSAGetLastError());
	if (s != INVALID_SOCKET)
		::closesocket(s);
	WSAOVERLAPPED ov = {};
	ov.hEvent = ::WSACreateEvent();
	WSABUF wb = {5, (CHAR*)"small"};
	DWORD sent = 0, flags = 0;
	int r = ::WSASend(p.s, &wb, 1, NULL, 0, &ov, NULL);
	int e = r ? ::WSAGetLastError() : 0;
	DWORD xfer = 0;
	BOOL g = ::WSAGetOverlappedResult(p.s, &ov, &xfer, TRUE, &flags);
	out("WSASend", "small", "r=%d err=%d result=%d bytes=%lu", r, e, g, xfer);
	// Make a send pend: fill the pipe while the peer does not read.
	static char big[1 << 20];
	WSABUF bb = {sizeof big, big};
	int pendingAt = -1;
	for (int i = 0; i < 64; ++i) {
		::WSAResetEvent(ov.hEvent);
		r = ::WSASend(p.s, &bb, 1, NULL, 0, &ov, NULL);
		if (r && ::WSAGetLastError() == WSA_IO_PENDING) {
			pendingAt = i;
			break;
		}
		::WSAGetOverlappedResult(p.s, &ov, &xfer, TRUE, &flags);
	}
	out("WSASend", "eventually.pends", "%d", pendingAt >= 0);
	if (pendingAt >= 0) {
		::WSASetLastError(0);
		g = ::WSAGetOverlappedResult(p.s, &ov, &xfer, FALSE, &flags);
		out("WSAGetOverlappedResult", "incomplete", "r=%d wsa=%d", g, ::WSAGetLastError());
		BOOL c = ::CancelIo((HANDLE)p.s);
		g = ::WSAGetOverlappedResult(p.s, &ov, &xfer, TRUE, &flags);
		int ge = ::WSAGetLastError();
		out("CancelIo", "pending.wsasend", "cancel=%d result=%d wsa=%d", c, g, g ? 0 : ge);
		out("CancelIo", "~pending.wsasend.bytes", "%lu", xfer);
	}
	// A WSASend that pends, then the peer resets the connection.
	Pair q = MakePair();
	for (int i = 0; i < 64; ++i) {
		::WSAResetEvent(ov.hEvent);
		r = ::WSASend(q.s, &bb, 1, NULL, 0, &ov, NULL);
		if (r && ::WSAGetLastError() == WSA_IO_PENDING)
			break;
		::WSAGetOverlappedResult(q.s, &ov, &xfer, TRUE, &flags);
	}
	Abort(q.c);
	q.c = INVALID_SOCKET;
	g = ::WSAGetOverlappedResult(q.s, &ov, &xfer, TRUE, &flags);
	out("WSAGetOverlappedResult", "pending.then.peerreset", "result=%d wsa=%d", g, g ? 0 : ::WSAGetLastError());
	r = ::WSASend(q.s, &wb, 1, &sent, 0, NULL, NULL);
	out("WSASend", "after.reset", "%s", WsaErr(r).c_str());
	Close(q);
	::WSACloseEvent(ov.hEvent);
	Close(p);
	r = ::WSASend((SOCKET)0x1234, &wb, 1, &sent, 0, NULL, NULL);
	out("WSASend", "notsocket", "%s", WsaErr(r).c_str());
}

TEST(net_SocketCreate)
{
	Startup();
	struct { const char *n; int af, type, proto; } c[] = {
		{"tcp", AF_INET, SOCK_STREAM, 0}, {"udp", AF_INET, SOCK_DGRAM, IPPROTO_UDP}, {"tcp6", AF_INET6, SOCK_STREAM, 0},
		{"badaf", 12345, SOCK_STREAM, 0}, {"badtype", AF_INET, 77, 0}, {"stream.udp", AF_INET, SOCK_STREAM, IPPROTO_UDP},
		{"unspec", AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP}};
	for (auto &x : c) {
		SOCKET s = ::socket(x.af, x.type, x.proto);
		out("socket", x.n, "%s wsa=%d", s != INVALID_SOCKET ? "ok" : "INVALID", s != INVALID_SOCKET ? 0 : ::WSAGetLastError());
		if (s != INVALID_SOCKET)
			::closesocket(s);
	}
	// Pinger.cpp:126 - raw ICMP; depends on privilege, so marked '~'.
	SOCKET s = ::socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	out("socket", "~raw.icmp", "%s wsa=%d", s != INVALID_SOCKET ? "ok" : "INVALID", s != INVALID_SOCKET ? 0 : ::WSAGetLastError());
	if (s != INVALID_SOCKET)
		::closesocket(s);
}

TEST(net_IpHelperTables)
{
	// Preferences.cpp GetRandomTCPPort / GetRandomUDPPort.
	Startup();
	Pair p = MakePair();
	SOCKET u = ::socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a = Loop(0);
	::bind(u, (sockaddr*)&a, sizeof a);
	ULONG size = 0;
	DWORD r = ::GetTcpTable(NULL, &size, FALSE);
	out("GetTcpTable", "query", "r=%lu size_nonzero=%d", r, size > 0);
	size += sizeof(MIB_TCPROW) * 50;
	std::vector<BYTE> buf(size);
	PMIB_TCPTABLE t = (PMIB_TCPTABLE)buf.data();
	r = ::GetTcpTable(t, &size, TRUE);
	bool listen = false, est = false, sorted = true;
	for (DWORD i = 0; r == NO_ERROR && i < t->dwNumEntries; ++i) {
		const MIB_TCPROW &row = t->table[i];
		if (row.dwLocalPort == htons(Port(p.l)) && row.dwState == MIB_TCP_STATE_LISTEN)
			listen = true;
		if (row.dwLocalPort == htons(Port(p.c)) && row.dwState == MIB_TCP_STATE_ESTAB)
			est = true;
		if (i) {
			const MIB_TCPROW &pr = t->table[i - 1];
			if (ntohl(pr.dwLocalAddr) > ntohl(row.dwLocalAddr) ||
				(pr.dwLocalAddr == row.dwLocalAddr && ntohs((u_short)pr.dwLocalPort) > ntohs((u_short)row.dwLocalPort)))
				sorted = false;
		}
	}
	out("GetTcpTable", "fill", "r=%lu our_listener=%d our_established=%d sorted=%d", r, listen, est, sorted);
	ULONG tiny = 4;
	r = ::GetTcpTable(t, &tiny, FALSE);
	out("GetTcpTable", "tiny", "r=%lu", r);
	r = ::GetTcpTable(NULL, NULL, FALSE);
	out("GetTcpTable", "nullsize", "r=%lu", r);
	size = 0;
	r = ::GetUdpTable(NULL, &size, FALSE);
	out("GetUdpTable", "query", "r=%lu size_nonzero=%d", r, size > 0);
	size += sizeof(MIB_UDPROW) * 50;
	buf.assign(size, 0);
	PMIB_UDPTABLE ut = (PMIB_UDPTABLE)buf.data();
	r = ::GetUdpTable(ut, &size, TRUE);
	bool found = false;
	sorted = true;
	for (DWORD i = 0; r == NO_ERROR && i < ut->dwNumEntries; ++i) {
		if (ut->table[i].dwLocalPort == htons(Port(u)))
			found = true;
		if (i && (ntohl(ut->table[i - 1].dwLocalAddr) > ntohl(ut->table[i].dwLocalAddr) ||
				(ut->table[i - 1].dwLocalAddr == ut->table[i].dwLocalAddr && ntohs((u_short)ut->table[i - 1].dwLocalPort) > ntohs((u_short)ut->table[i].dwLocalPort))))
			sorted = false;
	}
	out("GetUdpTable", "fill", "r=%lu our_socket=%d sorted=%d", r, found, sorted);
	// Pinger.cpp:376 - error strings with a 511-char buffer.
	for (DWORD code : {11010ul, 11013ul, 11003ul, 11050ul, 0ul, 99999ul}) {
		WCHAR s[512];
		DWORD n = 511;
		r = ::GetIpErrorString(code, s, &n);
		out("GetIpErrorString", Fmt("%lu", code).c_str(), "r=%lu", r);
		if (r == NO_ERROR)
			out("GetIpErrorString", Fmt("~%lu.text", code).c_str(), "%s", Q(s).c_str());
	}
	WCHAR s2[4];
	DWORD n = 3;
	r = ::GetIpErrorString(11010, s2, &n);
	out("GetIpErrorString", "small", "r=%lu n=%lu", r, n);
	::closesocket(u);
	Close(p);
}

TEST_T(net_Icmp, 60000)
{
	// Pinger.cpp: IcmpCreateFile, IcmpSendEcho with no data and a TTL,
	// IcmpCloseHandle.
	HANDLE h = ::IcmpCreateFile();
	out("IcmpCreateFile", "create", "%s", h != INVALID_HANDLE_VALUE ? "ok" : Fmt("INVALID err=%lu", ::GetLastError()).c_str());
	if (h == INVALID_HANDLE_VALUE)
		return;
	char reply[sizeof(ICMP_ECHO_REPLY) + 64];
	IP_OPTION_INFORMATION opt = {};
	opt.Ttl = 64;
	::SetLastError(0);
	DWORD n = ::IcmpSendEcho(h, htonl(INADDR_LOOPBACK), NULL, 0, &opt, reply, sizeof reply, 3000);
	const ICMP_ECHO_REPLY &rp = *(ICMP_ECHO_REPLY*)reply;
	out("IcmpSendEcho", "loopback.nodata", "n=%lu err=%lu status=%lu addr=%08lx size=%u", n, n ? 0 : ::GetLastError(), n ? rp.Status : 0, n ? rp.Address : 0, n ? rp.DataSize : 0);
	out("IcmpSendEcho", "~loopback.nodata.rtt_ttl", "%lu %u", n ? rp.RoundTripTime : 0, n ? rp.Options.Ttl : 0);
	char data[32] = "symrepro ping payload";
	n = ::IcmpSendEcho(h, htonl(INADDR_LOOPBACK), data, 22, NULL, reply, sizeof reply, 3000);
	out("IcmpSendEcho", "loopback.data", "n=%lu status=%lu size=%u echoed=%d", n, n ? rp.Status : 0, n ? rp.DataSize : 0,
		n && rp.DataSize == 22 && rp.Data && !memcmp(rp.Data, data, 22));
	::SetLastError(0);
	n = ::IcmpSendEcho(h, htonl(INADDR_LOOPBACK), NULL, 0, &opt, reply, 8, 1000);
	out("IcmpSendEcho", "smallreply", "n=%lu err=%lu", n, ::GetLastError());
	// Through the network, as eMule's upload speed sense does: TTL 1 towards
	// a public address must come back from the first hop as TTL expired.
	opt.Ttl = 1;
	::SetLastError(0);
	n = ::IcmpSendEcho(h, ::inet_addr("8.8.8.8"), NULL, 0, &opt, reply, sizeof reply, 3000);
	out("IcmpSendEcho", "~ttl1.public", "n=%lu err=%lu status=%lu", n, n ? 0 : ::GetLastError(), n ? rp.Status : 0);
	::SetLastError(0);
	n = ::IcmpSendEcho(h, ::inet_addr("192.0.2.1"), NULL, 0, NULL, reply, sizeof reply, 1000);	// TEST-NET, unroutable
	out("IcmpSendEcho", "~testnet.timeout", "n=%lu err=%lu", n, n ? 0 : ::GetLastError());
	BOOL c = ::IcmpCloseHandle(h);
	out("IcmpCloseHandle", "close", "%s", B(c).c_str());
	::SetLastError(0);
	c = ::IcmpCloseHandle(h);
	out("IcmpCloseHandle", "twice", "%s", B(c).c_str());
}
