// symrepro - one behavioural probe per Win32 symbol eMule calls.
//
// Every test prints lines of the form
//
//     symbol|case|value
//
// and the same binary is run on Windows and under Wine. The two outputs are
// compared line by line: the oracle is free, nothing here needs to know what
// the right answer is, only that both implementations give the same one.
//
// A case name starting with '~' marks a value that legitimately depends on
// the machine (free disk space, host name, time zone...). Those lines are
// reported side by side but not counted as a difference.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>

typedef void (*TestFn)();

struct TestReg
{
	const char *name;
	TestFn fn;
	DWORD timeoutMs;
	TestReg(const char *n, TestFn f, DWORD t = 30000);
};

#define TEST_T(id, ms) static void id(); static TestReg s_reg_##id(#id, id, ms); static void id()
#define TEST(id) TEST_T(id, 30000)

// One output line. sym and cas are plain ASCII.
void out(const char *sym, const char *cas, const char *fmt, ...);

// Quoted, escaped UTF-8 rendering of a wide / narrow string.
std::string Q(const wchar_t *s);
std::string Q(const wchar_t *s, size_t n);
std::string QA(const char *s);
std::string QA(const char *s, size_t n);
std::string Hex(const void *p, size_t n);

// "ok" or "fail err=N" for a BOOL-returning call; captures GetLastError
// immediately, so call it directly on the API result.
std::string B(BOOL r);
// Same, for calls that report failure through a sentinel.
std::string BE(bool ok);
std::string Fmt(const char *fmt, ...);

// The per-test scratch directory, e.g. C:\srt\file.copy\ (with trailing
// backslash). Created empty before the test runs.
const std::wstring &TDir();
// Replace the scratch directory prefix by <T> so the value is comparable.
std::wstring Norm(const std::wstring &s);
std::string QN(const std::wstring &s);	// Q(Norm(s))

typedef int (*HelperFn)(int argc, wchar_t **argv);
int RegisterHelper(const char *name, HelperFn fn);
// A helper is the child-side half of a test, run as "symrepro --helper id ...".
#define HELPER(id) static int id(int, wchar_t**); static int s_h_##id = RegisterHelper(#id, id); static int id(int argc, wchar_t **argv)

// Run a command line, capture stdout+stderr, wait up to timeoutMs.
// Returns the exit code, or 0xDEAD and *timedOut=true on timeout.
DWORD SpawnCapture(const std::wstring &cmdline, DWORD timeoutMs, std::string *output, bool *timedOut = NULL);
// SpawnCapture of this exe with "--helper <args>".
DWORD RunHelper(const std::wstring &args, std::string *output, DWORD timeoutMs = 20000);

// Small helpers used across the tests.
bool WriteWholeFile(const std::wstring &path, const void *data, DWORD n);
std::wstring Widen(const char *s);
std::string Narrow(const wchar_t *s);
std::wstring SelfPath();

// Background helper that closes any top-level window of this process that
// appears while it is armed, recording the class names it saw. Used for the
// calls that put up UI (folder picker, WinINet error dialog).
class DialogCloser
{
public:
	explicit DialogCloser(DWORD delayMs = 300);
	~DialogCloser();
	std::string Seen() const;
private:
	static DWORD WINAPI Thread(LPVOID);
	HANDLE m_hThread;
	volatile LONG m_bStop;
	DWORD m_delay;
	std::string m_seen;
	CRITICAL_SECTION m_cs;
};

// Message pump with timeout; returns when pred() is true or time is up.
template <class P> bool PumpUntil(P pred, DWORD ms)
{
	const DWORD t0 = ::GetTickCount();
	for (;;) {
		MSG msg;
		while (::PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&msg);
			::DispatchMessageW(&msg);
		}
		if (pred())
			return true;
		if (::GetTickCount() - t0 >= ms)
			return false;
		::MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
	}
}
