// symrepro harness: registry, output, per-test child processes.
//
//   symrepro --all [prefix]   run every test (or those whose name starts with
//                             prefix), each in its own child process with a
//                             timeout, and print all results on stdout
//   symrepro --list           list the tests
//   symrepro --run NAME       run one test in this process
//   symrepro --helper NAME .. internal: child-side half of some tests
//
// One child per test so that a test that hangs or crashes costs only its own
// lines - the hang or the crash becomes the result, and the run goes on.
#include "harness.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <algorithm>

static std::vector<TestReg*> &Registry()
{
	static std::vector<TestReg*> s;
	return s;
}

TestReg::TestReg(const char *n, TestFn f, DWORD t) : name(n), fn(f), timeoutMs(t)
{
	Registry().push_back(this);
}

struct HelperReg { const char *name; HelperFn fn; };
static std::vector<HelperReg> &Helpers()
{
	static std::vector<HelperReg> s;
	return s;
}
int RegisterHelper(const char *name, HelperFn fn)
{
	Helpers().push_back(HelperReg{name, fn});
	return 0;
}

static std::wstring s_tdir;
const std::wstring &TDir() { return s_tdir; }

static void WriteOut(const std::string &s)
{
	DWORD n;
	::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &n, NULL);
}

std::string Fmt(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
	va_end(ap);
	return buf;
}

void out(const char *sym, const char *cas, const char *fmt, ...)
{
	char buf[16384];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
	va_end(ap);
	std::string line = std::string(sym) + "|" + cas + "|" + buf;
	for (char &c : line)
		if (c == '\n' || c == '\r')
			c = ' ';
	WriteOut(line + "\n");
}

static void EscapeCp(std::string &r, unsigned cp)
{
	if (cp == '"' || cp == '\\') {
		r += '\\';
		r += (char)cp;
	} else if (cp == '|') {
		r += "\\x7c";
	} else if (cp < 0x20 || cp == 0x7f) {
		char b[8];
		sprintf_s(b, "\\x%02x", cp);
		r += b;
	} else if (cp < 0x80) {
		r += (char)cp;
	} else if (cp < 0x800) {
		r += (char)(0xC0 | (cp >> 6));
		r += (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		r += (char)(0xE0 | (cp >> 12));
		r += (char)(0x80 | ((cp >> 6) & 0x3F));
		r += (char)(0x80 | (cp & 0x3F));
	} else {
		r += (char)(0xF0 | (cp >> 18));
		r += (char)(0x80 | ((cp >> 12) & 0x3F));
		r += (char)(0x80 | ((cp >> 6) & 0x3F));
		r += (char)(0x80 | (cp & 0x3F));
	}
}

std::string Q(const wchar_t *s, size_t n)
{
	if (s == NULL)
		return "NULL";
	std::string r = "\"";
	for (size_t i = 0; i < n; ++i) {
		unsigned cp = (unsigned short)s[i];
		if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && (unsigned short)s[i + 1] >= 0xDC00 && (unsigned short)s[i + 1] < 0xE000) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned short)s[i + 1] - 0xDC00);
			++i;
		} else if (cp >= 0xD800 && cp < 0xE000) {
			char b[12];
			sprintf_s(b, "\\u%04x", cp);	// lone surrogate, keep it visible
			r += b;
			continue;
		}
		EscapeCp(r, cp);
	}
	return r + "\"";
}

std::string Q(const wchar_t *s) { return s ? Q(s, wcslen(s)) : "NULL"; }

std::string QA(const char *s, size_t n)
{
	if (s == NULL)
		return "NULL";
	std::string r = "\"";
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)s[i];
		if (c >= 0x80) {
			char b[8];
			sprintf_s(b, "\\x%02x", c);
			r += b;
		} else
			EscapeCp(r, c);
	}
	return r + "\"";
}

std::string QA(const char *s) { return s ? QA(s, strlen(s)) : "NULL"; }

std::string Hex(const void *p, size_t n)
{
	static const char d[] = "0123456789abcdef";
	std::string r;
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = ((const unsigned char*)p)[i];
		r += d[c >> 4];
		r += d[c & 15];
	}
	return r;
}

std::string B(BOOL r)
{
	DWORD e = ::GetLastError();
	return r ? std::string("ok") : Fmt("fail err=%lu", e);
}

std::string BE(bool ok)
{
	DWORD e = ::GetLastError();
	return ok ? std::string("ok") : Fmt("fail err=%lu", e);
}

