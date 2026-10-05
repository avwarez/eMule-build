// WinINet as CHttpDownloadDlg and the URL parsers use it, against a sbuf
// HTTP server running in this process on loopback - no Internet involved.
#include "harness.h"
#include <wininet.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The server

struct Server
{
	SOCKET l = INVALID_SOCKET;
	u_short port = 0;
	HANDLE th = NULL;
	volatile LONG stop = 0;
	std::string lastRequest;
	CRITICAL_SECTION cs;
};

static std::string Response(const std::string &path)
{
	static const char gz[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\xcb\x48\xcd\xc9\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00";
	if (path == "/ok")
		return "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Type: text/plain\r\n\r\nhello world";
	if (path == "/gzip")
		return std::string("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 25\r\n\r\n") + std::string(gz, 25);
	if (path == "/xgzip")
		return std::string("HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 25\r\n\r\n") + std::string(gz, 25);
	if (path == "/chunked")
		return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
	if (path == "/redirect")
		return "HTTP/1.1 302 Found\r\nLocation: /ok\r\nContent-Length: 0\r\n\r\n";
	if (path == "/redirect301abs")
		return "HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1:PORT/ok\r\nContent-Length: 0\r\n\r\n";
	if (path == "/auth")
		return "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"symrepro\"\r\nContent-Length: 4\r\n\r\ndeny";
	if (path == "/404")
		return "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nnot";
	if (path == "/http10")
		return "HTTP/1.0 200 OK\r\n\r\nbody until close";
	if (path == "/badlength")
		return "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\nxyz";
	if (path == "/big")
		return "HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n" + std::string(100000, 'b');
	if (path == "/short")
		return "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly twenty bytes...";
	if (path == "/lfonly")
		return "HTTP/1.1 200 OK\nContent-Length: 2\n\nok";
	if (path == "/close")
		return "";
	if (path == "/garbage")
		return "this is not http at all\r\n\r\n";
	return "HTTP/1.1 500 Unknown\r\nContent-Length: 0\r\n\r\n";
}

static DWORD WINAPI ServerThread(LPVOID p)
{
	Server *s = (Server*)p;
	while (!s->stop) {
		fd_set rs;
		FD_ZERO(&rs);
		FD_SET(s->l, &rs);
		timeval tv = {0, 100000};
		if (::select(0, &rs, NULL, NULL, &tv) <= 0)
			continue;
		SOCKET c = ::accept(s->l, NULL, NULL);
		if (c == INVALID_SOCKET)
			continue;
		// Serve requests on this connection until the client closes it.
		std::string in;
		for (;;) {
			size_t end;
			while ((end = in.find("\r\n\r\n")) == std::string::npos) {
				char buf[4096];
				int n = ::recv(c, buf, sizeof buf, 0);
				if (n <= 0)
					goto done;
				in.append(buf, n);
			}
			std::string req = in.substr(0, end + 4);
			in.erase(0, end + 4);
			::EnterCriticalSection(&s->cs);
			s->lastRequest = req;
			::LeaveCriticalSection(&s->cs);
			size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
			std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
			std::string resp = Response(path);
			size_t pp = resp.find("PORT");
			if (pp != std::string::npos)
				resp.replace(pp, 4, Fmt("%u", s->port));
			if (resp.empty())
				break;
			::send(c, resp.data(), (int)resp.size(), 0);
			if (path == "/http10" || path == "/short" || path == "/garbage")
				break;
		}
	done:
		::shutdown(c, SD_SEND);
		::closesocket(c);
	}
	return 0;
}

static Server *StartServer()
{
	WSADATA wd;
	::WSAStartup(MAKEWORD(2, 2), &wd);
	Server *s = new Server;
	::InitializeCriticalSection(&s->cs);
	s->l = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	::bind(s->l, (sockaddr*)&a, sizeof a);
	::listen(s->l, 16);
	int n = sizeof a;
	::getsockname(s->l, (sockaddr*)&a, &n);
	s->port = ntohs(a.sin_port);
	s->th = ::CreateThread(NULL, 0, ServerThread, s, 0, NULL);
	return s;
}

static void StopServer(Server *s)
{
	s->stop = 1;
	::WaitForSingleObject(s->th, 5000);
	::closesocket(s->l);
	::CloseHandle(s->th);
}

// ---------------------------------------------------------------------------
// The client, shaped like CHttpDownloadDlg::DownloadThread.

static std::string s_status;
static std::string s_unicodeInfo;

static void CALLBACK StatusCb(HINTERNET, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len)
{
	if (ctx != 0x5150)
		return;
	if (!s_status.empty() && s_status.back() != ' ')
		s_status += ' ';
	s_status += Fmt("%lu", status);
	if (status == INTERNET_STATUS_RESOLVING_NAME || status == INTERNET_STATUS_NAME_RESOLVED ||
		status == INTERNET_STATUS_CONNECTING_TO_SERVER || status == INTERNET_STATUS_CONNECTED_TO_SERVER) {
		INT f = IS_TEXT_UNICODE_UNICODE_MASK;
		BOOL u = info && len ? ::IsTextUnicode(info, len, &f) : FALSE;
		s_unicodeInfo += Fmt(" %lu:%s", status, u ? "W" : "A");
	}
}

static std::string Query(HINTERNET h, DWORD what)
{
	wchar_t buf[256];
	DWORD sz = sizeof buf;	// bytes, as eMule passes _countof() - see the next test
	::SetLastError(0);
	if (!::HttpQueryInfoW(h, what, buf, &sz, NULL))
		return Fmt("fail(%lu)", ::GetLastError());
	return Q(buf);
}

static void Fetch(Server *srv, const char *cas, const wchar_t *object, bool readAll = true)
{
	s_status.clear();
	s_unicodeInfo.clear();
	HINTERNET ses = ::InternetOpenW(L"symrepro", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	INTERNET_STATUS_CALLBACK prev = ::InternetSetStatusCallbackW(ses, StatusCb);
	HINTERNET con = ::InternetConnectW(ses, L"127.0.0.1", srv->port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0x5150);
	static LPCWSTR accept[2] = {L"*/*", NULL};
	HINTERNET req = ::HttpOpenRequestW(con, NULL, object, NULL, NULL, accept,
		INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE | INTERNET_FLAG_KEEP_CONNECTION, 0x5150);
	::HttpAddRequestHeadersW(req, L"Accept-Encoding: gzip, x-gzip\r\n", _UI32_MAX, HTTP_ADDREQ_FLAG_ADD);
	::HttpAddRequestHeadersW(req, L"User-Agent: Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 6.0; SLCC1)\r\n", _UI32_MAX, HTTP_ADDREQ_FLAG_ADD);
	BOOL sent = ::HttpSendRequestW(req, NULL, 0, NULL, 0);
	DWORD se = sent ? 0 : ::GetLastError();
	out("HttpSendRequestW", cas, "%s", sent ? "ok" : Fmt("fail err=%lu", se).c_str());
	if (sent) {
		out("HttpQueryInfoW", Fmt("%s.status", cas).c_str(), "%s", Query(req, HTTP_QUERY_STATUS_CODE).c_str());
		out("HttpQueryInfoW", Fmt("%s.encoding", cas).c_str(), "%s", Query(req, HTTP_QUERY_CONTENT_ENCODING).c_str());
		out("HttpQueryInfoW", Fmt("%s.length", cas).c_str(), "%s", Query(req, HTTP_QUERY_CONTENT_LENGTH).c_str());
		DWORD num = 0, nsz = sizeof num;
		BOOL nr = ::HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &num, &nsz, NULL);
		out("HttpQueryInfoW", Fmt("%s.status.number", cas).c_str(), "r=%d %lu", nr, num);
		if (readAll) {
			std::string body;
			char buf[1024];
			DWORD n = 0;
			BOOL rr;
			int calls = 0;
			do {
				rr = ::InternetReadFile(req, buf, sizeof buf, &n);
				if (rr)
					body.append(buf, n);
				++calls;
			} while (rr && n && calls < 1000);
			DWORD re = rr ? 0 : ::GetLastError();
			out("InternetReadFile", cas, "%s len=%u data=%s", rr ? "ok" : Fmt("fail err=%lu", re).c_str(), (unsigned)body.size(),
				body.size() <= 40 ? QA(body.data(), body.size()).c_str() : Hex(body.data(), 8).c_str());
		}
	}
	// What the server saw: the request line and which headers were sent.
	::Sleep(50);
	::EnterCriticalSection(&srv->cs);
	std::string r = srv->lastRequest;
	srv->lastRequest.clear();
	::LeaveCriticalSection(&srv->cs);
	std::string line = r.substr(0, r.find("\r\n"));
	std::string names;
	for (size_t p = r.find("\r\n"); p != std::string::npos && p + 2 < r.size();) {
		size_t e = r.find("\r\n", p + 2);
		std::string h = r.substr(p + 2, e - p - 2);
		size_t colon = h.find(':');
		if (colon != std::string::npos)
			names += " " + h.substr(0, colon);
		p = e;
	}
	out("HttpOpenRequestW", Fmt("%s.wire", cas).c_str(), "%s |%s", QA(line.c_str()).c_str(), names.c_str());
	size_t ua = r.find("User-Agent:");
	out("HttpAddRequestHeadersW", Fmt("%s.useragent", cas).c_str(), "%s count=%d",
		ua != std::string::npos ? QA(r.substr(ua, r.find("\r\n", ua) - ua).c_str()).c_str() : "none",
		(int)(ua != std::string::npos) + (int)(ua != std::string::npos && r.find("User-Agent:", ua + 1) != std::string::npos));
	out("InternetSetStatusCallbackW", Fmt("%s.sequence", cas).c_str(), "prev=%s %s", prev == NULL ? "NULL" : "other", s_status.c_str());
	out("IsTextUnicode", Fmt("%s.statusinfo", cas).c_str(), "%s", s_unicodeInfo.empty() ? "-" : s_unicodeInfo.c_str() + 1);
	out("InternetCloseHandle", cas, "%s %s %s", B(::InternetCloseHandle(req)).c_str(), B(::InternetCloseHandle(con)).c_str(), B(::InternetCloseHandle(ses)).c_str());
}

TEST_T(inet_Download, 120000)
{
	Server *srv = StartServer();
	for (const wchar_t *o : {L"/ok", L"/gzip", L"/xgzip", L"/chunked", L"/redirect", L"/redirect301abs", L"/404", L"/http10",
			L"/badlength", L"/big", L"/short", L"/lfonly", L"/close", L"/garbage", L"/path with space", L"/\u00e9t\u00e9"}) {
		Fetch(srv, Narrow(o).c_str(), o);
	}
	StopServer(srv);
}

TEST_T(inet_Auth, 90000)
{
	// The 401 branch: drain the body, then InternetErrorDlg with eMule's
	// flags; a dialog, if any, is cancelled by the DialogCloser.
	Server *srv = StartServer();
	HINTERNET ses = ::InternetOpenW(L"symrepro", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	HINTERNET con = ::InternetConnectW(ses, L"127.0.0.1", srv->port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
	HINTERNET req = ::HttpOpenRequestW(con, NULL, L"/auth", NULL, NULL, NULL, INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE | INTERNET_FLAG_KEEP_CONNECTION, 0);
	BOOL sent = ::HttpSendRequestW(req, NULL, 0, NULL, 0);
	out("HttpSendRequestW", "auth", "%s status=%s", B(sent).c_str(), Query(req, HTTP_QUERY_STATUS_CODE).c_str());
	char d[51];
	DWORD n;
	int loops = 0;
	do {
		::InternetReadFile(req, d, 50, &n);
	} while (n != 0 && ++loops < 100);
	out("InternetReadFile", "auth.drain", "loops=%d", loops);
	HWND owner = ::CreateWindowW(L"STATIC", L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
	{
		DialogCloser dc(500);
		DWORD r = ::InternetErrorDlg(owner, req, ERROR_INTERNET_INCORRECT_PASSWORD,
			FLAGS_ERROR_UI_FILTER_FOR_ERRORS | FLAGS_ERROR_UI_FLAGS_GENERATE_DATA | FLAGS_ERROR_UI_FLAGS_CHANGE_OPTIONS, NULL);
		out("InternetErrorDlg", "incorrect_password", "r=%lu dialog=%s", r, dc.Seen() != "none" ? "shown" : "none");
		out("InternetErrorDlg", "~incorrect_password.classes", "%s", dc.Seen().c_str());
	}
	{
		DialogCloser dc(500);
		DWORD r = ::InternetErrorDlg(owner, req, ERROR_INTERNET_INCORRECT_PASSWORD, FLAGS_ERROR_UI_FLAGS_NO_UI, NULL);
		out("InternetErrorDlg", "noui", "r=%lu dialog=%s", r, dc.Seen() != "none" ? "shown" : "none");
	}
	DWORD r = ::InternetErrorDlg(NULL, req, ERROR_INTERNET_INCORRECT_PASSWORD, FLAGS_ERROR_UI_FILTER_FOR_ERRORS, NULL);
	out("InternetErrorDlg", "nullwindow", "r=%lu", r);
	r = ::InternetErrorDlg(owner, NULL, 12345, 0, NULL);
	out("InternetErrorDlg", "unknownerror", "r=%lu", r);
	::DestroyWindow(owner);
	::InternetCloseHandle(req);
	::InternetCloseHandle(con);
	::InternetCloseHandle(ses);
	StopServer(srv);
}

TEST(inet_HandlesAndErrors)
{
	HINTERNET ses = ::InternetOpenW(L"symrepro", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	out("InternetOpenW", "preconfig", "%s", ses ? "ok" : Fmt("NULL err=%lu", ::GetLastError()).c_str());
	HINTERNET bad = ::InternetOpenW(L"symrepro", 77, NULL, NULL, 0);
	out("InternetOpenW", "badaccesstype", "%s", bad ? "ok" : Fmt("NULL err=%lu", ::GetLastError()).c_str());
	if (bad)
		::InternetCloseHandle(bad);
	INTERNET_STATUS_CALLBACK p1 = ::InternetSetStatusCallbackW(ses, StatusCb);
	INTERNET_STATUS_CALLBACK p2 = ::InternetSetStatusCallbackW(ses, NULL);
	out("InternetSetStatusCallbackW", "set.reset", "first=%s second=%s", p1 == NULL ? "NULL" : p1 == INTERNET_INVALID_STATUS_CALLBACK ? "INVALID" : "other",
		p2 == StatusCb ? "prev" : "other");
	INTERNET_STATUS_CALLBACK p3 = ::InternetSetStatusCallbackW(NULL, StatusCb);
	out("InternetSetStatusCallbackW", "nullhandle", "%s err=%lu", p3 == INTERNET_INVALID_STATUS_CALLBACK ? "INVALID" : "other", ::GetLastError());
	// A port nobody listens on.
	WSADATA wd;
	::WSAStartup(MAKEWORD(2, 2), &wd);
	SOCKET t = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	::bind(t, (sockaddr*)&a, sizeof a);
	int alen = sizeof a;
	::getsockname(t, (sockaddr*)&a, &alen);
	::closesocket(t);
	HINTERNET con = ::InternetConnectW(ses, L"127.0.0.1", ntohs(a.sin_port), NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
	out("InternetConnectW", "lazy", "%s", con ? "ok" : Fmt("NULL err=%lu", ::GetLastError()).c_str());
	HINTERNET req = ::HttpOpenRequestW(con, NULL, L"/x", NULL, NULL, NULL, INTERNET_FLAG_RELOAD, 0);
	BOOL s = ::HttpSendRequestW(req, NULL, 0, NULL, 0);
	out("HttpSendRequestW", "refused", "%s", s ? "ok" : Fmt("fail err=%lu", ::GetLastError()).c_str());
	::InternetCloseHandle(req);
	::InternetCloseHandle(con);
	con = ::InternetConnectW(ses, L"nosuchhost.invalid", 80, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
	req = ::HttpOpenRequestW(con, NULL, L"/x", NULL, NULL, NULL, INTERNET_FLAG_RELOAD, 0);
	s = ::HttpSendRequestW(req, NULL, 0, NULL, 0);
	out("HttpSendRequestW", "unresolvable", "%s", s ? "ok" : Fmt("fail err=%lu", ::GetLastError()).c_str());
	// Header buffer semantics.
	DWORD n = 0;
	wchar_t sbuf[2];
	DWORD sz = sizeof sbuf;
	::SetLastError(0);
	BOOL q = ::HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE, sbuf, &sz, NULL);
	out("HttpQueryInfoW", "unsent", "r=%d err=%lu", q, ::GetLastError());
	::InternetCloseHandle(req);
	::InternetCloseHandle(con);
	BOOL ah = ::HttpAddRequestHeadersW(NULL, L"X: y\r\n", _UI32_MAX, HTTP_ADDREQ_FLAG_ADD);
	out("HttpAddRequestHeadersW", "nullhandle", "%s", B(ah).c_str());
	out("InternetCloseHandle", "null", "%s", B(::InternetCloseHandle(NULL)).c_str());
	char b[8];
	::SetLastError(0);
	BOOL rf = ::InternetReadFile(NULL, b, 8, &n);
	out("InternetReadFile", "nullhandle", "%s", B(rf).c_str());
	::InternetCloseHandle(ses);
	(void)sz;
}

TEST(inet_QueryInfoBuffer)
{
	// eMule passes _countof() (characters) where HttpQueryInfo wants bytes;
	// what happens with a too-sbuf buffer, and what size comes back.
	Server *srv = StartServer();
	HINTERNET ses = ::InternetOpenW(L"symrepro", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	HINTERNET con = ::InternetConnectW(ses, L"127.0.0.1", srv->port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
	HINTERNET req = ::HttpOpenRequestW(con, NULL, L"/ok", NULL, NULL, NULL, INTERNET_FLAG_RELOAD | INTERNET_FLAG_KEEP_CONNECTION, 0);
	::HttpSendRequestW(req, NULL, 0, NULL, 0);
	for (DWORD sz : {64u, 8u, 7u, 6u, 4u, 2u, 0u}) {
		wchar_t buf[32];
		wmemset(buf, L'#', 32);
		DWORD s = sz;
		::SetLastError(0);
		BOOL r = ::HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE, buf, &s, NULL);
		DWORD e = ::GetLastError();
		out("HttpQueryInfoW", Fmt("status.size%lu", sz).c_str(), "r=%d err=%lu outsize=%lu buf=%s", r, r ? 0 : e, s, Q(buf, 4).c_str());
	}
	wchar_t raw[1024];
	DWORD s = sizeof raw;
	BOOL r = ::HttpQueryInfoW(req, HTTP_QUERY_RAW_HEADERS_CRLF, raw, &s, NULL);
	out("HttpQueryInfoW", "rawheaders", "r=%d %s", r, r ? Q(raw).c_str() : "");
	DWORD idx = 0;
	s = sizeof raw;
	r = ::HttpQueryInfoW(req, HTTP_QUERY_CUSTOM, wcscpy(raw, L"Content-Type"), &s, &idx);
	out("HttpQueryInfoW", "custom", "r=%d %s idx=%lu", r, r ? Q(raw).c_str() : "", idx);
	s = sizeof raw;
	r = ::HttpQueryInfoW(req, HTTP_QUERY_VERSION, raw, &s, NULL);
	out("HttpQueryInfoW", "version", "r=%d %s", r, r ? Q(raw).c_str() : "");
	s = sizeof raw;
	r = ::HttpQueryInfoW(req, HTTP_QUERY_STATUS_TEXT, raw, &s, NULL);
	out("HttpQueryInfoW", "statustext", "r=%d %s", r, r ? Q(raw).c_str() : "");
	::InternetCloseHandle(req);
	::InternetCloseHandle(con);
	::InternetCloseHandle(ses);
	StopServer(srv);
}

TEST(inet_CrackUrl)
{
	// URLClient.cpp SetUrl, ED2KLink.cpp sources, AddSourceDlg.cpp.
	static const wchar_t *const urls[] = {
		L"http://host:4711/path/file.met?x=1#frag", L"http://user:pass@host/x", L"https://secure.example/x", L"http://host",
		L"host/path", L"HTTP://HOST/A", L"http://host:99999/", L"http://host:0/", L"http://h%20st/x", L"http://host/a b",
		L"ftp://x/y", L"http://[::1]:80/", L"http://1.2.3.4:4662/file.part", L"http://host/\u00e9t\u00e9.mp3",
		L"http://host/path/../x", L"http://host/?q", L"http://host#f", L"ed2k://|file|x.mp3|123|0123456789ABCDEF0123456789ABCDEF|/",
		L"mailto:a@b", L"file:///C:/x/y.txt", L"http:///nohost", L"http://:80/x", L"", L"http://host:/x", L"http://ho:st/x",
		L"http://host\\path\\x", L"  http://host/", L"http://host/x?a=1&b=%41",
	};
	for (const wchar_t *u : urls) {
		wchar_t scheme[64], host[256], user[64], pass[64], path[1024], extra[1024];
		URL_COMPONENTSW c = {};
		c.dwStructSize = sizeof c;
		c.lpszScheme = scheme;
		c.dwSchemeLength = _countof(scheme);
		c.nScheme = INTERNET_SCHEME_DEFAULT;
		c.lpszHostName = host;
		c.dwHostNameLength = _countof(host);
		c.lpszUserName = user;
		c.dwUserNameLength = _countof(user);
		c.lpszPassword = pass;
		c.dwPasswordLength = _countof(pass);
		c.lpszUrlPath = path;
		c.dwUrlPathLength = _countof(path);
		c.lpszExtraInfo = extra;
		c.dwExtraInfoLength = _countof(extra);
		::SetLastError(0);
		BOOL r = ::InternetCrackUrlW(u, 0, 0, &c);
		DWORD e = ::GetLastError();
		if (r)
			out("InternetCrackUrlW", Q(u).c_str(), "ok scheme=%s(%d) host=%s port=%u user=%s pass=%s path=%s extra=%s",
				Q(scheme, c.dwSchemeLength).c_str(), c.nScheme, Q(host, c.dwHostNameLength).c_str(), c.nPort, Q(user, c.dwUserNameLength).c_str(),
				Q(pass, c.dwPasswordLength).c_str(), Q(path, c.dwUrlPathLength).c_str(), Q(extra, c.dwExtraInfoLength).c_str());
		else
			out("InternetCrackUrlW", Q(u).c_str(), "fail err=%lu", e);
	}
	// Explicit length (AddSourceDlg passes GetLength()), and pointer-only mode.
	const wchar_t *u = L"http://host:81/a/b.txt?z";
	URL_COMPONENTSW c = {};
	c.dwStructSize = sizeof c;
	c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = 1;
	BOOL r = ::InternetCrackUrlW(u, (DWORD)wcslen(u), 0, &c);
	out("InternetCrackUrlW", "pointers", "r=%d host=+%d/%lu path=+%d/%lu extra=+%d/%lu port=%u", r, c.lpszHostName ? (int)(c.lpszHostName - u) : -1,
		c.dwHostNameLength, c.lpszUrlPath ? (int)(c.lpszUrlPath - u) : -1, c.dwUrlPathLength,
		c.lpszExtraInfo ? (int)(c.lpszExtraInfo - u) : -1, c.dwExtraInfoLength, c.nPort);
	r = ::InternetCrackUrlW(u, 10, 0, &c);
	out("InternetCrackUrlW", "truncatedlength", "r=%d err=%lu", r, r ? 0 : ::GetLastError());
	wchar_t tiny[3];
	URL_COMPONENTSW t = {};
	t.dwStructSize = sizeof t;
	t.lpszHostName = tiny;
	t.dwHostNameLength = _countof(tiny);
	::SetLastError(0);
	r = ::InternetCrackUrlW(L"http://longhostname/x", 0, 0, &t);
	out("InternetCrackUrlW", "smallhostbuffer", "r=%d err=%lu len=%lu", r, ::GetLastError(), t.dwHostNameLength);
	URL_COMPONENTSW d = {};
	d.dwStructSize = sizeof d;
	d.dwHostNameLength = 1;
	r = ::InternetCrackUrlW(L"http://h%41st/x%20y", 0, ICU_DECODE, &d);
	out("InternetCrackUrlW", "decode.pointers", "r=%d err=%lu", r, r ? 0 : ::GetLastError());
}

TEST(inet_CanonicalizeUrl)
{
	static const wchar_t *const urls[] = {
		L"http://host/a b/c", L"http://host/a%20b", L"http://host/a/../b/./c", L"http://host/\u00e9", L"http://host/a?b c#d e",
		L"http://host\\a\\b", L"HTTP://Host/Path", L"http://host/%7e%2F%25", L"http://host/a<b>\"c", L"http://host:80/x",
		L"http://host/x/..", L"http://host/../../x", L"ed2k://|file|a b.mp3|1|0123456789ABCDEF0123456789ABCDEF|/", L"host/a b", L"",
		L"http://host/a%zzb", L"http://host/%", L"http://user@host/a b", L"http://host/a\tb",
	};
	struct { const char *n; DWORD f; } flags[] = {
		{"noencode", ICU_NO_ENCODE}, {"decode_browser", ICU_DECODE | ICU_NO_ENCODE | ICU_BROWSER_MODE}, {"encodepercent", ICU_ENCODE_PERCENT},
		{"default", 0}};
	for (auto &f : flags)
		for (const wchar_t *u : urls) {
			wchar_t buf[INTERNET_MAX_URL_LENGTH];
			DWORD sz = _countof(buf);
			::SetLastError(0);
			BOOL r = ::InternetCanonicalizeUrlW(u, buf, &sz, f.f);
			DWORD e = ::GetLastError();
			out("InternetCanonicalizeUrlW", (std::string(f.n) + Q(u)).c_str(), "%s", r ? Fmt("ok len=%lu %s", sz, Q(buf).c_str()).c_str() : Fmt("fail err=%lu", e).c_str());
		}
	wchar_t sbuf[5];
	DWORD sz = _countof(sbuf);
	::SetLastError(0);
	BOOL r = ::InternetCanonicalizeUrlW(L"http://host/abcdef", sbuf, &sz, 0);
	out("InternetCanonicalizeUrlW", "small", "r=%d err=%lu need=%lu", r, ::GetLastError(), sz);
}
