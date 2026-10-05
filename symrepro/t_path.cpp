// Path strings - SHLWAPI Path*, GetFullPathName, environment expansion,
// current directory, module file names.
#include "harness.h"
#include <shlwapi.h>

static const wchar_t *const kPaths[] = {
	L"", L"C:", L"C:\\", L"C:\\a", L"C:\\a\\", L"C:\\a\\b.txt", L"C:\\a.b\\c", L"C:\\a\\b.tar.gz",
	L"\\\\server\\share", L"\\\\server\\share\\", L"\\\\server\\share\\x.y", L"\\\\server", L"\\\\?\\C:\\a\\b",
	L"a\\b", L"b.txt", L".txt", L"a.", L"a..b", L"C:\\a\\..\\b", L"C:\\a\\.\\b\\..\\..\\..\\c", L"..\\x", L"\\x",
	L"C:x", L"C:\\a\\b. ", L"C:\\dir.d\\file", L"file name with spaces.ext", L"\"C:\\Program Files\\x.exe\" -a -b",
	L"C:\\Program Files\\x.exe -a", L"C:\\a/b/c.txt", L"//server/share", L"C:\\\u00e9\\\u00fc.txt", L"X:\\",
	L"C:\\a\\b\\", L"C:\\a\\b.", L"C:\\.hidden", L"name.part.met", L"name.part.met.bak", L"x.ed2k", L"C:\\a\\\\b",
	L"\\", L"\\\\", L"C:\\a\\b c\\d e.f g", L"1:\\x", L"c:\\lower",
};

typedef void (*InPlace)(wchar_t *);

static void EachInPlace(const char *sym, InPlace fn)
{
	for (const wchar_t *p : kPaths) {
		wchar_t buf[MAX_PATH] = {0};
		wcscpy_s(buf, p);
		fn(buf);
		out(sym, Q(p).c_str(), "%s", Q(buf).c_str());
	}
}

TEST(path_PathAddBackslashW)
{
	for (const wchar_t *p : kPaths) {
		wchar_t buf[MAX_PATH] = {0};
		wcscpy_s(buf, p);
		LPWSTR r = ::PathAddBackslashW(buf);
		out("PathAddBackslashW", Q(p).c_str(), "%s ret=%s", Q(buf).c_str(), r ? Fmt("+%d", (int)(r - buf)).c_str() : "NULL");
	}
	wchar_t full[MAX_PATH];
	for (int i = 0; i < MAX_PATH - 1; ++i)
		full[i] = L'x';
	full[MAX_PATH - 1] = 0;
	LPWSTR r = ::PathAddBackslashW(full);
	out("PathAddBackslashW", "full", "ret=%s len=%d", r ? "nonnull" : "NULL", (int)wcslen(full));
}

TEST(path_PathBuildRootW)
{
	for (int d : {-1, 0, 1, 2, 25, 26, 100}) {
		wchar_t buf[8] = L"zzzzzzz";
		LPWSTR r = ::PathBuildRootW(buf, d);
		out("PathBuildRootW", Fmt("%d", d).c_str(), "%s ret=%s", Q(buf).c_str(), r == buf ? "buf" : r ? "other" : "NULL");
	}
}

TEST(path_PathCanonicalizeW)
{
	for (const wchar_t *p : kPaths) {
		wchar_t buf[MAX_PATH] = L"#";
		BOOL r = ::PathCanonicalizeW(buf, p);
		out("PathCanonicalizeW", Q(p).c_str(), "r=%d %s", r, Q(buf).c_str());
	}
}

TEST(path_PathCombineW)
{
	static const wchar_t *const dirs[] = {L"C:\\a", L"C:\\a\\", L"C:\\", L"", L"\\\\s\\sh", L"rel", NULL, L"C:"};
	static const wchar_t *const files[] = {L"b.txt", L"\\b.txt", L"..\\b", L"D:\\x", L"", L".", L"MEDIAINFO.DLL",
		L"MediaInfo\\MEDIAINFO.DLL", NULL, L"b\\..\\..\\..\\c"};
	for (const wchar_t *d : dirs)
		for (const wchar_t *f : files) {
			wchar_t buf[MAX_PATH] = L"#";
			LPWSTR r = ::PathCombineW(buf, d, f);
			out("PathCombineW", (Q(d) + "+" + Q(f)).c_str(), "%s %s", r ? "ok" : "NULL", r ? Q(buf).c_str() : "");
		}
}

