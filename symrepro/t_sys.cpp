// Threads, synchronisation, memory, modules, resources, exceptions, time,
// process-wide settings - KERNEL32, WINMM, ADVAPI32 token calls, VERSION,
// and the two DLLs eMule loads at run time (dbghelp, wmvcore).
#include "harness.h"
#include "resource.h"
#include <mmsystem.h>
#include <dbghelp.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Threads

static DWORD WINAPI ReturnArg(LPVOID p) { return (DWORD)(ULONG_PTR)p; }

TEST(sys_Thread)
{
	DWORD tid = 0;
	HANDLE h = ::CreateThread(NULL, 0, ReturnArg, (LPVOID)42, CREATE_SUSPENDED, &tid);
	out("CreateThread", "suspended", "%s tid!=self=%d", h ? "ok" : "fail", tid != ::GetCurrentThreadId());
	out("WaitForSingleObject", "suspended.timeout0", "%lu", ::WaitForSingleObject(h, 0));
	BOOL pr = ::SetThreadPriority(h, THREAD_PRIORITY_IDLE);
	out("SetThreadPriority", "idle", "%s get=%d", B(pr).c_str(), ::GetThreadPriority(h));
	pr = ::SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
	out("SetThreadPriority", "timecritical", "%s get=%d", B(pr).c_str(), ::GetThreadPriority(h));
	pr = ::SetThreadPriority(h, 7);
	out("SetThreadPriority", "invalid7", "%s get=%d", B(pr).c_str(), ::GetThreadPriority(h));
	pr = ::SetThreadPriority(h, THREAD_PRIORITY_NORMAL);
	DWORD s = ::SuspendThread(h);
	out("ResumeThread", "suspendcount.after_suspend", "prev=%lu", s);
	out("ResumeThread", "first", "%lu", ::ResumeThread(h));
	out("ResumeThread", "second", "%lu", ::ResumeThread(h));
	out("WaitForSingleObject", "thread.done", "%lu", ::WaitForSingleObject(h, 5000));
	DWORD code = 0;
	::GetExitCodeThread(h, &code);
	out("CreateThread", "exitcode", "%lu", code);
	::SetLastError(0);
	DWORD r = ::ResumeThread(h);
	out("ResumeThread", "finished", "%ld err=%lu", (LONG)r, ::GetLastError());
	::SetLastError(0);
	pr = ::SetThreadPriority(h, THREAD_PRIORITY_IDLE);
	out("SetThreadPriority", "finished", "%s", B(pr).c_str());
	::CloseHandle(h);
	::SetLastError(0);
	r = ::ResumeThread(NULL);
	out("ResumeThread", "null", "%ld err=%lu", (LONG)r, ::GetLastError());
	h = ::CreateThread(NULL, 0, ReturnArg, 0, 0, NULL);
	r = ::ResumeThread(h);
	out("ResumeThread", "notsuspended", "%lu", r);
	::WaitForSingleObject(h, 5000);
	::CloseHandle(h);
	// Thread ids differ, and the pseudo handle.
	DWORD other = 0;
	h = ::CreateThread(NULL, 0, [](LPVOID p) -> DWORD { *(DWORD*)p = ::GetCurrentThreadId(); return 0; }, &other, 0, NULL);
	::WaitForSingleObject(h, 5000);
	out("GetCurrentThreadId", "distinct", "%d", other != 0 && other != ::GetCurrentThreadId());
	out("GetCurrentThreadId", "matches.GetThreadId", "%d", ::GetThreadId(h) == other);
	::CloseHandle(h);
	out("GetCurrentProcess", "pseudo", "%Id", (INT_PTR)::GetCurrentProcess());
	out("GetCurrentProcessId", "matches.GetProcessId", "%d", ::GetProcessId(::GetCurrentProcess()) == ::GetCurrentProcessId());
	out("CloseHandle", "pseudoprocess", "%s", B(::CloseHandle(::GetCurrentProcess())).c_str());
}

TEST(sys_Event)
{
	HANDLE m = ::CreateEventW(NULL, TRUE, FALSE, NULL);	// WebSocket.cpp s_hTerminate
	HANDLE a = ::CreateEventW(NULL, FALSE, TRUE, NULL);	// WebSocket.cpp per-connection event
	out("CreateEventW", "manual.initial", "%lu", ::WaitForSingleObject(m, 0));
	out("CreateEventW", "auto.initial", "%lu", ::WaitForSingleObject(a, 0));
	out("CreateEventW", "auto.consumed", "%lu", ::WaitForSingleObject(a, 0));
	out("SetEvent", "manual", "%s", B(::SetEvent(m)).c_str());
	out("SetEvent", "manual.wait1", "%lu", ::WaitForSingleObject(m, 0));
	out("SetEvent", "manual.wait2", "%lu", ::WaitForSingleObject(m, 0));
	out("ResetEvent", "manual", "%s wait=%lu", B(::ResetEvent(m)).c_str(), ::WaitForSingleObject(m, 0));
	out("SetEvent", "twice", "%s %s", B(::SetEvent(a)).c_str(), B(::SetEvent(a)).c_str());
	out("SetEvent", "twice.wait1", "%lu", ::WaitForSingleObject(a, 0));
	out("SetEvent", "twice.wait2", "%lu", ::WaitForSingleObject(a, 0));
	::SetLastError(0);
	out("SetEvent", "invalid", "%s", B(::SetEvent(NULL)).c_str());
	HANDLE th = ::CreateThread(NULL, 0, ReturnArg, 0, 0, NULL);
	::WaitForSingleObject(th, 5000);
	out("SetEvent", "onthread", "%s", B(::SetEvent(th)).c_str());
	::CloseHandle(th);
	// Named events: same name returns the existing object.
	HANDLE n1 = ::CreateEventW(NULL, TRUE, FALSE, L"symrepro_ev");
	DWORD e1 = ::GetLastError();
	HANDLE n2 = ::CreateEventW(NULL, FALSE, TRUE, L"symrepro_ev");
	DWORD e2 = ::GetLastError();
	out("CreateEventW", "named", "first_err=%lu second_err=%lu second_state=%lu", e1, e2, ::WaitForSingleObject(n2, 0));
	::CloseHandle(n1);
	::CloseHandle(n2);
	::CloseHandle(m);
	::CloseHandle(a);
}

