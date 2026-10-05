// Files and directories - KERNEL32 file API, plus SHFileOperation and
// StgOpenStorage, which eMule uses on files.
#include "harness.h"
#include <shellapi.h>
#include <objbase.h>
#include <winioctl.h>
#include <stdio.h>
#include <share.h>
#include <algorithm>

static std::string ReadAll(const std::wstring &p)
{
	HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return Fmt("<open err=%lu>", ::GetLastError());
	char buf[4096];
	DWORD n = 0;
	::ReadFile(h, buf, sizeof buf, &n, NULL);
	::CloseHandle(h);
	return QA(buf, n);
}

static std::string Exists(const std::wstring &p)
{
	DWORD a = ::GetFileAttributesW(p.c_str());
	return a == INVALID_FILE_ATTRIBUTES ? Fmt("no(err=%lu)", ::GetLastError()) : Fmt("yes(attr=%lx)", a);
}

static std::string H(HANDLE h)
{
	DWORD e = ::GetLastError();
	return h == INVALID_HANDLE_VALUE ? Fmt("fail err=%lu", e) : Fmt("ok err=%lu", e);
}

TEST(file_CreateFile_sharing)
{
	// Every pair (first open, second open) of access x share mode. A row per
	// first open, one character per second open: 1 = opened, 0 = refused
	// with a sharing violation, anything else = the error code.
	const std::wstring f = TDir() + L"share.bin";
	WriteWholeFile(f, "data", 4);
	static const DWORD acc[] = {GENERIC_READ, GENERIC_WRITE, GENERIC_READ | GENERIC_WRITE, FILE_APPEND_DATA, DELETE, 0};
	static const DWORD shr[] = {0, FILE_SHARE_READ, FILE_SHARE_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_SHARE_DELETE};
	for (DWORD a1 : acc)
		for (DWORD s1 : shr) {
			HANDLE h1 = ::CreateFileW(f.c_str(), a1, s1, NULL, OPEN_EXISTING, 0, NULL);
			if (h1 == INVALID_HANDLE_VALUE) {
				out("CreateFileW", Fmt("share.%lx.%lx", a1, s1).c_str(), "first fail err=%lu", ::GetLastError());
				continue;
			}
			std::string row;
			for (DWORD a2 : acc)
				for (DWORD s2 : shr) {
					HANDLE h2 = ::CreateFileW(f.c_str(), a2, s2, NULL, OPEN_EXISTING, 0, NULL);
					if (h2 != INVALID_HANDLE_VALUE) {
						row += '1';
						::CloseHandle(h2);
					} else {
						DWORD e = ::GetLastError();
						row += e == ERROR_SHARING_VIOLATION ? std::string("0") : Fmt("(%lu)", e);
					}
				}
			::CloseHandle(h1);
			out("CreateFileW", Fmt("share.%lx.%lx", a1, s1).c_str(), "%s", row.c_str());
		}
	// The CRT share modes eMule uses through _tfsopen / _wsopen_s.
	FILE *f1 = _wfsopen(f.c_str(), L"rb", _SH_DENYWR);
	FILE *f2 = _wfsopen(f.c_str(), L"rb", _SH_DENYWR);
	FILE *f3 = _wfsopen(f.c_str(), L"r+b", _SH_DENYNO);
	out("CreateFileW", "crt.denywr", "second_reader=%d writer=%d", f2 != NULL, f3 != NULL);
	if (f1) fclose(f1);
	if (f2) fclose(f2);
	if (f3) fclose(f3);
}