TEST(path_PathFileExistsW)
{
	const std::wstring f = TDir() + L"exists.txt", d = TDir() + L"dir";
	WriteWholeFile(f, "x", 1);
	::CreateDirectoryW(d.c_str(), NULL);
	struct { const char *n; std::wstring p; } c[] = {
		{"file", f}, {"dir", d}, {"dir.slash", d + L"\\"}, {"missing", TDir() + L"nope"}, {"missing.dir", TDir() + L"nope\\x"},
		{"empty", L""}, {"root", L"C:\\"}, {"drive", L"C:"}, {"file.trailingdot", f + L"."}, {"file.trailingspace", f + L" "},
		{"file.upper", TDir() + L"EXISTS.TXT"}, {"wildcard", TDir() + L"*.txt"}, {"nul", L"NUL"}, {"con", L"C:\\con"},
		{"badchar", TDir() + L"a<b"}, {"fileasdir", f + L"\\x"},
	};
	for (auto &x : c) {
		::SetLastError(0);
		BOOL r = ::PathFileExistsW(x.p.c_str());
		out("PathFileExistsW", x.n, "r=%d err=%lu", r, r ? 0 : ::GetLastError());
	}
}

TEST(path_PathFindExtensionW)
{
	for (const wchar_t *p : kPaths) {
		LPWSTR r = ::PathFindExtensionW(p);
		out("PathFindExtensionW", Q(p).c_str(), "+%d %s", (int)(r - p), Q(r).c_str());
	}
}

TEST(path_PathFindFileNameW)
{
	for (const wchar_t *p : kPaths) {
		LPWSTR r = ::PathFindFileNameW(p);
		out("PathFindFileNameW", Q(p).c_str(), "+%d", (int)(r - p));
	}
}

TEST(path_PathGetArgsW)
{
	static const wchar_t *const c[] = {L"x.exe -a", L"\"C:\\Program Files\\x.exe\" -a -b", L"C:\\Program Files\\x.exe -a",
		L"x.exe", L"", L"\"unterminated -a", L"x.exe  two", L"\"a b\"", L"a\tb"};
	for (const wchar_t *p : c) {
		LPWSTR r = ::PathGetArgsW(p);
		out("PathGetArgsW", Q(p).c_str(), "+%d", (int)(r - p));
	}
}

TEST(path_PathGetDriveNumberW)
{
	for (const wchar_t *p : kPaths)
		out("PathGetDriveNumberW", Q(p).c_str(), "%d", ::PathGetDriveNumberW(p));
}

TEST(path_PathIsRelativeW)
{
	for (const wchar_t *p : kPaths)
		out("PathIsRelativeW", Q(p).c_str(), "%d", ::PathIsRelativeW(p));
}

TEST(path_PathIsRootW)
{
	for (const wchar_t *p : kPaths)
		out("PathIsRootW", Q(p).c_str(), "%d", ::PathIsRootW(p));
}

TEST(path_PathMatchSpecW)
{
	// DownloadQueue.cpp: file name against a category's extension filter.
	static const wchar_t *const names[] = {L"song.mp3", L"SONG.MP3", L"a.b.mp3", L"movie.avi", L"noext", L"x.mp3.part",
		L"C:\\dir\\song.mp3", L"abc", L"a c", L".mp3", L"file.", L"\u00e9t\u00e9.mp3"};
	static const wchar_t *const specs[] = {L"*.mp3", L"*.MP3", L"*.mp3;*.avi", L"*.mp3; *.avi", L"a?c", L"*.*", L"*", L"*.",
		L"?", L"*mp3", L"*.mp?", L"", L"song.mp3", L"*.mp3 ", L"[a]*"};
	for (const wchar_t *n : names) {
		std::string row;
		for (const wchar_t *s : specs)
			row += ::PathMatchSpecW(n, s) ? '1' : '0';
		out("PathMatchSpecW", Q(n).c_str(), "%s", row.c_str());
	}
}

TEST(path_PathRemoveExtensionW) { EachInPlace("PathRemoveExtensionW", [](wchar_t *b) { ::PathRemoveExtensionW(b); }); }
TEST(path_PathStripPathW) { EachInPlace("PathStripPathW", [](wchar_t *b) { ::PathStripPathW(b); }); }

TEST(path_PathRemoveFileSpecW)
{
	for (const wchar_t *p : kPaths) {
		wchar_t buf[MAX_PATH] = {0};
		wcscpy_s(buf, p);
		BOOL r = ::PathRemoveFileSpecW(buf);
		out("PathRemoveFileSpecW", Q(p).c_str(), "r=%d %s", r, Q(buf).c_str());
	}
}

TEST(path_PathStripToRootW)
{
	for (const wchar_t *p : kPaths) {
		wchar_t buf[MAX_PATH] = {0};
		wcscpy_s(buf, p);
		BOOL r = ::PathStripToRootW(buf);
		out("PathStripToRootW", Q(p).c_str(), "r=%d %s", r, Q(buf).c_str());
	}
}