TEST(sys_Wait)
{
	HANDLE e[3];
	for (HANDLE &h : e)
		h = ::CreateEventW(NULL, TRUE, FALSE, NULL);
	out("WaitForMultipleObjects", "none", "%lu", ::WaitForMultipleObjects(3, e, FALSE, 0));
	::SetEvent(e[2]);
	out("WaitForMultipleObjects", "any.2", "%lu", ::WaitForMultipleObjects(3, e, FALSE, 0));
	::SetEvent(e[1]);
	out("WaitForMultipleObjects", "any.lowest", "%lu", ::WaitForMultipleObjects(3, e, FALSE, 0));
	out("WaitForMultipleObjects", "all.partial", "%lu", ::WaitForMultipleObjects(3, e, TRUE, 0));
	::SetEvent(e[0]);
	out("WaitForMultipleObjects", "all", "%lu", ::WaitForMultipleObjects(3, e, TRUE, 0));
	HANDLE dup[2] = {e[0], e[0]};
	::SetLastError(0);
	DWORD r = ::WaitForMultipleObjects(2, dup, TRUE, 0);
	out("WaitForMultipleObjects", "duplicate.all", "%lx err=%lu", r, ::GetLastError());
	r = ::WaitForMultipleObjects(2, dup, FALSE, 0);
	out("WaitForMultipleObjects", "duplicate.any", "%lx", r);
	HANDLE many[MAXIMUM_WAIT_OBJECTS + 1];
	for (HANDLE &h : many)
		h = e[0];
	::SetLastError(0);
	r = ::WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS + 1, many, FALSE, 0);
	out("WaitForMultipleObjects", "toomany", "%lx err=%lu", r, ::GetLastError());
	::SetLastError(0);
	r = ::WaitForMultipleObjects(0, e, FALSE, 0);
	out("WaitForMultipleObjects", "zero", "%lx err=%lu", r, ::GetLastError());
	HANDLE bad[2] = {e[0], (HANDLE)(ULONG_PTR)0x1234};
	::SetLastError(0);
	r = ::WaitForMultipleObjects(2, bad, FALSE, 0);
	out("WaitForMultipleObjects", "invalidhandle", "%lx err=%lu", r, ::GetLastError());
	::SetLastError(0);
	r = ::WaitForSingleObject((HANDLE)(ULONG_PTR)0x1234, 0);
	out("WaitForSingleObject", "invalidhandle", "%lx err=%lu", r, ::GetLastError());
	r = ::WaitForSingleObject(::GetCurrentProcess(), 0);
	out("WaitForSingleObject", "selfprocess", "%lx", r);
	// Abandoned mutex.
	HANDLE mtx = ::CreateMutexW(NULL, FALSE, NULL);
	HANDLE th = ::CreateThread(NULL, 0, [](LPVOID p) -> DWORD { ::WaitForSingleObject((HANDLE)p, 0); return 0; }, mtx, 0, NULL);
	::WaitForSingleObject(th, 5000);
	::CloseHandle(th);
	HANDLE w2[2] = {e[1], mtx};
	::ResetEvent(e[1]);
	r = ::WaitForMultipleObjects(2, w2, FALSE, 0);
	out("WaitForMultipleObjects", "abandoned", "%lx", r);
	r = ::WaitForSingleObject(mtx, 0);
	out("WaitForSingleObject", "abandoned.after", "%lx", r);
	// Timed wait accuracy, coarse: a 300 ms wait must not return early.
	DWORD t0 = ::GetTickCount();
	r = ::WaitForSingleObject(e[1], 300);
	DWORD dt = ::GetTickCount() - t0;
	out("WaitForSingleObject", "timeout300", "%lu notearly=%d", r, dt >= 280);
	for (HANDLE h : e)
		::CloseHandle(h);
	::CloseHandle(mtx);
}

HELPER(mutex_probe)
{
	HANDLE h = ::CreateMutexW(NULL, FALSE, argv[0]);
	DWORD e = ::GetLastError();
	printf("%s err=%lu\n", h ? "ok" : "fail", e);
	return 0;
}

TEST(sys_Mutex)
{
	// Emule.cpp:761 - the single-instance mutex. eMule checks
	// GetLastError() == ERROR_ALREADY_EXISTS after CreateMutex.
	const wchar_t *name = L"EMULE-{4EADC6FC-516F-4b7c-9066-97D893649570}-symrepro";
	::SetLastError(0xBEEF);
	HANDLE h1 = ::CreateMutexW(NULL, FALSE, name);
	DWORD e1 = ::GetLastError();
	HANDLE h2 = ::CreateMutexW(NULL, FALSE, name);
	DWORD e2 = ::GetLastError();
	out("CreateMutexW", "first", "%s err=%lu", h1 ? "ok" : "fail", e1);
	out("CreateMutexW", "second", "%s err=%lu same=%d", h2 ? "ok" : "fail", e2, h1 == h2);
	std::string o;
	RunHelper(std::wstring(L"mutex_probe ") + name, &o);
	out("CreateMutexW", "otherprocess", "%s", o.c_str());
	::CloseHandle(h2);
	::CloseHandle(h1);
	RunHelper(std::wstring(L"mutex_probe ") + name, &o);
	out("CreateMutexW", "otherprocess.afterclose", "%s", o.c_str());
	for (const wchar_t *n : {L"Local\\symrepro_m", L"Global\\symrepro_m", L"bad\\name", L"symrepro_ev_clash", L""}) {
		HANDLE ev = NULL;
		if (!wcscmp(n, L"symrepro_ev_clash"))
			ev = ::CreateEventW(NULL, FALSE, FALSE, n);
		::SetLastError(0xBEEF);
		HANDLE h = ::CreateMutexW(NULL, TRUE, n);
		DWORD e = ::GetLastError();
		out("CreateMutexW", Q(n).c_str(), "%s err=%lu", h ? "ok" : "fail", e);
		if (h) {
			out("CreateMutexW", (Q(n) + ".owned.release").c_str(), "%s", B(::ReleaseMutex(h)).c_str());
			::CloseHandle(h);
		}
		if (ev)
			::CloseHandle(ev);
	}
}