std::wstring Norm(const std::wstring &s)
{
	std::wstring r = s;
	if (s_tdir.empty())
		return r;
	std::wstring base = s_tdir.substr(0, s_tdir.size() - 1);	// without trailing '\'
	for (;;) {
		// case-insensitive search
		std::wstring lr = r, lb = base;
		std::transform(lr.begin(), lr.end(), lr.begin(), ::towlower);
		std::transform(lb.begin(), lb.end(), lb.begin(), ::towlower);
		size_t p = lr.find(lb);
		if (p == std::wstring::npos)
			break;
		r.replace(p, base.size(), L"<T>");
	}
	return r;
}

std::string QN(const std::wstring &s) { return Q(Norm(s).c_str()); }

bool WriteWholeFile(const std::wstring &path, const void *data, DWORD n)
{
	HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD w = 0;
	BOOL ok = ::WriteFile(h, data, n, &w, NULL);
	::CloseHandle(h);
	return ok && w == n;
}

std::wstring Widen(const char *s)
{
	int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	std::wstring r(n ? n - 1 : 0, L'\0');
	if (n > 1)
		::MultiByteToWideChar(CP_UTF8, 0, s, -1, &r[0], n);
	return r;
}

std::string Narrow(const wchar_t *s)
{
	int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	std::string r(n ? n - 1 : 0, '\0');
	if (n > 1)
		::WideCharToMultiByte(CP_UTF8, 0, s, -1, &r[0], n, NULL, NULL);
	return r;
}

std::wstring SelfPath()
{
	wchar_t b[MAX_PATH * 2];
	DWORD n = ::GetModuleFileNameW(NULL, b, _countof(b));
	return std::wstring(b, n);
}

// ---------------------------------------------------------------------------
// DialogCloser

DialogCloser::DialogCloser(DWORD delayMs) : m_bStop(0), m_delay(delayMs)
{
	::InitializeCriticalSection(&m_cs);
	m_hThread = ::CreateThread(NULL, 0, Thread, this, 0, NULL);
}

DialogCloser::~DialogCloser()
{
	::InterlockedExchange(&m_bStop, 1);
	::WaitForSingleObject(m_hThread, 5000);
	::CloseHandle(m_hThread);
	::DeleteCriticalSection(&m_cs);
}

std::string DialogCloser::Seen() const
{
	::EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
	std::string r = m_seen;
	::LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
	return r.empty() ? "none" : r;
}

struct EnumCtx { DialogCloser *self; DWORD pid; std::vector<HWND> found; };

static BOOL CALLBACK EnumTop(HWND h, LPARAM lp)
{
	EnumCtx *c = (EnumCtx*)lp;
	DWORD pid = 0;
	::GetWindowThreadProcessId(h, &pid);
	if (pid == c->pid && ::IsWindowVisible(h))
		c->found.push_back(h);
	return TRUE;
}

DWORD WINAPI DialogCloser::Thread(LPVOID p)
{
	DialogCloser *self = (DialogCloser*)p;
	std::vector<HWND> handled;
	while (!self->m_bStop) {
		EnumCtx c{self, ::GetCurrentProcessId(), {}};
		::EnumWindows(EnumTop, (LPARAM)&c);
		for (HWND h : c.found) {
			if (std::find(handled.begin(), handled.end(), h) != handled.end())
				continue;
			::Sleep(self->m_delay);
			wchar_t cls[128] = L"";
			::GetClassNameW(h, cls, _countof(cls));
			::EnterCriticalSection(&self->m_cs);
			if (!self->m_seen.empty())
				self->m_seen += ",";
			self->m_seen += Narrow(cls);
			::LeaveCriticalSection(&self->m_cs);
			handled.push_back(h);
			::PostMessageW(h, WM_COMMAND, IDCANCEL, 0);
			::PostMessageW(h, WM_CLOSE, 0, 0);
		}
		::Sleep(50);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Scratch directory

static void RemoveTree(const std::wstring &dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
				continue;
			std::wstring p = dir + L"\\" + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
				::SetFileAttributesW(p.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				RemoveTree(p);
			else
				::DeleteFileW(p.c_str());
		} while (::FindNextFileW(h, &fd));
		::FindClose(h);
	}
	::RemoveDirectoryW(dir.c_str());
}

static void PrepareTDir(const char *name)
{
	::CreateDirectoryW(L"C:\\srt", NULL);
	std::wstring d = L"C:\\srt\\" + Widen(name);
	RemoveTree(d);
	::CreateDirectoryW(d.c_str(), NULL);
	s_tdir = d + L"\\";
}

// ---------------------------------------------------------------------------
// Running

static int RunOne(const char *name)
{
	for (TestReg *t : Registry())
		if (!strcmp(t->name, name)) {
			PrepareTDir(name);
			__try {
				t->fn();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				out(name, "#exception", "0x%08lx", GetExceptionCode());
			}
			return 0;
		}
	fprintf(stderr, "no test %s\n", name);
	return 2;
}

DWORD SpawnCapture(const std::wstring &cmd, DWORD timeoutMs, std::string *output, bool *timedOut)
{
	if (timedOut)
		*timedOut = false;
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE hRead, hWrite;
	if (!::CreatePipe(&hRead, &hWrite, &sa, 0))
		return 0xFFFFFFF0;
	::SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOW si = {sizeof si};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = hWrite;
	si.hStdError = hWrite;
	si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION pi;
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(0);
	if (!::CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		::CloseHandle(hRead);
		::CloseHandle(hWrite);
		return 0xFFFFFFF1;
	}
	::CloseHandle(hWrite);
	std::string result;
	// Reader thread so a chatty child can never block on a full pipe.
	struct R { HANDLE h; std::string *s; } r = {hRead, &result};
	HANDLE hReader = ::CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
		R *r = (R*)p;
		char buf[4096];
		DWORD n;
		while (::ReadFile(r->h, buf, sizeof buf, &n, NULL) && n)
			r->s->append(buf, n);
		return 0;
	}, &r, 0, NULL);
	DWORD w = ::WaitForSingleObject(pi.hProcess, timeoutMs);
	if (w == WAIT_TIMEOUT) {
		::TerminateProcess(pi.hProcess, 0xDEAD);
		::WaitForSingleObject(pi.hProcess, 5000);
		if (timedOut)
			*timedOut = true;
	}
	// Grandchildren may hold the pipe open; do not wait for them forever.
	if (::WaitForSingleObject(hReader, 3000) == WAIT_TIMEOUT) {
		::CancelSynchronousIo(hReader);
		if (::WaitForSingleObject(hReader, 2000) == WAIT_TIMEOUT)
			::TerminateThread(hReader, 0);
	}
	DWORD code = 0;
	::GetExitCodeProcess(pi.hProcess, &code);
	::CloseHandle(hReader);
	::CloseHandle(hRead);
	::CloseHandle(pi.hThread);
	::CloseHandle(pi.hProcess);
	if (output)
		*output = result;
	return code;
}