TEST(path_PathRenameExtensionW)
{
	for (const wchar_t *ext : {L".tmp", L".bak", L"tmp", L""}) {
		for (const wchar_t *p : kPaths) {
			wchar_t buf[MAX_PATH] = {0};
			wcscpy_s(buf, p);
			BOOL r = ::PathRenameExtensionW(buf, ext);
			out("PathRenameExtensionW", (Q(p) + Q(ext)).c_str(), "r=%d %s", r, Q(buf).c_str());
		}
	}
	wchar_t full[MAX_PATH];
	for (int i = 0; i < MAX_PATH - 3; ++i)
		full[i] = L'x';
	full[MAX_PATH - 3] = 0;
	BOOL r = ::PathRenameExtensionW(full, L".tmp");
	out("PathRenameExtensionW", "overflow", "r=%d len=%d", r, (int)wcslen(full));
}

TEST(path_GetFullPathNameW)
{
	::SetCurrentDirectoryW(TDir().c_str());
	static const wchar_t *const c[] = {L"x.txt", L".\\x", L"..\\x", L"a\\..\\..\\..\\..\\..\\..\\x", L"C:", L"C:x", L"\\x",
		L"con", L"nul", L"COM1", L"a\\con.txt", L"LPT1.txt", L"file. ", L"file...", L"dir\\", L"dir\\.", L"/x/y", L"\\\\?\\C:\\x\\..",
		L"\\\\server\\share\\..\\..\\x", L"", L"C:\\a\\b\\c\\..\\.", L"  lead", L"trail  ", L"a*b", L"\\\\.\\C:", L"x:\\y"};
	for (const wchar_t *p : c) {
		wchar_t buf[MAX_PATH] = L"#";
		LPWSTR filePart = NULL;
		::SetLastError(0);
		DWORD n = ::GetFullPathNameW(p, MAX_PATH, buf, &filePart);
		DWORD e = ::GetLastError();
		out("GetFullPathNameW", Q(p).c_str(), "n=%lu err=%lu %s part=%s", n, n ? 0 : e, QN(buf).c_str(),
			filePart ? Fmt("+%d", (int)(filePart - buf)).c_str() : "NULL");
	}
	wchar_t small[4];
	DWORD n = ::GetFullPathNameW(L"abcdef", 4, small, NULL);
	out("GetFullPathNameW", "small", "n=%lu", n - (DWORD)TDir().size());	// required size relative to cwd length
	n = ::GetFullPathNameW(L"abcdef", 0, NULL, NULL);
	out("GetFullPathNameW", "query", "n=%lu", n - (DWORD)TDir().size());
}

TEST(path_ExpandEnvironmentStringsW)
{
	::SetEnvironmentVariableW(L"SR_A", L"alpha");
	::SetEnvironmentVariableW(L"SR_PATH", L"C:\\x y\\z");
	static const wchar_t *const c[] = {L"%SR_A%", L"%SR_A%\\x", L"%sr_a%", L"%NOPE_SR%", L"%%", L"%SR_A", L"a%SR_A%%SR_A%b",
		L"%SR_A%%", L"%%SR_A%%", L"no vars", L"", L"\"%SR_PATH%\\bin\\x.exe\" %1", L"%SR_A%%NOPE_SR%%SR_A%", L"%=C:%"};
	for (const wchar_t *p : c) {
		DWORD need = ::ExpandEnvironmentStringsW(p, NULL, 0);
		wchar_t buf[256] = L"#";
		DWORD n = ::ExpandEnvironmentStringsW(p, buf, 256);
		out("ExpandEnvironmentStringsW", Q(p).c_str(), "need=%lu n=%lu %s", need, n, Q(buf).c_str());
	}
	wchar_t small[4] = L"###";
	::SetLastError(0);
	DWORD n = ::ExpandEnvironmentStringsW(L"%SR_A%%SR_A%", small, 4);
	out("ExpandEnvironmentStringsW", "small", "n=%lu err=%lu buf=%s", n, ::GetLastError(), Q(small, 3).c_str());
	// The real ones eMule may see in a preview command line: present or not.
	for (const wchar_t *v : {L"%ProgramFiles%", L"%SystemRoot%", L"%windir%", L"%APPDATA%", L"%LOCALAPPDATA%", L"%TEMP%",
			L"%USERPROFILE%", L"%ProgramFiles(x86)%", L"%ProgramW6432%", L"%PUBLIC%", L"%ComSpec%", L"%SystemDrive%"}) {
		wchar_t buf[MAX_PATH];
		DWORD r = ::ExpandEnvironmentStringsW(v, buf, MAX_PATH);
		out("ExpandEnvironmentStringsW", Narrow(v).c_str(), "expanded=%d", r && wcscmp(buf, v) != 0);
		out("ExpandEnvironmentStringsW", ("~" + Narrow(v)).c_str(), "%s", Q(buf).c_str());
	}
}