TEST(sys_DuplicateHandle)
{
	// UPnPImplMiniLib.cpp:404 - duplicate a thread handle with only
	// SYNCHRONIZE access, then wait on it.
	HANDLE th = ::CreateThread(NULL, 0, [](LPVOID) -> DWORD { ::Sleep(200); return 9; }, NULL, 0, NULL);
	HANDLE dup = NULL;
	BOOL r = ::DuplicateHandle(::GetCurrentProcess(), th, ::GetCurrentProcess(), &dup, SYNCHRONIZE, FALSE, 0);
	out("DuplicateHandle", "thread.synchronize", "%s", B(r).c_str());
	out("DuplicateHandle", "wait.timeout0", "%lu", ::WaitForSingleObject(dup, 0));
	out("DuplicateHandle", "wait", "%lu", ::WaitForSingleObject(dup, 5000));
	DWORD code = 0;
	::SetLastError(0);
	r = ::GetExitCodeThread(dup, &code);
	out("DuplicateHandle", "exitcode.withoutright", "%s", B(r).c_str());
	::SetLastError(0);
	r = ::TerminateThread(dup, 0);
	out("DuplicateHandle", "terminate.withoutright", "%s", B(r).c_str());
	::CloseHandle(dup);
	r = ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
	out("DuplicateHandle", "pseudothread", "%s real=%d", B(r).c_str(), dup != ::GetCurrentThread());
	::CloseHandle(dup);
	::SetLastError(0);
	r = ::DuplicateHandle(::GetCurrentProcess(), (HANDLE)(ULONG_PTR)0x1234, ::GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
	out("DuplicateHandle", "invalid", "%s", B(r).c_str());
	HANDLE ev = ::CreateEventW(NULL, FALSE, FALSE, NULL);
	r = ::DuplicateHandle(::GetCurrentProcess(), ev, ::GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
	::SetLastError(0);
	BOOL c = ::CloseHandle(ev);
	out("DuplicateHandle", "closesource", "%s source_close_again=%s", B(r).c_str(), B(c).c_str());
	::CloseHandle(dup);
	::CloseHandle(th);
	::SetLastError(0);
	out("CloseHandle", "invalid", "%s", B(::CloseHandle((HANDLE)(ULONG_PTR)0x1234)).c_str());
	::SetLastError(0);
	out("CloseHandle", "null", "%s", B(::CloseHandle(NULL)).c_str());
	::SetLastError(0);
	out("CloseHandle", "invalid_handle_value", "%s", B(::CloseHandle(INVALID_HANDLE_VALUE)).c_str());
}

TEST(sys_CriticalSection)
{
	static CRITICAL_SECTION cs;
	::InitializeCriticalSection(&cs);
	::EnterCriticalSection(&cs);
	::EnterCriticalSection(&cs);	// recursion
	out("EnterCriticalSection", "recursion", "count=%ld owner_is_self=%d", cs.RecursionCount,
		(DWORD)(ULONG_PTR)cs.OwningThread == ::GetCurrentThreadId());
	HANDLE th = ::CreateThread(NULL, 0, [](LPVOID) -> DWORD { return ::TryEnterCriticalSection(&cs); }, NULL, 0, NULL);
	::WaitForSingleObject(th, 5000);
	DWORD code = 9;
	::GetExitCodeThread(th, &code);
	::CloseHandle(th);
	out("EnterCriticalSection", "otherthread.try", "%lu", code);
	::LeaveCriticalSection(&cs);
	::LeaveCriticalSection(&cs);
	out("LeaveCriticalSection", "released", "count=%ld", cs.RecursionCount);
	// Contention: two threads incrementing under the lock.
	static volatile LONG counter = 0;
	auto worker = [](LPVOID) -> DWORD {
		for (int i = 0; i < 100000; ++i) {
			::EnterCriticalSection(&cs);
			counter = counter + 1;
			::LeaveCriticalSection(&cs);
		}
		return 0;
	};
	HANDLE t[4];
	for (HANDLE &h : t)
		h = ::CreateThread(NULL, 0, worker, NULL, 0, NULL);
	::WaitForMultipleObjects(4, t, TRUE, 20000);
	for (HANDLE h : t)
		::CloseHandle(h);
	out("EnterCriticalSection", "contention", "%ld", counter);
	::DeleteCriticalSection(&cs);
	out("DeleteCriticalSection", "done", "ok");
}

TEST(sys_Global)
{
	// OtherFunctions.cpp clipboard helpers: GlobalAlloc(GHND | GMEM_SHARE).
	HGLOBAL h = ::GlobalAlloc(GHND | GMEM_SHARE, 10);
	out("GlobalAlloc", "ghnd", "%s size=%Iu flags=%x", h ? "ok" : "fail", ::GlobalSize(h), ::GlobalFlags(h));
	BYTE *p = (BYTE*)::GlobalLock(h);
	out("GlobalLock", "first", "%s zeroed=%d movable=%d flags=%x", p ? "ok" : "fail", p && !p[0] && !p[9], (void*)p != (void*)h, ::GlobalFlags(h));
	BYTE *p2 = (BYTE*)::GlobalLock(h);
	out("GlobalLock", "second", "same=%d flags=%x", p2 == p, ::GlobalFlags(h));
	::SetLastError(0xBEEF);
	BOOL u = ::GlobalUnlock(h);
	out("GlobalUnlock", "1of2", "r=%d err=%lu", u, ::GetLastError());
	::SetLastError(0xBEEF);
	u = ::GlobalUnlock(h);
	out("GlobalUnlock", "2of2", "r=%d err=%lu", u, ::GetLastError());
	::SetLastError(0xBEEF);
	u = ::GlobalUnlock(h);
	out("GlobalUnlock", "extra", "r=%d err=%lu", u, ::GetLastError());
	HGLOBAL f = ::GlobalFree(h);
	out("GlobalFree", "moveable", "%s", f ? "nonnull" : "NULL");
	HGLOBAL fx = ::GlobalAlloc(GMEM_FIXED, 16);
	void *pf = ::GlobalLock(fx);
	out("GlobalLock", "fixed", "same=%d", pf == (void*)fx);
	::SetLastError(0xBEEF);
	u = ::GlobalUnlock(fx);
	out("GlobalUnlock", "fixed", "r=%d err=%lu", u, ::GetLastError());
	out("GlobalFree", "fixed", "%s", ::GlobalFree(fx) ? "nonnull" : "NULL");
	::SetLastError(0);
	void *pn = ::GlobalLock(NULL);
	out("GlobalLock", "null", "%s err=%lu", pn ? "nonnull" : "NULL", ::GetLastError());
	h = ::GlobalAlloc(GHND, 0);
	p = (BYTE*)::GlobalLock(h);
	out("GlobalAlloc", "zero", "%s lock=%s size=%Iu", h ? "ok" : "fail", p ? "nonnull" : "NULL", ::GlobalSize(h));
	if (p)
		::GlobalUnlock(h);
	::GlobalFree(h);
	h = ::GlobalAlloc(GHND, 8);
	::GlobalLock(h);
	f = ::GlobalFree(h);
	out("GlobalFree", "whilelocked", "%s", f ? "nonnull" : "NULL");
}

// ---------------------------------------------------------------------------
// Modules and resources

TEST(sys_LoadLibrary)
{
	::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	struct { const char *n; const wchar_t *p; } c[] = {
		{"dbghelp", L"DBGHELP.DLL"}, {"missing", L"nonexistent_symrepro.dll"}, {"missing.path", L"C:\\nope\\x.dll"},
		{"self", NULL}, {"kernel32", L"kernel32"}, {"wininet", L"wininet.dll"}, {"riched20", L"riched20.dll"}, {"msftedit", L"msftedit.dll"}};
	for (auto &x : c) {
		std::wstring p = x.p ? x.p : SelfPath();
		::SetLastError(0);
		HMODULE h = ::LoadLibraryW(p.c_str());
		DWORD e = ::GetLastError();
		out("LoadLibraryW", x.n, "%s err=%lu", h ? "ok" : "NULL", h ? 0 : e);
		if (h)
			out("FreeLibrary", x.n, "%s", B(::FreeLibrary(h)).c_str());
	}
	WriteWholeFile(TDir() + L"notpe.dll", "this is not a PE file at all, just text", 39);
	::SetLastError(0);
	HMODULE h = ::LoadLibraryW((TDir() + L"notpe.dll").c_str());
	out("LoadLibraryW", "notpe", "%s err=%lu", h ? "ok" : "NULL", ::GetLastError());
	::SetLastError(0);
	h = ::LoadLibraryW(TDir().c_str());
	out("LoadLibraryW", "directory", "%s err=%lu", h ? "ok" : "NULL", ::GetLastError());
	// The resource-only language DLL built next to the exe (lang\*.dll in eMule).
	std::wstring lang = SelfPath();
	lang = lang.substr(0, lang.rfind(L'\\') + 1) + L"srlang.dll";
	h = ::LoadLibraryW(lang.c_str());
	out("LoadLibraryW", "langdll", "%s", h ? "ok" : Fmt("NULL err=%lu", ::GetLastError()).c_str());
	if (h) {
		wchar_t buf[64] = L"";
		int n = ::LoadStringW(h, 1, buf, 64);
		out("LoadLibraryW", "langdll.string", "n=%d %s", n, Q(buf).c_str());
		HRSRC r = ::FindResourceW(h, MAKEINTRESOURCEW(1), RT_RCDATA);
		out("FindResourceW", "langdll.rcdata", "%s size=%lu", r ? "ok" : "NULL", r ? ::SizeofResource(h, r) : 0);
		out("FreeLibrary", "langdll", "%s", B(::FreeLibrary(h)).c_str());
		::SetLastError(0);
		out("FreeLibrary", "langdll.again", "%s", B(::FreeLibrary(h)).c_str());
	}
	::SetLastError(0);
	out("FreeLibrary", "null", "%s", B(::FreeLibrary(NULL)).c_str());
	HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
	for (const char *fn : {"GetTickCount", "gettickcount", "NoSuchFunction_sr", "GetProcessDEPPolicy", "SetProcessDEPPolicy",
			"HeapSetInformation", "GetTickCount64", ""}) {
		::SetLastError(0);
		FARPROC p = ::GetProcAddress(k, fn);
		out("GetProcAddress", Fmt("kernel32.%s", fn).c_str(), "%s err=%lu", p ? "ok" : "NULL", p ? 0 : ::GetLastError());
	}
	HMODULE ws = ::GetModuleHandleW(L"ws2_32.dll");
	FARPROC byOrd = ::GetProcAddress(ws, (LPCSTR)MAKEINTRESOURCEA(3));	// closesocket
	out("GetProcAddress", "ws2_32.ordinal3", "%s match=%d", byOrd ? "ok" : "NULL", byOrd == ::GetProcAddress(ws, "closesocket"));
	::SetLastError(0);
	FARPROC p = ::GetProcAddress(NULL, "wmain");
	out("GetProcAddress", "selfnull.wmain", "%s err=%lu", p ? "ok" : "NULL", p ? 0 : ::GetLastError());
}

TEST(sys_Resources)
{
	HMODULE self = ::GetModuleHandleW(NULL);
	// kademlia/io/DataIO.cpp: custom resource type looked up by integer id.
	HRSRC r = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_MAP), L"WIDECHARMAP");
	out("FindResourceW", "custom", "%s", r ? "ok" : Fmt("NULL err=%lu", ::GetLastError()).c_str());
	DWORD sz = ::SizeofResource(self, r);
	HGLOBAL g = ::LoadResource(self, r);
	const BYTE *p = (const BYTE*)::LockResource(g);
	DWORD sum = 0;
	for (DWORD i = 0; p && i < sz; ++i)
		sum = sum * 31 + p[i];
	out("SizeofResource", "custom", "%lu", sz);
	out("LoadResource", "custom", "%s", g ? "ok" : "NULL");
	out("LockResource", "custom", "%s sum=%08lx", p ? "ok" : "NULL", sum);
	out("LockResource", "custom.readonly_page", "%s", [p]() {
		MEMORY_BASIC_INFORMATION mbi;
		::VirtualQuery(p, &mbi, sizeof mbi);
		return Fmt("protect=%lx", mbi.Protect);
	}().c_str());
	// MiniMule.cpp: "GIF" type.
	r = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_GIF), L"GIF");
	out("FindResourceW", "gif", "%s size=%lu", r ? "ok" : "NULL", r ? ::SizeofResource(self, r) : 0);
	r = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_GIF), L"gif");
	out("FindResourceW", "gif.lowercase_type", "%s", r ? "ok" : "NULL");
	r = ::FindResourceW(self, L"NAMED_ITEM", RT_RCDATA);
	out("FindResourceW", "named", "%s size=%lu", r ? "ok" : "NULL", r ? ::SizeofResource(self, r) : 0);
	r = ::FindResourceW(self, L"named_item", RT_RCDATA);
	out("FindResourceW", "named.lowercase", "%s", r ? "ok" : "NULL");
	::SetLastError(0);
	r = ::FindResourceW(self, MAKEINTRESOURCEW(999), L"WIDECHARMAP");
	out("FindResourceW", "missingname", "%s err=%lu", r ? "ok" : "NULL", ::GetLastError());
	::SetLastError(0);
	r = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_MAP), L"NOSUCHTYPE");
	out("FindResourceW", "missingtype", "%s err=%lu", r ? "ok" : "NULL", ::GetLastError());
	r = ::FindResourceW(NULL, MAKEINTRESOURCEW(IDR_MAP), L"WIDECHARMAP");
	out("FindResourceW", "nullmodule", "%s", r ? "ok" : "NULL");
	::SetLastError(0);
	sz = ::SizeofResource(self, NULL);
	out("SizeofResource", "null", "%lu err=%lu", sz, ::GetLastError());
	::SetLastError(0);
	g = ::LoadResource(self, NULL);
	out("LoadResource", "null", "%s err=%lu", g ? "nonnull" : "NULL", ::GetLastError());
	out("LockResource", "null", "%s", ::LockResource(NULL) ? "nonnull" : "NULL");
	wchar_t buf[64];
	int n = ::LoadStringW(self, IDS_UNICODE, buf, 64);
	out("LoadStringW", "unicode", "n=%d %s", n, Q(buf).c_str());
}