DWORD RunHelper(const std::wstring &args, std::string *output, DWORD timeoutMs)
{
	return SpawnCapture(L"\"" + SelfPath() + L"\" --helper " + args, timeoutMs, output);
}

static std::string RunChild(const TestReg *t)
{
	std::string result;
	bool to = false;
	DWORD code = SpawnCapture(L"\"" + SelfPath() + L"\" --run " + Widen(t->name), t->timeoutMs, &result, &to);
	if (!result.empty() && result.back() != '\n')
		result += "\n";
	if (to)
		result += Fmt("%s|#harness|TIMEOUT after %lu ms\n", t->name, t->timeoutMs);
	else
		result += Fmt("%s|#exit|0x%08lx\n", t->name, code);
	return result;
}

int wmain(int argc, wchar_t **argv)
{
	std::sort(Registry().begin(), Registry().end(), [](const TestReg *a, const TestReg *b) { return strcmp(a->name, b->name) < 0; });
	if (argc >= 3 && !wcscmp(argv[1], L"--run"))
		return RunOne(Narrow(argv[2]).c_str());
	if (argc >= 3 && !wcscmp(argv[1], L"--helper")) {
		std::string n = Narrow(argv[2]);
		for (const HelperReg &h : Helpers())
			if (n == h.name)
				return h.fn(argc - 3, argv + 3);
		return 3;
	}
	if (argc >= 2 && !wcscmp(argv[1], L"--list")) {
		for (TestReg *t : Registry())
			printf("%s\n", t->name);
		return 0;
	}
	if (argc >= 2 && !wcscmp(argv[1], L"--all")) {
		std::string prefix = argc >= 3 ? Narrow(argv[2]) : "";
		// Pin down what this run is, on the first lines of the output.
		SYSTEM_INFO si;
		::GetNativeSystemInfo(&si);
		out("#run", "~arch", "native=%u ptr=%u", si.wProcessorArchitecture, (unsigned)sizeof(void*));
		HMODULE hNt = ::GetModuleHandleW(L"ntdll.dll");
		const char *(CDECL *pWineVer)() = hNt ? (const char *(CDECL *)())::GetProcAddress(hNt, "wine_get_version") : NULL;
		out("#run", "~platform", "%s", pWineVer ? (std::string("wine ") + pWineVer()).c_str() : "windows");
		for (TestReg *t : Registry()) {
			if (strncmp(t->name, prefix.c_str(), prefix.size()))
				continue;
			WriteOut(std::string("@@ ") + t->name + "\n");
			WriteOut(RunChild(t));
		}
		return 0;
	}
	fprintf(stderr, "usage: symrepro --all [prefix] | --list | --run NAME\n");
	return 1;
}