TEST(file_CreateFile_dispositions)
{
	const std::wstring f = TDir() + L"disp.txt";
	struct { const char *n; DWORD acc, disp; bool pre; } c[] = {
		{"create_new.missing", GENERIC_WRITE, CREATE_NEW, false}, {"create_new.exists", GENERIC_WRITE, CREATE_NEW, true},
		{"create_always.missing", GENERIC_WRITE, CREATE_ALWAYS, false}, {"create_always.exists", GENERIC_WRITE, CREATE_ALWAYS, true},
		{"open_existing.missing", GENERIC_READ, OPEN_EXISTING, false}, {"open_existing.exists", GENERIC_READ, OPEN_EXISTING, true},
		{"open_always.missing", GENERIC_WRITE, OPEN_ALWAYS, false}, {"open_always.exists", GENERIC_WRITE, OPEN_ALWAYS, true},
		{"truncate.exists", GENERIC_WRITE, TRUNCATE_EXISTING, true}, {"truncate.readonly_access", GENERIC_READ, TRUNCATE_EXISTING, true},
		{"truncate.missing", GENERIC_WRITE, TRUNCATE_EXISTING, false}, {"baddisp", GENERIC_READ, 0, true},
	};
	for (auto &x : c) {
		::DeleteFileW(f.c_str());
		if (x.pre)
			WriteWholeFile(f, "12345", 5);
		::SetLastError(0xBEEF);
		HANDLE h = ::CreateFileW(f.c_str(), x.acc, FILE_SHARE_READ, NULL, x.disp, FILE_ATTRIBUTE_NORMAL, NULL);
		std::string r = H(h);
		DWORD size = h != INVALID_HANDLE_VALUE ? ::GetFileSize(h, NULL) : 0;
		if (h != INVALID_HANDLE_VALUE)
			::CloseHandle(h);
		out("CreateFileW", x.n, "%s size=%lu", r.c_str(), size);
	}
	// Paths that are not plain files.
	::CreateDirectoryW((TDir() + L"adir").c_str(), NULL);
	WriteWholeFile(TDir() + L"ro.txt", "ro", 2);
	::SetFileAttributesW((TDir() + L"ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);
	struct { const char *n; std::wstring p; DWORD acc, disp, flags; } d[] = {
		{"dir.plain", TDir() + L"adir", GENERIC_READ, OPEN_EXISTING, 0},
		{"dir.backup", TDir() + L"adir", GENERIC_READ, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS},
		{"dir.create", TDir() + L"adir", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"readonly.write", TDir() + L"ro.txt", GENERIC_WRITE, OPEN_EXISTING, 0},
		{"readonly.read", TDir() + L"ro.txt", GENERIC_READ, OPEN_EXISTING, 0},
		{"readonly.createalways", TDir() + L"ro.txt", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"missingdir", TDir() + L"nodir\\x.txt", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"badchar", TDir() + L"a<b.txt", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"badchar.colon", TDir() + L"a:b.txt", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"badchar.question", TDir() + L"a?b.txt", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"trailingdot", TDir() + L"dot.", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"trailingspace", TDir() + L"space ", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"reserved.con", TDir() + L"con", GENERIC_WRITE, OPEN_EXISTING, 0},
		{"reserved.nul", L"NUL", GENERIC_WRITE, OPEN_EXISTING, 0},
		{"reserved.aux.txt", TDir() + L"aux.txt", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"long.255", TDir() + std::wstring(255, L'n'), GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"long.256", TDir() + std::wstring(256, L'n'), GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"long.total300", TDir() + std::wstring(120, L'a') + L"\\" + std::wstring(170, L'b'), GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"unicode", TDir() + L"\u6587\u4ef6 \u00e9\U0001F600.txt", GENERIC_WRITE, CREATE_ALWAYS, 0},
		{"case.upper", TDir() + L"RO.TXT", GENERIC_READ, OPEN_EXISTING, 0},
		{"slash.fwd", TDir() + L"adir/../ro.txt", GENERIC_READ, OPEN_EXISTING, 0},
		{"file.as.dir", TDir() + L"ro.txt\\x", GENERIC_READ, OPEN_EXISTING, 0},
	};
	for (auto &x : d) {
		::SetLastError(0xBEEF);
		HANDLE h = ::CreateFileW(x.p.c_str(), x.acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, x.disp, x.flags, NULL);
		std::string r = H(h);
		DWORD type = h != INVALID_HANDLE_VALUE ? ::GetFileType(h) : 99;
		if (h != INVALID_HANDLE_VALUE)
			::CloseHandle(h);
		out("CreateFileW", x.n, "%s type=%lu", r.c_str(), type);
	}
	WIN32_FIND_DATAW fd;
	for (const wchar_t *pat : {L"dot*", L"space*"}) {
		HANDLE hf = ::FindFirstFileW((TDir() + pat).c_str(), &fd);
		out("CreateFileW", Fmt("stored.%s", Narrow(pat).c_str()).c_str(), "%s", hf != INVALID_HANDLE_VALUE ? Q(fd.cFileName).c_str() : "none");
		if (hf != INVALID_HANDLE_VALUE)
			::FindClose(hf);
	}
	::SetFileAttributesW((TDir() + L"ro.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST(file_CreateFile_append)
{
	// Emule.cpp opens its diagnostic log with FILE_APPEND_DATA only: every
	// write must land at the end whatever the file pointer says.
	const std::wstring f = TDir() + L"append.txt";
	WriteWholeFile(f, "abc", 3);
	HANDLE h = ::CreateFileW(f.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	out("CreateFileW", "append.open", "%s", H(h).c_str());
	DWORD w = 0;
	::SetFilePointer(h, 0, NULL, FILE_BEGIN);
	BOOL r = ::WriteFile(h, "X", 1, &w, NULL);
	out("WriteFile", "append.afterseek0", "%s w=%lu", B(r).c_str(), w);
	r = ::WriteFile(h, "Y", 1, &w, NULL);
	char buf[8];
	DWORD n = 0;
	BOOL rr = ::ReadFile(h, buf, 1, &n, NULL);
	out("ReadFile", "append.handle", "%s n=%lu", B(rr).c_str(), n);
	::CloseHandle(h);
	out("WriteFile", "append.content", "%s", ReadAll(f).c_str());
}

TEST(file_DeletePending)
{
	// A file deleted while another handle (with FILE_SHARE_DELETE) is open:
	// eMule's upload/download threads hold such handles.
	const std::wstring f = TDir() + L"pending.txt";
	WriteWholeFile(f, "abc", 3);
	HANDLE h = ::CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	BOOL r = ::DeleteFileW(f.c_str());
	out("DeleteFileW", "open.sharedelete", "%s", B(r).c_str());
	out("DeleteFileW", "open.sharedelete.exists", "%s", Exists(f).c_str());
	HANDLE h2 = ::CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	out("DeleteFileW", "open.sharedelete.reopen", "%s", H(h2).c_str());
	if (h2 != INVALID_HANDLE_VALUE)
		::CloseHandle(h2);
	h2 = ::CreateFileW(f.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	out("DeleteFileW", "open.sharedelete.recreate", "%s", H(h2).c_str());
	if (h2 != INVALID_HANDLE_VALUE)
		::CloseHandle(h2);
	char buf[4];
	DWORD n = 0;
	OVERLAPPED ov = {0};
	ov.hEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);
	r = ::ReadFile(h, buf, 3, NULL, &ov);
	if (!r && ::GetLastError() == ERROR_IO_PENDING)
		r = ::GetOverlappedResult(h, &ov, &n, TRUE);
	else
		::GetOverlappedResult(h, &ov, &n, TRUE);
	out("DeleteFileW", "open.sharedelete.readold", "%s n=%lu", B(r).c_str(), n);
	::CloseHandle(ov.hEvent);
	::CloseHandle(h);
	out("DeleteFileW", "afterclose.exists", "%s", Exists(f).c_str());

	WriteWholeFile(f, "abc", 3);
	h = ::CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::DeleteFileW(f.c_str());
	out("DeleteFileW", "open.noshare", "%s", B(r).c_str());
	::CloseHandle(h);
	::DeleteFileW(f.c_str());
}

TEST(file_DeleteFileW)
{
	WriteWholeFile(TDir() + L"ro.txt", "x", 1);
	::SetFileAttributesW((TDir() + L"ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);
	::CreateDirectoryW((TDir() + L"d").c_str(), NULL);
	WriteWholeFile(TDir() + L"ok.txt", "x", 1);
	struct { const char *n; std::wstring p; } c[] = {
		{"ok", TDir() + L"ok.txt"}, {"again", TDir() + L"ok.txt"}, {"missing.dir", TDir() + L"nod\\x"}, {"readonly", TDir() + L"ro.txt"},
		{"directory", TDir() + L"d"}, {"wildcard", TDir() + L"*.txt"}, {"empty", L""}, {"trailing.slash", TDir() + L"ro.txt\\"}};
	for (auto &x : c) {
		BOOL r = ::DeleteFileW(x.p.c_str());
		out("DeleteFileW", x.n, "%s", B(r).c_str());
	}
	::SetFileAttributesW((TDir() + L"ro.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST(file_ReadWrite)
{
	const std::wstring f = TDir() + L"rw.bin";
	HANDLE h = ::CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD n = 0;
	BOOL r = ::WriteFile(h, "0123456789", 10, &n, NULL);
	out("WriteFile", "basic", "%s n=%lu", B(r).c_str(), n);
	char buf[32];
	r = ::ReadFile(h, buf, sizeof buf, &n, NULL);
	out("ReadFile", "ateof", "%s n=%lu", B(r).c_str(), n);
	DWORD p = ::SetFilePointer(h, 100, NULL, FILE_BEGIN);
	out("SetFilePointer", "beyond", "%lu", p);
	r = ::ReadFile(h, buf, sizeof buf, &n, NULL);
	out("ReadFile", "beyondeof", "%s n=%lu", B(r).c_str(), n);
	r = ::WriteFile(h, "Z", 1, &n, NULL);
	out("WriteFile", "beyondeof", "%s n=%lu size=%lu", B(r).c_str(), n, ::GetFileSize(h, NULL));
	::SetFilePointer(h, 10, NULL, FILE_BEGIN);
	r = ::ReadFile(h, buf, 4, &n, NULL);
	out("ReadFile", "gap", "%s n=%lu %s", B(r).c_str(), n, Hex(buf, n).c_str());
	r = ::WriteFile(h, "", 0, &n, NULL);
	out("WriteFile", "zero", "%s n=%lu", B(r).c_str(), n);
	// SetFilePointer edge cases (ZIPFile.cpp seeks relative to FILE_END).
	::SetLastError(0);
	p = ::SetFilePointer(h, -5, NULL, FILE_END);
	out("SetFilePointer", "end-5", "%lu err=%lu", p, ::GetLastError());
	::SetLastError(0);
	p = ::SetFilePointer(h, -1000, NULL, FILE_END);
	out("SetFilePointer", "negative", "%ld err=%lu", (LONG)p, ::GetLastError());
	::SetLastError(0);
	p = ::SetFilePointer(h, 3, NULL, FILE_CURRENT);
	out("SetFilePointer", "current+3", "%lu err=%lu", p, ::GetLastError());
	LONG hi = 1;
	::SetLastError(0);
	p = ::SetFilePointer(h, 5, &hi, FILE_BEGIN);
	out("SetFilePointer", "4G+5", "%lu hi=%ld err=%lu", p, hi, ::GetLastError());
	::SetLastError(0);
	p = ::SetFilePointer(h, 0, NULL, 7);
	out("SetFilePointer", "badmethod", "%ld err=%lu", (LONG)p, ::GetLastError());
	LARGE_INTEGER li, np;
	li.QuadPart = 0x123456789LL;
	r = ::SetFilePointerEx(h, li, &np, FILE_BEGIN);
	out("SetFilePointerEx", "big", "%s np=%llx", B(r).c_str(), np.QuadPart);
	li.QuadPart = -1;
	np.QuadPart = 7;
	::SetFilePointerEx(h, LARGE_INTEGER{}, NULL, FILE_BEGIN);
	r = ::SetFilePointerEx(h, li, &np, FILE_CURRENT);
	out("SetFilePointerEx", "negative", "%s np=%lld", B(r).c_str(), np.QuadPart);
	li.QuadPart = 0;
	r = ::SetFilePointerEx(h, li, &np, FILE_END);
	out("SetFilePointerEx", "end", "%s np=%lld", B(r).c_str(), np.QuadPart);
	r = ::SetFilePointerEx(h, li, NULL, FILE_CURRENT);
	out("SetFilePointerEx", "nullout", "%s", B(r).c_str());
	DWORD high = 77;
	DWORD lo = ::GetFileSize(h, &high);
	out("GetFileSize", "file", "%lu high=%lu", lo, high);
	::CloseHandle(h);
	// GetFileSize on things that are not files.
	HANDLE hd = ::CreateFileW(TDir().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	::SetLastError(0);
	lo = ::GetFileSize(hd, NULL);
	out("GetFileSize", "dir", "%lu err=%lu", lo, ::GetLastError());
	::CloseHandle(hd);
	HANDLE pr, pw;
	::CreatePipe(&pr, &pw, NULL, 0);
	::SetLastError(0);
	lo = ::GetFileSize(pr, NULL);
	out("GetFileSize", "pipe", "%ld err=%lu", (LONG)lo, ::GetLastError());
	// ReadFile / WriteFile on a pipe and on a read-only handle.
	r = ::WriteFile(pr, "x", 1, &n, NULL);
	out("WriteFile", "pipe.readend", "%s", B(r).c_str());
	r = ::WriteFile(pw, "x", 1, &n, NULL);
	out("WriteFile", "pipe.writeend", "%s n=%lu", B(r).c_str(), n);
	::CloseHandle(pw);
	r = ::ReadFile(pr, buf, 8, &n, NULL);
	out("ReadFile", "pipe.data", "%s n=%lu", B(r).c_str(), n);
	r = ::ReadFile(pr, buf, 8, &n, NULL);
	out("ReadFile", "pipe.broken", "%s n=%lu", B(r).c_str(), n);
	::CloseHandle(pr);
	h = ::CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::WriteFile(h, "x", 1, &n, NULL);
	out("WriteFile", "readonlyhandle", "%s", B(r).c_str());
	r = ::FlushFileBuffers(h);
	out("FlushFileBuffers", "readonlyhandle", "%s", B(r).c_str());
	::CloseHandle(h);
	h = ::CreateFileW(f.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::FlushFileBuffers(h);
	out("FlushFileBuffers", "writehandle", "%s", B(r).c_str());
	::CloseHandle(h);
	::CreatePipe(&pr, &pw, NULL, 0);
	r = ::FlushFileBuffers(pw);
	out("FlushFileBuffers", "pipe", "%s", B(r).c_str());
	::CloseHandle(pr);
	::CloseHandle(pw);
	::SetLastError(0);
	r = ::FlushFileBuffers(INVALID_HANDLE_VALUE);
	out("FlushFileBuffers", "invalid", "%s", B(r).c_str());
}

TEST(file_Overlapped)
{
	// PartFileWriteThread / UploadDiskIOThread: FILE_FLAG_OVERLAPPED handles
	// tied to a completion port; ReadFile/WriteFile with an OVERLAPPED and a
	// NULL byte count. Whether a call completes inline or pends is the
	// platform's choice, but a completion packet must be queued either way.
	const std::wstring f = TDir() + L"ov.bin";
	std::string data(65536, 'q');
	WriteWholeFile(f, data.data(), (DWORD)data.size());
	HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
	HANDLE h = ::CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	HANDLE assoc = ::CreateIoCompletionPort(h, port, 0x55, 0);
	out("CreateIoCompletionPort", "assoc", "%s", assoc == port ? "same" : "other");
	struct { const char *n; bool write; LONGLONG off; DWORD len; } c[] = {
		{"read.0", false, 0, 4096}, {"read.tail", false, 65530, 100}, {"read.eof", false, 65536, 10}, {"read.beyond", false, 1 << 20, 10},
		{"write.0", true, 0, 10}, {"write.extend", true, 70000, 10}, {"write.zero", true, 5, 0}};
	for (auto &x : c) {
		OVERLAPPED ov = {0};
		ov.Offset = (DWORD)x.off;
		ov.OffsetHigh = (DWORD)(x.off >> 32);
		static char buf[8192];
		::SetLastError(0);
		BOOL r = x.write ? ::WriteFile(h, data.data(), x.len, NULL, &ov) : ::ReadFile(h, buf, x.len, NULL, &ov);
		DWORD e = ::GetLastError();
		// Whatever the immediate answer, see what the port delivers.
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		LPOVERLAPPED pov = NULL;
		BOOL g = ::GetQueuedCompletionStatus(port, &bytes, &key, &pov, 2000);
		DWORD ge = g ? 0 : ::GetLastError();
		const char *imm = r ? "inline" : e == ERROR_IO_PENDING ? "pending" : "error";
		out(x.write ? "WriteFile" : "ReadFile", Fmt("ov.%s", x.n).c_str(), "imm=%s err=%lu port=%d porterr=%lu bytes=%lu key=%lx ov=%s status=%lx",
			imm, r ? 0 : e, g, ge, bytes, (unsigned long)key, pov == &ov ? "same" : pov ? "other" : "NULL", (unsigned long)ov.Internal);
	}
	// PostQueuedCompletionStatus with a NULL overlapped (eMule's WAKEUP / quit).
	BOOL r = ::PostQueuedCompletionStatus(port, 0, 0xABC, NULL);
	DWORD bytes = 1;
	ULONG_PTR key = 0;
	LPOVERLAPPED pov = (LPOVERLAPPED)1;
	BOOL g = ::GetQueuedCompletionStatus(port, &bytes, &key, &pov, 1000);
	out("PostQueuedCompletionStatus", "wakeup", "%s get=%d key=%lx bytes=%lu ov=%s", B(r).c_str(), g, (unsigned long)key, bytes, pov ? "nonnull" : "NULL");
	::SetLastError(0);
	g = ::GetQueuedCompletionStatus(port, &bytes, &key, &pov, 0);
	out("GetQueuedCompletionStatus", "empty.timeout0", "r=%d err=%lu ov=%s", g, ::GetLastError(), pov ? "nonnull" : "NULL");
	// CancelIo on a handle with nothing pending.
	r = ::CancelIo(h);
	out("CancelIo", "nothing", "%s", B(r).c_str());
	::CloseHandle(h);
	// Closing the port while a thread waits on it.
	HANDLE th = ::CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
		DWORD b;
		ULONG_PTR k;
		LPOVERLAPPED o;
		BOOL g = ::GetQueuedCompletionStatus((HANDLE)p, &b, &k, &o, 5000);
		return g ? 0 : ::GetLastError();
	}, port, 0, NULL);
	::Sleep(200);
	::CloseHandle(port);
	DWORD w = ::WaitForSingleObject(th, 6000);
	DWORD code = 0;
	::GetExitCodeThread(th, &code);
	out("GetQueuedCompletionStatus", "portclosed", "wait=%lu err=%lu", w, code);
	::CloseHandle(th);
}

TEST(file_GetFileType)
{
	HANDLE h = ::CreateFileW((TDir() + L"t.txt").c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	out("GetFileType", "disk", "%lu", ::GetFileType(h));
	::CloseHandle(h);
	HANDLE pr, pw;
	::CreatePipe(&pr, &pw, NULL, 0);
	out("GetFileType", "pipe", "%lu", ::GetFileType(pr));
	::CloseHandle(pr);
	::CloseHandle(pw);
	h = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	out("GetFileType", "nul", "%lu", ::GetFileType(h));
	::CloseHandle(h);
	::SetLastError(0);
	DWORD t = ::GetFileType(INVALID_HANDLE_VALUE);
	out("GetFileType", "invalid", "%lu err=%lu", t, ::GetLastError());
	SOCKET s = INVALID_SOCKET;
	WSADATA wd;
	::WSAStartup(MAKEWORD(2, 2), &wd);
	s = ::socket(AF_INET, SOCK_STREAM, 0);
	out("GetFileType", "socket", "%lu", ::GetFileType((HANDLE)s));
	::closesocket(s);
	h = ::CreateEventW(NULL, FALSE, FALSE, NULL);
	::SetLastError(0);
	t = ::GetFileType(h);
	out("GetFileType", "event", "%lu err=%lu", t, ::GetLastError());
	::CloseHandle(h);
}

TEST(file_Attributes)
{
	const std::wstring f = TDir() + L"a.txt", d = TDir() + L"d";
	WriteWholeFile(f, "hello", 5);
	::CreateDirectoryW(d.c_str(), NULL);
	WriteWholeFile(TDir() + L"ro.txt", "x", 1);
	::SetFileAttributesW((TDir() + L"ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);
	WriteWholeFile(TDir() + L"hid.txt", "x", 1);
	BOOL sr = ::SetFileAttributesW((TDir() + L"hid.txt").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
	out("SetFileAttributesW", "hidden.system", "%s", B(sr).c_str());
	struct { const char *n; std::wstring p; } c[] = {
		{"file", f}, {"dir", d}, {"dir.slash", d + L"\\"}, {"readonly", TDir() + L"ro.txt"}, {"hidden", TDir() + L"hid.txt"},
		{"missing", TDir() + L"nope"}, {"missing.parent", TDir() + L"nope\\x"}, {"file.slash", f + L"\\"}, {"root", L"C:\\"},
		{"drive", L"C:"}, {"empty", L""}, {"wild", TDir() + L"*"}, {"tdir", TDir()}, {"dot", d + L"\\."}, {"dotdot", d + L"\\.."},
		{"file.trailingdot", f + L"."}, {"nul", L"NUL"}};
	for (auto &x : c) {
		::SetLastError(0);
		DWORD a = ::GetFileAttributesW(x.p.c_str());
		DWORD e = ::GetLastError();
		out("GetFileAttributesW", x.n, "%lx err=%lu", a, a == INVALID_FILE_ATTRIBUTES ? e : 0);
		WIN32_FILE_ATTRIBUTE_DATA fa = {0};
		BOOL r = ::GetFileAttributesExW(x.p.c_str(), GetFileExInfoStandard, &fa);
		std::string rs = B(r);
		out("GetFileAttributesExW", x.n, "%s attr=%lx size=%lu ctime=%d atime=%d mtime=%d", rs.c_str(), fa.dwFileAttributes, fa.nFileSizeLow,
			fa.ftCreationTime.dwHighDateTime != 0, fa.ftLastAccessTime.dwHighDateTime != 0, fa.ftLastWriteTime.dwHighDateTime != 0);
	}
	::SetLastError(0);
	WIN32_FILE_ATTRIBUTE_DATA fa;
	BOOL r = ::GetFileAttributesExW(f.c_str(), (GET_FILEEX_INFO_LEVELS)5, &fa);
	out("GetFileAttributesExW", "badlevel", "%s", B(r).c_str());
	::SetFileAttributesW((TDir() + L"ro.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
	::SetFileAttributesW((TDir() + L"hid.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST(file_InformationByHandle)
{
	const std::wstring f = TDir() + L"i.txt";
	WriteWholeFile(f, "hello", 5);
	HANDLE h = ::CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	BY_HANDLE_FILE_INFORMATION fi = {0};
	BOOL r = ::GetFileInformationByHandle(h, &fi);
	out("GetFileInformationByHandle", "file", "%s attr=%lx links=%lu size=%lu serial=%d index=%d", B(r).c_str(), fi.dwFileAttributes,
		fi.nNumberOfLinks, fi.nFileSizeLow, fi.dwVolumeSerialNumber != 0, (fi.nFileIndexHigh | fi.nFileIndexLow) != 0);
	HANDLE h2 = ::CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	BY_HANDLE_FILE_INFORMATION fi2 = {0};
	::GetFileInformationByHandle(h2, &fi2);
	out("GetFileInformationByHandle", "sameindex", "%d", fi.nFileIndexLow == fi2.nFileIndexLow && fi.nFileIndexHigh == fi2.nFileIndexHigh);
	::CloseHandle(h2);
	::CloseHandle(h);
	HANDLE hd = ::CreateFileW(TDir().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	r = ::GetFileInformationByHandle(hd, &fi);
	out("GetFileInformationByHandle", "dir", "%s attr=%lx", B(r).c_str(), fi.dwFileAttributes);
	::CloseHandle(hd);
	HANDLE pr, pw;
	::CreatePipe(&pr, &pw, NULL, 0);
	r = ::GetFileInformationByHandle(pr, &fi);
	out("GetFileInformationByHandle", "pipe", "%s", B(r).c_str());
	::CloseHandle(pr);
	::CloseHandle(pw);
}

TEST(file_FileTime)
{
	// PartFile.cpp:419-425 - read creation and write time, then write the
	// modification time into the CREATION slot only.
	const std::wstring f = TDir() + L"t.part";
	WriteWholeFile(f, "x", 1);
	HANDLE h = ::CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	FILETIME ft;
	SYSTEMTIME st = {2020, 5, 0, 17, 12, 34, 56, 789};
	::SystemTimeToFileTime(&st, &ft);
	ft.dwLowDateTime += 1234;	// sub-millisecond part, 100 ns units
	BOOL r = ::SetFileTime(h, NULL, NULL, &ft);
	out("SetFileTime", "mtime", "%s", B(r).c_str());
	FILETIME c = {0}, a = {0}, m = {0};
	r = ::GetFileTime(h, &c, &a, &m);
	out("GetFileTime", "mtime.readback", "%s m=%08lx%08lx exact=%d", B(r).c_str(), m.dwHighDateTime, m.dwLowDateTime,
		m.dwLowDateTime == ft.dwLowDateTime && m.dwHighDateTime == ft.dwHighDateTime);
	FILETIME ct = ft;
	ct.dwHighDateTime -= 100;
	r = ::SetFileTime(h, &ct, NULL, NULL);
	out("SetFileTime", "ctime", "%s", B(r).c_str());
	r = ::GetFileTime(h, &c, NULL, &m);
	out("GetFileTime", "ctime.readback", "%s exact=%d mtime_unchanged=%d", B(r).c_str(),
		c.dwLowDateTime == ct.dwLowDateTime && c.dwHighDateTime == ct.dwHighDateTime,
		m.dwLowDateTime == ft.dwLowDateTime && m.dwHighDateTime == ft.dwHighDateTime);
	// 0xFFFFFFFF / zero FILETIMEs have special meanings in SetFileTime.
	FILETIME keep = {0xFFFFFFFF, 0xFFFFFFFF};
	r = ::SetFileTime(h, NULL, NULL, &keep);
	::GetFileTime(h, NULL, NULL, &m);
	out("SetFileTime", "minus1", "%s mtime_unchanged=%d", B(r).c_str(), m.dwLowDateTime == ft.dwLowDateTime && m.dwHighDateTime == ft.dwHighDateTime);
	FILETIME zero = {0, 0};
	r = ::SetFileTime(h, NULL, NULL, &zero);
	::GetFileTime(h, NULL, NULL, &m);
	out("SetFileTime", "zero", "%s mtime_unchanged=%d", B(r).c_str(), m.dwLowDateTime == ft.dwLowDateTime && m.dwHighDateTime == ft.dwHighDateTime);
	// Pre-1970 and far-future dates.
	SYSTEMTIME old = {1960, 1, 0, 1, 0, 0, 0, 0}, fut = {2200, 1, 0, 1, 0, 0, 0, 0};
	for (SYSTEMTIME *s : {&old, &fut}) {
		FILETIME t;
		::SystemTimeToFileTime(s, &t);
		r = ::SetFileTime(h, NULL, NULL, &t);
		::GetFileTime(h, NULL, NULL, &m);
		out("SetFileTime", Fmt("year%u", s->wYear).c_str(), "%s exact=%d", B(r).c_str(), m.dwLowDateTime == t.dwLowDateTime && m.dwHighDateTime == t.dwHighDateTime);
	}
	::CloseHandle(h);
	h = ::CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::SetFileTime(h, NULL, NULL, &ft);
	out("SetFileTime", "readonlyhandle", "%s", B(r).c_str());
	::CloseHandle(h);
	// Does a write move the modification time on close or on write?
	h = ::CreateFileW(f.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	::SetFileTime(h, NULL, NULL, &ft);
	DWORD n;
	::WriteFile(h, "y", 1, &n, NULL);
	::GetFileTime(h, NULL, NULL, &m);
	bool changedOnWrite = !(m.dwLowDateTime == ft.dwLowDateTime && m.dwHighDateTime == ft.dwHighDateTime);
	::CloseHandle(h);
	WIN32_FILE_ATTRIBUTE_DATA fa;
	::GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa);
	bool changedAfterClose = !(fa.ftLastWriteTime.dwLowDateTime == ft.dwLowDateTime && fa.ftLastWriteTime.dwHighDateTime == ft.dwHighDateTime);
	out("GetFileTime", "afterwrite", "beforeclose=%d afterclose=%d", changedOnWrite, changedAfterClose);
}

TEST(file_CopyFileW)
{
	const std::wstring s = TDir() + L"src.txt", d = TDir() + L"dst.txt";
	WriteWholeFile(s, "source", 6);
	FILETIME ft;
	SYSTEMTIME st = {2021, 3, 0, 4, 5, 6, 7, 0};
	::SystemTimeToFileTime(&st, &ft);
	HANDLE h = ::CreateFileW(s.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	::SetFileTime(h, NULL, NULL, &ft);
	::CloseHandle(h);
	::SetFileAttributesW(s.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE);
	BOOL r = ::CopyFileW(s.c_str(), d.c_str(), FALSE);
	out("CopyFileW", "new", "%s content=%s", B(r).c_str(), ReadAll(d).c_str());
	WIN32_FILE_ATTRIBUTE_DATA fa;
	::GetFileAttributesExW(d.c_str(), GetFileExInfoStandard, &fa);
	out("CopyFileW", "new.attrs", "attr=%lx mtime_kept=%d", fa.dwFileAttributes,
		fa.ftLastWriteTime.dwLowDateTime == ft.dwLowDateTime && fa.ftLastWriteTime.dwHighDateTime == ft.dwHighDateTime);
	r = ::CopyFileW(s.c_str(), d.c_str(), TRUE);
	out("CopyFileW", "exists.failifexists", "%s", B(r).c_str());
	r = ::CopyFileW(s.c_str(), d.c_str(), FALSE);
	out("CopyFileW", "exists.readonlytarget", "%s", B(r).c_str());
	::SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
	WriteWholeFile(d, "longer old content", 18);
	r = ::CopyFileW(s.c_str(), d.c_str(), FALSE);
	out("CopyFileW", "overwrite", "%s content=%s", B(r).c_str(), ReadAll(d).c_str());
	::SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
	r = ::CopyFileW((TDir() + L"none").c_str(), d.c_str(), FALSE);
	out("CopyFileW", "missing.src", "%s", B(r).c_str());
	r = ::CopyFileW(s.c_str(), (TDir() + L"nodir\\x").c_str(), FALSE);
	out("CopyFileW", "missing.dstdir", "%s", B(r).c_str());
	r = ::CopyFileW(s.c_str(), s.c_str(), FALSE);
	out("CopyFileW", "self", "%s", B(r).c_str());
	::SetFileAttributesW(s.c_str(), FILE_ATTRIBUTE_NORMAL);
	h = ::CreateFileW(s.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	r = ::CopyFileW(s.c_str(), (TDir() + L"locked.txt").c_str(), FALSE);
	out("CopyFileW", "src.lockedexclusive", "%s", B(r).c_str());
	::CloseHandle(h);
	h = ::CreateFileW(s.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::CopyFileW(s.c_str(), (TDir() + L"shared.txt").c_str(), FALSE);
	out("CopyFileW", "src.openshareread", "%s", B(r).c_str());
	::CloseHandle(h);
	h = ::CreateFileW(d.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::CopyFileW(s.c_str(), d.c_str(), FALSE);
	out("CopyFileW", "dst.openbyreader", "%s", B(r).c_str());
	::CloseHandle(h);
	::CreateDirectoryW((TDir() + L"dir").c_str(), NULL);
	r = ::CopyFileW((TDir() + L"dir").c_str(), (TDir() + L"dir2").c_str(), FALSE);
	out("CopyFileW", "directory", "%s", B(r).c_str());
	r = ::CopyFileW(s.c_str(), (TDir() + L"dir").c_str(), FALSE);
	out("CopyFileW", "ontodirectory", "%s", B(r).c_str());
}

static DWORD CALLBACK Progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD stream, DWORD reason, HANDLE, HANDLE, LPVOID ctx)
{
	std::string *log = (std::string*)ctx;
	*log += Fmt("[%lld/%lld s%lu r%lu]", done.QuadPart, total.QuadPart, stream, reason);
	return PROGRESS_CONTINUE;
}

TEST(file_MoveFile)
{
	const std::wstring a = TDir() + L"a.txt", b = TDir() + L"b.txt";
	WriteWholeFile(a, "A", 1);
	BOOL r = ::MoveFileW(a.c_str(), b.c_str());
	out("MoveFileW", "rename", "%s a=%s b=%s", B(r).c_str(), Exists(a).c_str(), ReadAll(b).c_str());
	WriteWholeFile(a, "A2", 2);
	r = ::MoveFileW(a.c_str(), b.c_str());
	out("MoveFileW", "exists", "%s", B(r).c_str());
	r = ::MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	out("MoveFileExW", "replace", "%s b=%s a=%s", B(r).c_str(), ReadAll(b).c_str(), Exists(a).c_str());
	WriteWholeFile(a, "A3", 2);
	r = ::MoveFileExW(a.c_str(), b.c_str(), 0);
	out("MoveFileExW", "noreplace", "%s", B(r).c_str());
	::SetFileAttributesW(b.c_str(), FILE_ATTRIBUTE_READONLY);
	r = ::MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	out("MoveFileExW", "replace.readonlytarget", "%s", B(r).c_str());
	::SetFileAttributesW(b.c_str(), FILE_ATTRIBUTE_NORMAL);
	HANDLE h = ::CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	out("MoveFileExW", "replace.targetopen", "%s", B(r).c_str());
	::CloseHandle(h);
	h = ::CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	r = ::MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	out("MoveFileExW", "replace.targetopen.sharedelete", "%s b=%s", B(r).c_str(), ReadAll(b).c_str());
	::CloseHandle(h);
	WriteWholeFile(a, "A4", 2);
	h = ::CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::MoveFileW(a.c_str(), (TDir() + L"c.txt").c_str());
	out("MoveFileW", "srcopen.noshare", "%s", B(r).c_str());
	::CloseHandle(h);
	h = ::CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	r = ::MoveFileW(a.c_str(), (TDir() + L"c.txt").c_str());
	out("MoveFileW", "srcopen.sharedelete", "%s", B(r).c_str());
	::CloseHandle(h);
	const std::wstring c = TDir() + L"c.txt", C = TDir() + L"C.TXT";
	r = ::MoveFileW(c.c_str(), C.c_str());
	WIN32_FIND_DATAW fd;
	HANDLE hf = ::FindFirstFileW((TDir() + L"c.*").c_str(), &fd);
	out("MoveFileW", "caseonly", "%s name=%s", B(r).c_str(), hf != INVALID_HANDLE_VALUE ? Q(fd.cFileName).c_str() : "none");
	if (hf != INVALID_HANDLE_VALUE)
		::FindClose(hf);
	r = ::MoveFileW((TDir() + L"none").c_str(), (TDir() + L"x").c_str());
	out("MoveFileW", "missing", "%s", B(r).c_str());
	r = ::MoveFileW(C.c_str(), (TDir() + L"nodir\\x").c_str());
	out("MoveFileW", "missing.dstdir", "%s", B(r).c_str());
	::CreateDirectoryW((TDir() + L"d1").c_str(), NULL);
	WriteWholeFile(TDir() + L"d1\\in.txt", "i", 1);
	r = ::MoveFileW((TDir() + L"d1").c_str(), (TDir() + L"d2").c_str());
	out("MoveFileW", "directory", "%s in=%s", B(r).c_str(), Exists(TDir() + L"d2\\in.txt").c_str());
	r = ::MoveFileW((TDir() + L"d2").c_str(), (TDir() + L"d2\\sub").c_str());
	out("MoveFileW", "dir.intoself", "%s", B(r).c_str());
	r = ::MoveFileExW(C.c_str(), (TDir() + L"d2").c_str(), MOVEFILE_REPLACE_EXISTING);
	out("MoveFileExW", "replace.directory", "%s", B(r).c_str());
	// MoveFileWithProgress with MOVEFILE_COPY_ALLOWED (PartFile.cpp completing
	// a download into the incoming directory): same volume here.
	std::string log;
	WriteWholeFile(TDir() + L"p.part", "0123456789", 10);
	r = ::MoveFileWithProgressW((TDir() + L"p.part").c_str(), (TDir() + L"done.bin").c_str(), Progress, &log, MOVEFILE_COPY_ALLOWED);
	out("MoveFileWithProgressW", "samevolume", "%s callbacks=%s", B(r).c_str(), log.empty() ? "none" : log.c_str());
	log.clear();
	WriteWholeFile(TDir() + L"p.part", "0123456789", 10);
	r = ::MoveFileWithProgressW((TDir() + L"p.part").c_str(), (TDir() + L"done.bin").c_str(), Progress, &log, MOVEFILE_COPY_ALLOWED);
	out("MoveFileWithProgressW", "exists", "%s callbacks=%s", B(r).c_str(), log.empty() ? "none" : log.c_str());
	// ANSI rename with a CP1252 character (MoveFileExA is in the import table).
	WriteWholeFile(TDir() + L"ansi.txt", "x", 1);
	std::string an = Narrow(TDir().c_str());
	r = ::MoveFileExA((an + "ansi.txt").c_str(), (an + "\xe4nsi.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
	out("MoveFileExA", "cp1252", "%s exists=%s", B(r).c_str(), Exists(TDir() + L"\u00e4nsi.txt").c_str());
}

TEST(file_Directories)
{
	const std::wstring d = TDir() + L"dir";
	BOOL r = ::CreateDirectoryW(d.c_str(), NULL);
	out("CreateDirectoryW", "new", "%s", B(r).c_str());
	r = ::CreateDirectoryW(d.c_str(), NULL);
	out("CreateDirectoryW", "exists", "%s", B(r).c_str());
	r = ::CreateDirectoryW((TDir() + L"a\\b").c_str(), NULL);
	out("CreateDirectoryW", "missingparent", "%s", B(r).c_str());
	r = ::CreateDirectoryW((TDir() + L"slash\\").c_str(), NULL);
	out("CreateDirectoryW", "trailingslash", "%s", B(r).c_str());
	WriteWholeFile(TDir() + L"file", "x", 1);
	r = ::CreateDirectoryW((TDir() + L"file").c_str(), NULL);
	out("CreateDirectoryW", "fileexists", "%s", B(r).c_str());
	r = ::CreateDirectoryW(L"C:\\", NULL);
	out("CreateDirectoryW", "root", "%s", B(r).c_str());
	r = ::CreateDirectoryW((TDir() + L"bad<name").c_str(), NULL);
	out("CreateDirectoryW", "badchar", "%s", B(r).c_str());
	r = ::CreateDirectoryW((TDir() + L"dotted.").c_str(), NULL);
	out("CreateDirectoryW", "trailingdot", "%s", B(r).c_str());
	r = ::CreateDirectoryW(L"", NULL);
	out("CreateDirectoryW", "empty", "%s", B(r).c_str());
	WriteWholeFile(d + L"\\x", "x", 1);
	r = ::RemoveDirectoryW(d.c_str());
	out("RemoveDirectoryW", "notempty", "%s", B(r).c_str());
	::DeleteFileW((d + L"\\x").c_str());
	r = ::RemoveDirectoryW((TDir() + L"file").c_str());
	out("RemoveDirectoryW", "isfile", "%s", B(r).c_str());
	r = ::RemoveDirectoryW((TDir() + L"none").c_str());
	out("RemoveDirectoryW", "missing", "%s", B(r).c_str());
	::SetCurrentDirectoryW(d.c_str());
	r = ::RemoveDirectoryW(d.c_str());
	out("RemoveDirectoryW", "iscwd", "%s", B(r).c_str());
	::SetCurrentDirectoryW(TDir().c_str());
	if (!r)
		r = ::RemoveDirectoryW(d.c_str());
	out("RemoveDirectoryW", "empty", "%s", B(r).c_str());
	::CreateDirectoryW(d.c_str(), NULL);
	HANDLE h = ::CreateFileW(d.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	r = ::RemoveDirectoryW(d.c_str());
	out("RemoveDirectoryW", "openhandle.noshare", "%s exists=%s", B(r).c_str(), Exists(d).c_str());
	::CloseHandle(h);
	::CreateDirectoryW(d.c_str(), NULL);
	::SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_READONLY);
	r = ::RemoveDirectoryW(d.c_str());
	out("RemoveDirectoryW", "readonly", "%s", B(r).c_str());
	::SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
}

static std::string FindAll(const std::wstring &pattern, bool sortNames)
{
	WIN32_FIND_DATAW fd;
	::SetLastError(0);
	HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return Fmt("fail err=%lu", ::GetLastError());
	std::vector<std::string> names;
	do {
		names.push_back(Narrow(fd.cFileName) + Fmt("(%lx,%lu)", fd.dwFileAttributes & ~FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, fd.nFileSizeLow));
	} while (::FindNextFileW(h, &fd));
	DWORD e = ::GetLastError();
	BOOL c = ::FindClose(h);
	if (sortNames)
		std::sort(names.begin(), names.end());
	std::string r;
	for (auto &n : names)
		r += n + " ";
	return r + Fmt("end=%lu close=%d", e, c);
}

TEST(file_Find)
{
	for (const wchar_t *n : {L"b.txt", L"a.txt", L"C.TXT", L"a1.txt", L"ab.txt", L"a.txtx", L"noext", L"x.htm", L"x.html",
			L"\u00e9.txt", L"_z.txt", L"Z.txt", L"10.txt", L"9.txt", L".hidden"}) {
		WriteWholeFile(TDir() + n, "x", 1);
	}
	::CreateDirectoryW((TDir() + L"sub").c_str(), NULL);
	::CreateDirectoryW((TDir() + L"empty").c_str(), NULL);
	// Order as returned: eMule fills lists in this order before sorting.
	out("FindFirstFileW", "order.star", "%s", FindAll(TDir() + L"*", false).c_str());
	for (const wchar_t *p : {L"*", L"*.*", L"*.txt", L"*.TXT", L"a?.txt", L"*.", L"a*", L"*.htm", L"*.ht?", L"x.htm*", L"<.txt",
			L"empty\\*", L"empty\\*.txt", L"nodir\\*", L"b.txt", L"b.txt\\", L"sub", L"sub\\", L"*\\*", L"???.txt"}) {
		out("FindFirstFileW", Q(p).c_str(), "%s", FindAll(TDir() + p, true).c_str());
	}
	WIN32_FIND_DATAW fd;
	::SetLastError(0);
	BOOL r = ::FindNextFileW(INVALID_HANDLE_VALUE, &fd);
	out("FindNextFileW", "invalid", "%s", B(r).c_str());
	r = ::FindClose(INVALID_HANDLE_VALUE);
	out("FindClose", "invalid", "%s", B(r).c_str());
	// Root of the drive: are "." and ".." reported?
	HANDLE h = ::FindFirstFileW(L"C:\\*", &fd);
	bool dot = false, dotdot = false;
	if (h != INVALID_HANDLE_VALUE) {
		do {
			dot |= !wcscmp(fd.cFileName, L".");
			dotdot |= !wcscmp(fd.cFileName, L"..");
		} while (::FindNextFileW(h, &fd));
		::FindClose(h);
	}
	out("FindFirstFileW", "root.dots", "dot=%d dotdot=%d", dot, dotdot);
	// A file created while enumerating.
	h = ::FindFirstFileW((TDir() + L"sub\\*").c_str(), &fd);
	WriteWholeFile(TDir() + L"sub\\late.txt", "x", 1);
	int count = 0;
	if (h != INVALID_HANDLE_VALUE) {
		do
			++count;
		while (::FindNextFileW(h, &fd));
		::FindClose(h);
	}
	out("FindNextFileW", "~addedduring", "count=%d", count);
}

TEST(file_Volume)
{
	ULARGE_INTEGER avail, total, freeb;
	BOOL r = ::GetDiskFreeSpaceExW(TDir().c_str(), &avail, &total, &freeb);
	out("GetDiskFreeSpaceExW", "tdir", "%s nonzero=%d", B(r).c_str(), avail.QuadPart > 0);
	out("GetDiskFreeSpaceExW", "~tdir.value", "%llu", avail.QuadPart);
	WriteWholeFile(TDir() + L"f", "x", 1);
	for (auto c : {std::make_pair("file", TDir() + L"f"), std::make_pair("missing", TDir() + L"nope\\"), std::make_pair("root", std::wstring(L"C:\\")),
			std::make_pair("drive", std::wstring(L"C:")), std::make_pair("noslash", TDir().substr(0, TDir().size() - 1)),
			std::make_pair("unc", std::wstring(L"\\\\nosuchserver_sr\\share\\")), std::make_pair("null", std::wstring())}) {
		::SetLastError(0);
		r = ::GetDiskFreeSpaceExW(c.second.empty() ? NULL : c.second.c_str(), &avail, NULL, NULL);
		out("GetDiskFreeSpaceExW", c.first, "%s", B(r).c_str());
	}
	for (const wchar_t *p : {L"C:\\", L"C:", L"C:\\srt\\", L"C:\\srt", L"c:\\", L"\\\\nosuchserver_sr\\share\\", L"", L"1:\\"}) {
		out("GetDriveTypeW", Q(p).c_str(), "%u", ::GetDriveTypeW(*p ? p : NULL));
	}
	for (wchar_t d = L'D'; d <= L'Z'; ++d) {
		wchar_t root[] = {d, L':', L'\\', 0};
		out("GetDriveTypeW", Fmt("~%c", (char)d).c_str(), "%u", ::GetDriveTypeW(root));
	}
	wchar_t buf[512];
	DWORD n = ::GetLogicalDriveStringsW(_countof(buf) - 1, buf);
	bool wellFormed = n > 0 && buf[n] == 0;
	bool hasC = false;
	for (wchar_t *p = buf; *p; p += wcslen(p) + 1) {
		wellFormed &= wcslen(p) == 3 && p[1] == L':' && p[2] == L'\\';
		hasC |= (p[0] == L'C' || p[0] == L'c');
	}
	out("GetLogicalDriveStringsW", "format", "wellformed=%d hasC=%d", wellFormed, hasC);
	std::string all;
	for (wchar_t *p = buf; *p; p += wcslen(p) + 1)
		all += Narrow(p) + " ";
	out("GetLogicalDriveStringsW", "~drives", "%s", all.c_str());
	wchar_t small[4];
	DWORD need = ::GetLogicalDriveStringsW(3, small);
	out("GetLogicalDriveStringsW", "small", "need=n+1:%d", need == n + 1);
	DWORD maxlen = 0, flags = 0, serial = 0;
	wchar_t fs[MAX_PATH + 1] = L"", label[MAX_PATH + 1] = L"";
	r = ::GetVolumeInformationW(L"C:\\", NULL, 0, NULL, &maxlen, &flags, fs, _countof(fs));
	out("GetVolumeInformationW", "C", "%s fs=%s maxlen=%lu", B(r).c_str(), Q(fs).c_str(), maxlen);
	out("GetVolumeInformationW", "~C.flags", "%08lx", flags);
	const DWORD interesting = FILE_SUPPORTS_SPARSE_FILES | FILE_FILE_COMPRESSION | FILE_PERSISTENT_ACLS | FILE_UNICODE_ON_DISK |
		FILE_CASE_PRESERVED_NAMES | FILE_NAMED_STREAMS;
	out("GetVolumeInformationW", "C.flags.eMule", "%08lx", flags & interesting);
	r = ::GetVolumeInformationW(L"C:\\", label, _countof(label), &serial, NULL, NULL, NULL, 0);
	out("GetVolumeInformationW", "C.label", "%s serial=%d", B(r).c_str(), serial != 0);
	for (const wchar_t *p : {L"C:", L"C:\\srt\\", L"\\\\nosuchserver_sr\\share\\", L"Q:\\"}) {
		r = ::GetVolumeInformationW(p, NULL, 0, NULL, &maxlen, &flags, fs, _countof(fs));
		out("GetVolumeInformationW", Q(p).c_str(), "%s", B(r).c_str());
	}
}

TEST(file_Compression)
{
	// PartFile.cpp: FSCTL_SET_SPARSE on new part files, FSCTL_SET_COMPRESSION
	// on request; OtherFunctions: GetCompressedFileSize for the real size.
	const std::wstring f = TDir() + L"sparse.part";
	HANDLE h = ::CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD ret = 0;
	BOOL r = ::DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL);
	out("DeviceIoControl", "set_sparse", "%s", B(r).c_str());
	LARGE_INTEGER li;
	li.QuadPart = 64 << 20;
	::SetFilePointerEx(h, li, NULL, FILE_BEGIN);
	DWORD n;
	::WriteFile(h, "end", 3, &n, NULL);
	::CloseHandle(h);
	DWORD hi = 0;
	::SetLastError(0);
	DWORD lo = ::GetCompressedFileSizeW(f.c_str(), &hi);
	ULONGLONG comp = ((ULONGLONG)hi << 32) | lo;
	out("GetCompressedFileSizeW", "sparse64M", "err=%lu lessthanlogical=%d", ::GetLastError(), comp < (64ull << 20));
	out("GetCompressedFileSizeW", "~sparse64M.value", "%llu", comp);
	WriteWholeFile(TDir() + L"small", "abc", 3);
	lo = ::GetCompressedFileSizeW((TDir() + L"small").c_str(), &hi);
	out("GetCompressedFileSizeW", "small", "%lu hi=%lu", lo, hi);
	::SetLastError(0);
	lo = ::GetCompressedFileSizeW((TDir() + L"missing").c_str(), &hi);
	out("GetCompressedFileSizeW", "missing", "%lx err=%lu", lo, ::GetLastError());
	lo = ::GetCompressedFileSizeW((TDir() + L"small").c_str(), NULL);
	out("GetCompressedFileSizeW", "nullhigh", "%lu", lo);
	h = ::CreateFileW((TDir() + L"small").c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	USHORT fmt = COMPRESSION_FORMAT_DEFAULT;
	r = ::DeviceIoControl(h, FSCTL_SET_COMPRESSION, &fmt, sizeof fmt, NULL, 0, &ret, NULL);
	out("DeviceIoControl", "set_compression", "%s", B(r).c_str());
	fmt = COMPRESSION_FORMAT_NONE;
	r = ::DeviceIoControl(h, FSCTL_SET_COMPRESSION, &fmt, sizeof fmt, NULL, 0, &ret, NULL);
	out("DeviceIoControl", "clear_compression", "%s", B(r).c_str());
	::CloseHandle(h);
	h = ::CreateFileW((TDir() + L"small").c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	r = ::DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL);
	out("DeviceIoControl", "set_sparse.readonlyhandle", "%s", B(r).c_str());
	::CloseHandle(h);
}

TEST(file_SHFileOperationW)
{
	// ShellDeleteFile: FO_DELETE with FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
	// FOF_SILENT | FOF_NORECURSION (to the recycle bin), double-NUL list.
	struct { const char *n; const wchar_t *name; FILEOP_FLAGS fl; bool make; bool ro; } c[] = {
		{"bin", L"tobin.txt", FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NORECURSION, true, false},
		{"bin.readonly", L"ro.txt", FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NORECURSION, true, true},
		{"nobin", L"del.txt", FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI, true, false},
		{"missing", L"missing.txt", FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NORECURSION | FOF_NOERRORUI, false, false},
	};
	for (auto &x : c) {
		std::wstring p = TDir() + x.name;
		if (x.make)
			WriteWholeFile(p, "x", 1);
		if (x.ro)
			::SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY);
		wchar_t from[MAX_PATH + 1] = {0};
		wcsncpy_s(from, p.c_str(), _TRUNCATE);
		from[p.size() + 1] = 0;
		SHFILEOPSTRUCTW fo = {0};
		fo.wFunc = FO_DELETE;
		fo.pFrom = from;
		fo.fFlags = x.fl;
		int r = ::SHFileOperationW(&fo);
		out("SHFileOperationW", x.n, "r=%d aborted=%d gone=%d", r, fo.fAnyOperationsAborted, ::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES);
	}
}

TEST(file_StgOpenStorage)
{
	// IsThumbsDb: StgOpenStorage(STGM_READ | STGM_SHARE_DENY_WRITE) on a
	// file called thumbs.db, then EnumElements.
	::CoInitialize(NULL);
	const std::wstring f = TDir() + L"thumbs.db";
	IStorage *stg = NULL;
	HRESULT hr = ::StgCreateDocfile(f.c_str(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &stg);
	if (SUCCEEDED(hr)) {
		IStream *st = NULL;
		stg->CreateStream(L"Catalog", STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, 0, &st);
		if (st) {
			ULONG w;
			st->Write("abc", 3, &w);
			st->Release();
		}
		stg->Release();
	}
	out("StgOpenStorage", "fixture", "hr=%08lx", hr);
	struct { const char *n; std::wstring p; } c[] = {
		{"docfile", f}, {"plain", TDir() + L"plain.db"}, {"empty", TDir() + L"empty.db"}, {"missing", TDir() + L"none.db"}};
	WriteWholeFile(TDir() + L"plain.db", "not a storage file at all", 25);
	WriteWholeFile(TDir() + L"empty.db", "", 0);
	for (auto &x : c) {
		IStorage *p = NULL;
		hr = ::StgOpenStorage(x.p.c_str(), NULL, STGM_READ | STGM_SHARE_DENY_WRITE, NULL, 0, &p);
		std::string enumres = "-";
		if (p) {
			IEnumSTATSTG *e = NULL;
			if (SUCCEEDED(p->EnumElements(0, NULL, 0, &e))) {
				STATSTG s;
				HRESULT n = e->Next(1, &s, NULL);
				enumres = Fmt("next=%08lx", n);
				if (n == S_OK) {
					enumres += " " + Q(s.pwcsName);
					::CoTaskMemFree(s.pwcsName);
				}
				e->Release();
			}
			p->Release();
		}
		out("StgOpenStorage", x.n, "hr=%08lx %s", hr, enumres.c_str());
	}
	// Opened by a writer that denies nothing: does STGM_SHARE_DENY_WRITE fail?
	HANDLE h = ::CreateFileW(f.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	IStorage *p = NULL;
	hr = ::StgOpenStorage(f.c_str(), NULL, STGM_READ | STGM_SHARE_DENY_WRITE, NULL, 0, &p);
	out("StgOpenStorage", "openwriter", "hr=%08lx", hr);
	if (p)
		p->Release();
	::CloseHandle(h);
	::CoUninitialize();
}