TEST(sys_Version)
{
	// OtherFunctions.cpp:3400 - file version of a DLL/EXE.
	std::wstring self = SelfPath();
	DWORD unused = 77;
	DWORD sz = ::GetFileVersionInfoSizeW(self.c_str(), &unused);
	out("GetFileVersionInfoSizeW", "self", "nonzero=%d handle=%lu", sz > 0, unused);
	std::vector<BYTE> blk(sz + 16);
	BOOL r = ::GetFileVersionInfoW(self.c_str(), 0, sz, blk.data());
	out("GetFileVersionInfoW", "self", "%s", B(r).c_str());
	VS_FIXEDFILEINFO *ffi = NULL;
	UINT len = 0;
	r = ::VerQueryValueW(blk.data(), L"\\", (LPVOID*)&ffi, &len);
	out("VerQueryValueW", "root", "%s len=%u sig=%08lx file=%lu.%lu.%lu.%lu prod=%lu.%lu.%lu.%lu", B(r).c_str(), len,
		ffi ? ffi->dwSignature : 0, ffi ? HIWORD(ffi->dwFileVersionMS) : 0, ffi ? LOWORD(ffi->dwFileVersionMS) : 0,
		ffi ? HIWORD(ffi->dwFileVersionLS) : 0, ffi ? LOWORD(ffi->dwFileVersionLS) : 0,
		ffi ? HIWORD(ffi->dwProductVersionMS) : 0, ffi ? LOWORD(ffi->dwProductVersionMS) : 0,
		ffi ? HIWORD(ffi->dwProductVersionLS) : 0, ffi ? LOWORD(ffi->dwProductVersionLS) : 0);
	wchar_t *s = NULL;
	r = ::VerQueryValueW(blk.data(), L"\\StringFileInfo\\040904b0\\FileDescription", (LPVOID*)&s, &len);
	out("VerQueryValueW", "string", "%s len=%u %s", B(r).c_str(), len, r ? Q(s).c_str() : "");
	r = ::VerQueryValueW(blk.data(), L"\\StringFileInfo\\040904B0\\FILEDESCRIPTION", (LPVOID*)&s, &len);
	out("VerQueryValueW", "string.case", "%s", B(r).c_str());
	DWORD *tr = NULL;
	r = ::VerQueryValueW(blk.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &len);
	out("VerQueryValueW", "translation", "%s len=%u %08lx", B(r).c_str(), len, tr ? *tr : 0);
	::SetLastError(0);
	r = ::VerQueryValueW(blk.data(), L"\\StringFileInfo\\040904b0\\NoSuchValue", (LPVOID*)&s, &len);
	out("VerQueryValueW", "missing", "%s len=%u", B(r).c_str(), len);
	// Small buffer to GetFileVersionInfo: truncated but usable?
	std::vector<BYTE> small(64);
	r = ::GetFileVersionInfoW(self.c_str(), 0, (DWORD)small.size(), small.data());
	out("GetFileVersionInfoW", "small", "%s", B(r).c_str());
	// A DLL from the system: present on both, contents differ (~).
	wchar_t sys[MAX_PATH];
	::GetSystemDirectoryW(sys, MAX_PATH);
	std::wstring k = std::wstring(sys) + L"\\kernel32.dll";
	sz = ::GetFileVersionInfoSizeW(k.c_str(), &unused);
	out("GetFileVersionInfoSizeW", "kernel32", "nonzero=%d", sz > 0);
	WriteWholeFile(TDir() + L"plain.txt", "x", 1);
	::SetLastError(0);
	sz = ::GetFileVersionInfoSizeW((TDir() + L"plain.txt").c_str(), &unused);
	out("GetFileVersionInfoSizeW", "notpe", "%lu err=%lu", sz, ::GetLastError());
	::SetLastError(0);
	sz = ::GetFileVersionInfoSizeW((TDir() + L"missing.dll").c_str(), &unused);
	out("GetFileVersionInfoSizeW", "missing", "%lu err=%lu", sz, ::GetLastError());
	std::wstring lang = SelfPath();
	lang = lang.substr(0, lang.rfind(L'\\') + 1) + L"srlang.dll";
	::SetLastError(0);
	sz = ::GetFileVersionInfoSizeW(lang.c_str(), &unused);
	out("GetFileVersionInfoSizeW", "noversionresource", "%lu err=%lu", sz, ::GetLastError());
}