TEST(path_CurrentDirectory)
{
	BOOL r = ::SetCurrentDirectoryW(TDir().c_str());
	out("SetCurrentDirectoryW", "tdir", "%s", B(r).c_str());
	wchar_t buf[MAX_PATH];
	DWORD n = ::GetCurrentDirectoryW(MAX_PATH, buf);
	out("GetCurrentDirectoryW", "tdir", "n-len=%ld %s", (long)n - (long)wcslen(buf), QN(buf).c_str());
	DWORD need = ::GetCurrentDirectoryW(0, NULL);
	out("GetCurrentDirectoryW", "query", "need-len=%ld", (long)need - (long)wcslen(buf));
	wchar_t small[3] = L"##";
	n = ::GetCurrentDirectoryW(3, small);
	out("GetCurrentDirectoryW", "small", "n-len=%ld buf=%s", (long)n - (long)wcslen(buf), Q(small, 2).c_str());
	::CreateDirectoryW((TDir() + L"sub").c_str(), NULL);
	struct { const char *n; std::wstring p; } c[] = {
		{"sub", L"sub"}, {"dotdot", L".."}, {"missing", TDir() + L"nope"}, {"file", L"C:\\srt\\x.none"}, {"slash", TDir() + L"sub\\"},
		{"fwd", TDir() + L"sub/"}, {"empty", L""}, {"drive", L"C:"}, {"root", L"C:\\"}};
	for (auto &x : c) {
		::SetCurrentDirectoryW(TDir().c_str());
		r = ::SetCurrentDirectoryW(x.p.c_str());
		std::string rs = B(r);
		::GetCurrentDirectoryW(MAX_PATH, buf);
		out("SetCurrentDirectoryW", x.n, "%s now=%s", rs.c_str(), QN(buf).c_str());
	}
}

TEST(path_GetModuleFileNameW)
{
	wchar_t buf[MAX_PATH];
	DWORD n = ::GetModuleFileNameW(NULL, buf, MAX_PATH);
	std::wstring s(buf, n);
	out("GetModuleFileNameW", "self", "n=len:%d abs=%d name=%s", n == wcslen(buf), !::PathIsRelativeW(buf), Q(::PathFindFileNameW(buf)).c_str());
	n = ::GetModuleFileNameW(::GetModuleHandleW(NULL), buf, MAX_PATH);
	out("GetModuleFileNameW", "selfhandle", "same=%d", s == std::wstring(buf, n));
	wchar_t small[5] = L"####";
	::SetLastError(0);
	n = ::GetModuleFileNameW(NULL, small, 5);
	DWORD e = ::GetLastError();
	out("GetModuleFileNameW", "small", "n=%lu err=%lu last=%d", n, e, (int)small[4]);
	n = ::GetModuleFileNameW(::GetModuleHandleW(L"kernel32.dll"), buf, MAX_PATH);
	out("GetModuleFileNameW", "~kernel32", "%s", Q(buf).c_str());
	out("GetModuleFileNameW", "kernel32.name", "%s", Q(_wcslwr(::PathFindFileNameW(buf))).c_str());
	::SetLastError(0);
	n = ::GetModuleFileNameW((HMODULE)(ULONG_PTR)0x1234, buf, MAX_PATH);
	out("GetModuleFileNameW", "badhandle", "n=%lu err=%lu", n, ::GetLastError());
}

TEST(path_GetModuleHandleW)
{
	HMODULE self = ::GetModuleHandleW(NULL);
	out("GetModuleHandleW", "null", "%s", self == (HMODULE)GetModuleHandleW(SelfPath().c_str()) ? "eq-self-path" : "differs");
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)self;
	out("GetModuleHandleW", "null.mz", "%d", dos->e_magic == IMAGE_DOS_SIGNATURE);
	for (const wchar_t *m : {L"kernel32.dll", L"KERNEL32", L"kernel32", L"ntdll.dll", L"ws2_32.dll", L"nonexistent_sr.dll",
			L"symrepro.exe", L"symrepro", L"C:\\windows\\system32\\kernel32.dll", L"kernel32.dll "}) {
		::SetLastError(0);
		HMODULE h = ::GetModuleHandleW(m);
		out("GetModuleHandleW", Q(m).c_str(), "%s err=%lu", h ? "nonnull" : "NULL", h ? 0 : ::GetLastError());
	}
}