// ---------------------------------------------------------------------------
// Exceptions

static LONG s_vehCount;
static DWORD s_vehCode;
static ULONG_PTR s_vehInfo0;
static LONG CALLBACK Veh(PEXCEPTION_POINTERS ep)
{
	++s_vehCount;
	s_vehCode = ep->ExceptionRecord->ExceptionCode;
	s_vehInfo0 = ep->ExceptionRecord->NumberParameters ? ep->ExceptionRecord->ExceptionInformation[0] : 99;
	return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD s_filtCode, s_filtFlags, s_filtParams;
static int Filter(PEXCEPTION_POINTERS ep, int action)
{
	s_filtCode = ep->ExceptionRecord->ExceptionCode;
	s_filtFlags = ep->ExceptionRecord->ExceptionFlags;
	s_filtParams = ep->ExceptionRecord->NumberParameters;
	return action;
}

// A C++ throw/catch cannot share a function with __try.
static void CppThrow()
{
	try {
		throw 5;
	} catch (int) {
	}
}

TEST(sys_Exceptions)
{
	// kademlia/utils/ThreadName.cpp: RaiseException(MS_VC_EXCEPTION) inside
	// __try / __except(EXCEPTION_CONTINUE_EXECUTION).
	ULONG_PTR info[4] = {0x1000, (ULONG_PTR)"name", (ULONG_PTR)-1, 0};
	bool continued = false;
	__try {
		::RaiseException(0x406D1388, 0, 4, info);
		continued = true;
	} __except (Filter(GetExceptionInformation(), EXCEPTION_CONTINUE_EXECUTION)) {
	}
	out("RaiseException", "threadname.continue", "continued=%d code=%lx flags=%lx params=%lu", continued, s_filtCode, s_filtFlags, s_filtParams);
	__try {
		::RaiseException(0xE0001234, 0, 0, NULL);
	} __except (Filter(GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER)) {
	}
	out("RaiseException", "noparams", "code=%lx flags=%lx params=%lu", s_filtCode, s_filtFlags, s_filtParams);
	ULONG_PTR many[20] = {0};
	__try {
		::RaiseException(0xE0001235, 0, 20, many);
	} __except (Filter(GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER)) {
	}
	out("RaiseException", "toomanyparams", "code=%lx params=%lu", s_filtCode, s_filtParams);
	__try {
		::RaiseException(0xE0001236, 0xFFFFFFFF, 0, NULL);
	} __except (Filter(GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER)) {
	}
	out("RaiseException", "allflags", "code=%lx flags=%lx", s_filtCode, s_filtFlags);
	// Continuing a non-continuable exception raises a new one.
	DWORD outer = 0;
	__try {
		__try {
			::RaiseException(0xE0001237, EXCEPTION_NONCONTINUABLE, 0, NULL);
		} __except (EXCEPTION_CONTINUE_EXECUTION) {
		}
	} __except (outer = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
	}
	out("RaiseException", "noncontinuable.continued", "outer=%lx", outer);

	// Emule.cpp:336 - AddVectoredExceptionHandler(1, DiagAVHandler): sees
	// every first-chance exception before any frame handler.
	PVOID vh = ::AddVectoredExceptionHandler(1, Veh);
	out("AddVectoredExceptionHandler", "add", "%s", vh ? "ok" : "NULL");
	s_vehCount = 0;
	__try {
		*(volatile int*)(ULONG_PTR)0x10 = 1;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	out("AddVectoredExceptionHandler", "av.write", "count=%ld code=%lx info0=%Iu", s_vehCount, s_vehCode, s_vehInfo0);
	s_vehCount = 0;
	__try {
		volatile int x = *(volatile int*)(ULONG_PTR)0x10;
		(void)x;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	out("AddVectoredExceptionHandler", "av.read", "count=%ld code=%lx info0=%Iu", s_vehCount, s_vehCode, s_vehInfo0);
	s_vehCount = 0;
	CppThrow();
	out("AddVectoredExceptionHandler", "cppthrow", "count=%ld code=%lx", s_vehCount, s_vehCode);
	s_vehCount = 0;
	__try {
		volatile int z = 0;
		volatile int q = 5 / z;
		(void)q;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	out("AddVectoredExceptionHandler", "intdivzero", "count=%ld code=%lx", s_vehCount, s_vehCode);
	s_vehCount = 0;
	::OutputDebugStringW(L"symrepro: OutputDebugString under a vectored handler\n");
	out("AddVectoredExceptionHandler", "outputdebugstring", "count=%ld code=%lx", s_vehCount, s_vehCount ? s_vehCode : 0);
	out("AddVectoredExceptionHandler", "remove", "%lu", ::RemoveVectoredExceptionHandler(vh));
	out("AddVectoredExceptionHandler", "remove.again", "%lu", ::RemoveVectoredExceptionHandler(vh));
}

HELPER(unhandled)
{
	// Mdump.cpp: SetUnhandledExceptionFilter(TopLevelFilter), whose filter
	// writes a minidump and returns EXCEPTION_EXECUTE_HANDLER.
	::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	LPTOP_LEVEL_EXCEPTION_FILTER prev = ::SetUnhandledExceptionFilter([](PEXCEPTION_POINTERS ep) -> LONG {
		printf("filter code=%lx\n", ep->ExceptionRecord->ExceptionCode);
		HMODULE h = ::LoadLibraryW(L"DBGHELP.DLL");
		typedef BOOL (WINAPI *MDW)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
		MDW pfn = h ? (MDW)::GetProcAddress(h, "MiniDumpWriteDump") : NULL;
		printf("MiniDumpWriteDump proc=%s\n", pfn ? "ok" : "NULL");
		if (pfn) {
			wchar_t path[MAX_PATH];
			::GetEnvironmentVariableW(L"SR_DUMP", path, MAX_PATH);
			HANDLE f = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			MINIDUMP_EXCEPTION_INFORMATION ei = {::GetCurrentThreadId(), ep, FALSE};
			BOOL ok = pfn(::GetCurrentProcess(), ::GetCurrentProcessId(), f, MiniDumpNormal, &ei, NULL, NULL);
			LARGE_INTEGER sz = {};
			::GetFileSizeEx(f, &sz);
			printf("MiniDumpWriteDump r=%d err=%lu size_nonzero=%d\n", ok, ok ? 0 : ::GetLastError(), sz.QuadPart > 0);
			::CloseHandle(f);
		}
		fflush(stdout);
		return EXCEPTION_EXECUTE_HANDLER;
	});
	printf("prev=%s\n", prev ? "nonnull" : "NULL");
	fflush(stdout);
	if (argc > 0 && !wcscmp(argv[0], L"raise"))
		::RaiseException(0xE0004321, 0, 0, NULL);
	*(volatile int*)(ULONG_PTR)0x10 = 1;
	printf("survived\n");
	return 0;
}

HELPER(exitprocess)
{
	// EmuleDlg.cpp:1586, Mdump.cpp:158 - ExitProcess with a thread still busy.
	::CreateThread(NULL, 0, [](LPVOID) -> DWORD { for (;;) ::Sleep(10); }, NULL, 0, NULL);
	printf("before\n");
	fflush(stdout);
	::ExitProcess(argc > 0 ? _wtoi(argv[0]) : 0);
}

TEST(sys_UnhandledAndExit)
{
	std::wstring dump = TDir() + L"crash.dmp";
	::SetEnvironmentVariableW(L"SR_DUMP", dump.c_str());
	std::string o;
	DWORD code = RunHelper(L"unhandled av", &o);
	out("SetUnhandledExceptionFilter", "av", "exit=%lx out=%s", code, QA(o.c_str()).c_str());
	code = RunHelper(L"unhandled raise", &o);
	out("SetUnhandledExceptionFilter", "raise", "exit=%lx out=%s", code, QA(o.c_str()).c_str());
	code = RunHelper(L"exitprocess 0", &o);
	out("ExitProcess", "0", "exit=%lu out=%s", code, QA(o.c_str()).c_str());
	code = RunHelper(L"exitprocess 7", &o);
	out("ExitProcess", "7", "exit=%lu out=%s", code, QA(o.c_str()).c_str());
}

// ---------------------------------------------------------------------------
// Process-wide settings

TEST(sys_ProcessSettings)
{
	// Emule.cpp:152-216.
	DWORD flags = 0xFFFF;
	BOOL perm = 7;
	::SetLastError(0);
	BOOL r = ::GetProcessDEPPolicy(::GetCurrentProcess(), &flags, &perm);
	DWORD e = ::GetLastError();
	out("GetProcessDEPPolicy", "self", "r=%d err=%lu flags=%lx perm=%d", r, r ? 0 : e, flags, perm);
	::SetLastError(0);
	r = ::SetProcessDEPPolicy(PROCESS_DEP_ENABLE | PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION);
	e = ::GetLastError();
	out("SetProcessDEPPolicy", "enable", "r=%d err=%lu", r, r ? 0 : e);
	flags = 0xFFFF;
	r = ::GetProcessDEPPolicy(::GetCurrentProcess(), &flags, &perm);
	out("GetProcessDEPPolicy", "after", "r=%d flags=%lx perm=%d", r, flags, perm);
	::SetLastError(0);
	r = ::SetProcessDEPPolicy(0);
	e = ::GetLastError();
	out("SetProcessDEPPolicy", "disable.afterpermanent", "r=%d err=%lu", r, r ? 0 : e);
	r = ::HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
	out("HeapSetInformation", "terminate_on_corruption", "%s", B(r).c_str());
	ULONG lfh = 2;
	r = ::HeapSetInformation(::GetProcessHeap(), HeapCompatibilityInformation, &lfh, sizeof lfh);
	out("HeapSetInformation", "lfh", "%s", B(r).c_str());
	r = ::HeapSetInformation(NULL, (HEAP_INFORMATION_CLASS)99, NULL, 0);
	out("HeapSetInformation", "badclass", "%s", B(r).c_str());
	// Emule.cpp:1718 - standby prevention.
	EXECUTION_STATE s1 = ::SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_CONTINUOUS);
	EXECUTION_STATE s2 = ::SetThreadExecutionState(ES_CONTINUOUS);
	EXECUTION_STATE s3 = ::SetThreadExecutionState(ES_CONTINUOUS);
	EXECUTION_STATE s4 = ::SetThreadExecutionState(ES_SYSTEM_REQUIRED);
	out("SetThreadExecutionState", "sequence", "%lx %lx %lx %lx", s1, s2, s3, s4);
	EXECUTION_STATE s5 = ::SetThreadExecutionState(0x12345678);
	out("SetThreadExecutionState", "invalid", "%lx", s5);
	// Emule.cpp:602.
	r = ::SetConsoleCtrlHandler([](DWORD) -> BOOL { return TRUE; }, TRUE);
	out("SetConsoleCtrlHandler", "add", "%s", B(r).c_str());
	r = ::SetConsoleCtrlHandler(NULL, TRUE);
	out("SetConsoleCtrlHandler", "ignore", "%s", B(r).c_str());
	r = ::SetConsoleCtrlHandler([](DWORD) -> BOOL { return FALSE; }, FALSE);
	out("SetConsoleCtrlHandler", "removemissing", "%s", B(r).c_str());
	// GetProfileInt: win.ini lookups through MFC's CWinApp::GetProfileInt.
	out("GetProfileIntW", "missing", "%u", ::GetProfileIntW(L"symrepro", L"nokey", 5));
	::WriteProfileStringW(L"symrepro", L"val", L"  12abc");
	out("GetProfileIntW", "leadingspace.trailingjunk", "%u", ::GetProfileIntW(L"symrepro", L"val", 5));
	::WriteProfileStringW(L"symrepro", L"val", L"-3");
	out("GetProfileIntW", "negative", "%d", (int)::GetProfileIntW(L"symrepro", L"val", 5));
	::WriteProfileStringW(L"symrepro", L"val", L"0x10");
	out("GetProfileIntW", "hex", "%u", ::GetProfileIntW(L"symrepro", L"val", 5));
	::WriteProfileStringW(L"symrepro", L"val", L"");
	out("GetProfileIntW", "empty", "%u", ::GetProfileIntW(L"symrepro", L"val", 5));
	::WriteProfileStringW(L"symrepro", NULL, NULL);
	out("GetProfileIntW", "deleted", "%u", ::GetProfileIntW(L"symrepro", L"val", 5));
}

TEST(sys_Privileges)
{
	// EmuleDlg.cpp:3251 - SE_SHUTDOWN privilege before a system shutdown.
	HANDLE tok = NULL;
	BOOL r = ::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok);
	out("OpenProcessToken", "self", "%s", B(r).c_str());
	TOKEN_PRIVILEGES tp = {1};
	r = ::LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
	out("LookupPrivilegeValueW", "shutdown", "%s luid=%lu:%ld", B(r).c_str(), tp.Privileges[0].Luid.LowPart, tp.Privileges[0].Luid.HighPart);
	LUID l;
	r = ::LookupPrivilegeValueW(NULL, L"SeNoSuchPrivilege", &l);
	out("LookupPrivilegeValueW", "missing", "%s", B(r).c_str());
	r = ::LookupPrivilegeValueW(NULL, L"seshutdownprivilege", &l);
	out("LookupPrivilegeValueW", "lowercase", "%s luid=%lu", B(r).c_str(), l.LowPart);
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	::SetLastError(0xBEEF);
	r = ::AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, 0);
	DWORD e = ::GetLastError();
	out("AdjustTokenPrivileges", "shutdown", "r=%d", r);
	out("AdjustTokenPrivileges", "~shutdown.err", "%lu", e);
	::SetLastError(0);
	r = ::AdjustTokenPrivileges(NULL, FALSE, &tp, 0, NULL, 0);
	out("AdjustTokenPrivileges", "nulltoken", "%s", B(r).c_str());
	HANDLE ro = NULL;
	::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &ro);
	::SetLastError(0);
	r = ::AdjustTokenPrivileges(ro, FALSE, &tp, 0, NULL, 0);
	out("AdjustTokenPrivileges", "queryonlytoken", "%s", B(r).c_str());
	::CloseHandle(ro);
	::CloseHandle(tok);
}

// ---------------------------------------------------------------------------
// Time

TEST(sys_Time)
{
	DWORD t1 = ::GetTickCount(), m1 = ::timeGetTime();
	::Sleep(50);
	DWORD t2 = ::GetTickCount(), m2 = ::timeGetTime();
	out("GetTickCount", "advances", "%d", t2 - t1 >= 40 && t2 - t1 < 500);
	out("timeGetTime", "advances", "%d", m2 - m1 >= 40 && m2 - m1 < 500);
	out("timeGetTime", "~offset_from_tickcount_ms", "%ld", (long)(m1 - t1));
	LARGE_INTEGER f, c1, c2;
	BOOL r = ::QueryPerformanceFrequency(&f);
	out("QueryPerformanceFrequency", "ok", "%s positive=%d", B(r).c_str(), f.QuadPart > 0);
	out("QueryPerformanceFrequency", "~value", "%lld", f.QuadPart);
	::QueryPerformanceCounter(&c1);
	::Sleep(20);
	::QueryPerformanceCounter(&c2);
	double ms = 1000.0 * (c2.QuadPart - c1.QuadPart) / f.QuadPart;
	out("QueryPerformanceCounter", "consistent_with_sleep20", "%d", ms >= 15 && ms < 200);
	TIMECAPS tc = {0};
	MMRESULT mr = ::timeGetDevCaps(&tc, sizeof tc);
	out("timeGetDevCaps", "caps", "r=%u min=%u max=%u", mr, tc.wPeriodMin, tc.wPeriodMax);
	mr = ::timeGetDevCaps(&tc, 2);
	out("timeGetDevCaps", "smallsize", "r=%u", mr);
	mr = ::timeGetDevCaps(NULL, sizeof tc);
	out("timeGetDevCaps", "null", "r=%u", mr);
	out("timeBeginPeriod", "1", "%u", ::timeBeginPeriod(1));
	out("timeBeginPeriod", "0", "%u", ::timeBeginPeriod(0));
	out("timeBeginPeriod", "huge", "%u", ::timeBeginPeriod(1000000));
	out("timeEndPeriod", "1", "%u", ::timeEndPeriod(1));
	out("timeEndPeriod", "unmatched5", "%u", ::timeEndPeriod(5));
	out("timeEndPeriod", "0", "%u", ::timeEndPeriod(0));
	// Local time = UTC - bias, consistent with the time zone information.
	TIME_ZONE_INFORMATION tzi;
	DWORD id = ::GetTimeZoneInformation(&tzi);
	SYSTEMTIME lt, ut;
	::GetLocalTime(&lt);
	::GetSystemTime(&ut);
	FILETIME fl, fu;
	::SystemTimeToFileTime(&lt, &fl);
	::SystemTimeToFileTime(&ut, &fu);
	LONGLONG diffMin = (((LONGLONG)fl.dwHighDateTime << 32 | fl.dwLowDateTime) - ((LONGLONG)fu.dwHighDateTime << 32 | fu.dwLowDateTime)) / 600000000LL;
	LONG bias = tzi.Bias + (id == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : id == TIME_ZONE_ID_STANDARD ? tzi.StandardBias : 0);
	out("GetTimeZoneInformation", "consistent_with_localtime", "%d", diffMin == -bias || diffMin == -bias - 1 || diffMin == -bias + 1);
	out("GetTimeZoneInformation", "~id", "%lu bias=%ld std=%s dst=%s", id, tzi.Bias, Q(tzi.StandardName).c_str(), Q(tzi.DaylightName).c_str());
	out("GetLocalTime", "fields_valid", "%d", lt.wYear >= 2024 && lt.wMonth >= 1 && lt.wMonth <= 12 && lt.wDay >= 1 && lt.wDayOfWeek <= 6);
	out("GetSystemTime", "fields_valid", "%d", ut.wYear >= 2024 && ut.wMilliseconds < 1000 && ut.wDayOfWeek <= 6);
	// Sleep(0) yields and returns; Sleep(1) cost (eMule's Pinger and throttler use short sleeps).
	DWORD s0 = ::timeGetTime();
	for (int i = 0; i < 100; ++i)
		::Sleep(0);
	out("Sleep", "zero.x100.under100ms", "%d", ::timeGetTime() - s0 < 100);
	::timeBeginPeriod(1);
	s0 = ::timeGetTime();
	for (int i = 0; i < 20; ++i)
		::Sleep(1);
	DWORD d = ::timeGetTime() - s0;
	::timeEndPeriod(1);
	out("Sleep", "one.x20.period1", "atleast20=%d under100=%d", d >= 19, d < 100);
	out("Sleep", "~one.x20.period1.ms", "%lu", d);
}

TEST(sys_DebugAndBeep)
{
	::OutputDebugStringA("symrepro A\n");
	::OutputDebugStringW(L"symrepro W \u00e9\n");
	::OutputDebugStringA(NULL);
	out("OutputDebugStringA", "returns", "ok");
	out("OutputDebugStringW", "returns", "ok");
	::SetLastError(0);
	BOOL r = ::Beep(800, 50);
	out("Beep", "~result", "%s", B(r).c_str());
}

TEST(sys_wmvcore)
{
	// MediaInfo.cpp:1946 - wmvcore.dll is optional (media feature pack).
	HMODULE h = ::LoadLibraryW(L"wmvcore.dll");
	out("LoadLibraryW", "~wmvcore", "%s", h ? "ok" : Fmt("NULL err=%lu", ::GetLastError()).c_str());
	if (h) {
		typedef HRESULT (STDMETHODCALLTYPE *Create)(IUnknown*, DWORD, IUnknown**);
		Create sr = (Create)::GetProcAddress(h, "WMCreateSyncReader");
		Create ed = (Create)::GetProcAddress(h, "WMCreateEditor");
		out("GetProcAddress", "~wmvcore.procs", "sync=%d editor=%d", sr != NULL, ed != NULL);
		::FreeLibrary(h);
	}
}

TEST(sys_PlaySound)
{
	// EmuleDlg.cpp:2119-2123, IrcMain.cpp:228 - notification sounds.
	// Whether a sound device exists is the machine's business ('~'); what a
	// missing file or alias does is not.
	BOOL r = ::PlaySoundW((TDir() + L"missing.wav").c_str(), NULL, SND_FILENAME | SND_NODEFAULT | SND_NOSTOP | SND_NOWAIT | SND_ASYNC);
	out("PlaySoundW", "missing.nodefault", "%d", r);
	r = ::PlaySoundW(L"SymreproNoSuchEvent", NULL, SND_APPLICATION | SND_ASYNC | SND_NODEFAULT | SND_NOWAIT);
	out("PlaySoundW", "noalias.nodefault", "%d", r);
	// A valid 8 kHz mono 16-bit WAV, 0.05 s of silence.
	const DWORD samples = 400, dataBytes = samples * 2;
	std::string wav = "RIFF";
	DWORD v = 36 + dataBytes;
	wav.append((char*)&v, 4);
	wav += "WAVEfmt ";
	v = 16;
	wav.append((char*)&v, 4);
	WORD fmt[] = {1, 1};
	wav.append((char*)fmt, 4);
	DWORD rate = 8000, bps = 16000;
	wav.append((char*)&rate, 4);
	wav.append((char*)&bps, 4);
	WORD align[] = {2, 16};
	wav.append((char*)align, 4);
	wav += "data";
	wav.append((char*)&dataBytes, 4);
	wav.append(dataBytes, '\0');
	WriteWholeFile(TDir() + L"ok.wav", wav.data(), (DWORD)wav.size());
	r = ::PlaySoundW((TDir() + L"ok.wav").c_str(), NULL, SND_FILENAME | SND_NOSTOP | SND_NOWAIT | SND_ASYNC);
	out("PlaySoundW", "~valid.async", "%d", r);
	::Sleep(200);
	r = ::PlaySoundW((TDir() + L"ok.wav").c_str(), NULL, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
	out("PlaySoundW", "~valid.sync", "%d", r);
	WriteWholeFile(TDir() + L"bad.wav", "RIFFxxxxWAVEjunk", 16);
	r = ::PlaySoundW((TDir() + L"bad.wav").c_str(), NULL, SND_FILENAME | SND_NODEFAULT | SND_SYNC);
	out("PlaySoundW", "corrupt.nodefault", "%d", r);
	r = ::PlaySoundW(NULL, NULL, 0);
	out("PlaySoundW", "stop", "%d", r);
}
