// COM, OLE storage, the shell - OLE32, SHELL32, URLMON.
#include "harness.h"
#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <urlmon.h>
#include <intshcut.h>
#include <stdio.h>

static std::string HR(HRESULT hr) { return Fmt("%08lx", (unsigned long)hr); }

static std::string Guid(const GUID &g)
{
	return Fmt("%08lx-%04x-%04x-%s", g.Data1, g.Data2, g.Data3, Hex(g.Data4, 8).c_str());
}

TEST(ole_CoInitialize)
{
	HRESULT a = ::CoInitialize(NULL);
	HRESULT b = ::CoInitialize(NULL);
	out("CoInitialize", "first.second", "%s %s", HR(a).c_str(), HR(b).c_str());
	::CoUninitialize();
	::CoUninitialize();
	out("CoUninitialize", "balanced", "ok");
	::CoUninitialize();	// unbalanced: must be harmless
	out("CoUninitialize", "extra", "ok");
	HANDLE th = ::CreateThread(NULL, 0, [](LPVOID) -> DWORD {
		HRESULT m = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
		HRESULT s = ::CoInitialize(NULL);
		out("CoInitialize", "after.mta", "%s %s", HR(m).c_str(), HR(s).c_str());
		::CoUninitialize();
		return 0;
	}, NULL, 0, NULL);
	::WaitForSingleObject(th, 10000);
	::CloseHandle(th);
	GUID g1, g2;
	HRESULT h1 = ::CoCreateGuid(&g1), h2 = ::CoCreateGuid(&g2);
	out("CoCreateGuid", "two", "%s %s distinct=%d version=%x variant=%x", HR(h1).c_str(), HR(h2).c_str(), !IsEqualGUID(g1, g2),
		(g1.Data3 >> 12) & 0xF, g1.Data4[0] >> 6);
	out("CoCreateGuid", "null", "%s", HR(::CoCreateGuid(NULL)).c_str());
	void *p = ::CoTaskMemAlloc(0);
	out("CoTaskMemAlloc", "zero", "%s", p ? "nonnull" : "NULL");
	::CoTaskMemFree(p);
	p = ::CoTaskMemAlloc(100);
	out("CoTaskMemAlloc", "100", "%s", p ? "nonnull" : "NULL");
	::CoTaskMemFree(p);
	::CoTaskMemFree(NULL);
	out("CoTaskMemFree", "null", "ok");
}

TEST(ole_CoCreateInstance)
{
	// Every class eMule instantiates, through the interface it asks for.
	::CoInitialize(NULL);
	struct { const char *n; CLSID clsid; IID iid; bool optional; } c[] = {
		{"ShellLink", CLSID_ShellLink, IID_IShellLinkW, false},
		{"DragDropHelper", CLSID_DragDropHelper, IID_IDropTargetHelper, false},
		{"TaskbarList", CLSID_TaskbarList, IID_ITaskbarList3, false},
		{"AutoComplete", CLSID_AutoComplete, IID_IAutoComplete2, false},
		{"InternetShortcut", CLSID_InternetShortcut, IID_IUniformResourceLocatorW, false},
		{"PersistentZoneIdentifier", CLSID_PersistentZoneIdentifier, __uuidof(IZoneIdentifier), false},
		{"HTMLDocument", {0x25336920, 0x03F9, 0x11CF, {0x8F, 0xD0, 0x00, 0xAA, 0x00, 0x68, 0x6F, 0x13}}, IID_IUnknown, true},
		{"SpVoice", {0x96749377, 0x3391, 0x11D2, {0x9E, 0xE3, 0x00, 0xC0, 0x4F, 0x79, 0x73, 0x96}}, IID_IUnknown, true},
		{"MediaDet", {0x65BD0711, 0x24D2, 0x4FF7, {0x93, 0x24, 0xED, 0x2E, 0x5D, 0x3A, 0xBA, 0xFA}}, IID_IUnknown, true},
		{"NoSuchClass", {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}}, IID_IUnknown, false},
	};
	for (auto &x : c) {
		IUnknown *u = NULL;
		HRESULT hr = ::CoCreateInstance(x.clsid, NULL, CLSCTX_INPROC_SERVER, x.iid, (void**)&u);
		out("CoCreateInstance", (x.optional ? std::string("~") + x.n : std::string(x.n)).c_str(), "%s", HR(hr).c_str());
		if (u)
			u->Release();
	}
	ITaskbarList3 *tb = NULL;
	if (SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, (void**)&tb))) {
		out("CoCreateInstance", "TaskbarList.HrInit", "%s", HR(tb->HrInit()).c_str());
		tb->Release();
	}
	IUnknown *u = NULL;
	HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IClassFactory, (void**)&u);
	out("CoCreateInstance", "ShellLink.wrongiid", "%s", HR(hr).c_str());
	if (u)
		u->Release();
	::CoUninitialize();
	hr = ::CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&u);
	out("CoCreateInstance", "notinitialized", "%s", HR(hr).c_str());
	if (u)
		u->Release();
}

TEST(ole_ShellLinkAndZone)
{
	::CoInitialize(NULL);
	// A shortcut, as SharedFileList.cpp resolves them in shared folders.
	const std::wstring target = TDir() + L"target.txt", lnk = TDir() + L"link.lnk";
	WriteWholeFile(target, "t", 1);
	IShellLinkW *sl = NULL;
	HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl);
	if (sl) {
		sl->SetPath(target.c_str());
		IPersistFile *pf = NULL;
		sl->QueryInterface(IID_IPersistFile, (void**)&pf);
		hr = pf ? pf->Save(lnk.c_str(), TRUE) : E_NOINTERFACE;
		out("CoCreateInstance", "ShellLink.save", "%s", HR(hr).c_str());
		if (pf)
			pf->Release();
		sl->Release();
	}
	SHFILEINFOW fi = {};
	DWORD_PTR r = ::SHGetFileInfoW(lnk.c_str(), 0, &fi, sizeof fi, SHGFI_ATTRIBUTES);
	out("SHGetFileInfoW", "attributes.lnk", "r=%d link=%d", r != 0, (fi.dwAttributes & SFGAO_LINK) != 0);
	sl = NULL;
	::CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl);
	if (sl) {
		IPersistFile *pf = NULL;
		sl->QueryInterface(IID_IPersistFile, (void**)&pf);
		hr = pf ? pf->Load(lnk.c_str(), STGM_READ) : E_NOINTERFACE;
		wchar_t path[MAX_PATH] = L"";
		WIN32_FIND_DATAW fd;
		HRESULT g = sl->GetPath(path, MAX_PATH, &fd, SLGP_UNCPRIORITY);
		out("CoCreateInstance", "ShellLink.load", "%s getpath=%s %s", HR(hr).c_str(), HR(g).c_str(), QN(path).c_str());
		if (pf)
			pf->Release();
		sl->Release();
	}
	// PartFile.cpp:2731 - mark a completed download as coming from the Internet.
	const std::wstring dl = TDir() + L"download.bin";
	WriteWholeFile(dl, "d", 1);
	IZoneIdentifier *zi = NULL;
	hr = ::CoCreateInstance(CLSID_PersistentZoneIdentifier, NULL, CLSCTX_INPROC_SERVER, __uuidof(IZoneIdentifier), (void**)&zi);
	if (zi) {
		HRESULT s = zi->SetId(URLZONE_INTERNET);
		IPersistFile *pf = NULL;
		zi->QueryInterface(IID_IPersistFile, (void**)&pf);
		HRESULT sv = pf ? pf->Save(dl.c_str(), TRUE) : E_NOINTERFACE;
		out("CoCreateInstance", "ZoneIdentifier.save", "setid=%s save=%s", HR(s).c_str(), HR(sv).c_str());
		HANDLE h = ::CreateFileW((dl + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		char buf[256] = "";
		DWORD n = 0;
		if (h != INVALID_HANDLE_VALUE) {
			::ReadFile(h, buf, sizeof buf - 1, &n, NULL);
			::CloseHandle(h);
		}
		out("CoCreateInstance", "ZoneIdentifier.stream", "%s %s", h != INVALID_HANDLE_VALUE ? "present" : "absent", QA(buf, n).c_str());
		if (pf)
			pf->Release();
		zi->Release();
	}
	::CoUninitialize();
}

// Minimal IDataObject offering one bitmap as CF_BITMAP / TYMED_GDI, the way
// CHTRichEditCtrl hands a smiley to OleCreateStaticFromData.
class BitmapData : public IDataObject
{
	LONG m_ref = 1;
	HBITMAP m_bmp;
public:
	explicit BitmapData(HBITMAP b) : m_bmp(b) {}
	STDMETHODIMP QueryInterface(REFIID riid, void **pp) override
	{
		if (riid == IID_IUnknown || riid == IID_IDataObject) {
			*pp = this;
			AddRef();
			return S_OK;
		}
		*pp = NULL;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = ::InterlockedDecrement(&m_ref);
		if (!r)
			delete this;
		return r;
	}
	STDMETHODIMP GetData(FORMATETC *fe, STGMEDIUM *m) override
	{
		if (fe->cfFormat != CF_BITMAP || !(fe->tymed & TYMED_GDI))
			return DV_E_FORMATETC;
		m->tymed = TYMED_GDI;
		m->hBitmap = (HBITMAP)::OleDuplicateData(m_bmp, CF_BITMAP, 0);
		m->pUnkForRelease = NULL;
		return S_OK;
	}
	STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
	STDMETHODIMP QueryGetData(FORMATETC *fe) override { return fe->cfFormat == CF_BITMAP ? S_OK : DV_E_FORMATETC; }
	STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC *o) override { o->ptd = NULL; return E_NOTIMPL; }
	STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
	STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
	STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

TEST(ole_StaticFromData)
{
	// HTRichEditCtrl.cpp:1265-1289.
	::OleInitialize(NULL);
	ILockBytes *lb = NULL;
	HRESULT hr = ::CreateILockBytesOnHGlobal(NULL, TRUE, &lb);
	out("CreateILockBytesOnHGlobal", "new", "%s", HR(hr).c_str());
	IStorage *stg = NULL;
	hr = ::StgCreateDocfileOnILockBytes(lb, STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_READWRITE, 0, &stg);
	out("StgCreateDocfileOnILockBytes", "emuleflags", "%s", HR(hr).c_str());
	IStorage *bad = NULL;
	hr = ::StgCreateDocfileOnILockBytes(lb, STGM_READWRITE, 0, &bad);
	out("StgCreateDocfileOnILockBytes", "noshare", "%s", HR(hr).c_str());
	if (bad)
		bad->Release();
	HDC dc = ::GetDC(NULL);
	HBITMAP bmp = ::CreateCompatibleBitmap(dc, 16, 16);
	::ReleaseDC(NULL, dc);
	BitmapData *data = new BitmapData(bmp);
	FORMATETC fe = {CF_BITMAP, NULL, DVASPECT_CONTENT, -1, TYMED_GDI};
	IOleObject *obj = NULL;
	IStorage *sub = NULL;
	if (stg)
		stg->CreateStorage(L"smiley1", STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_READWRITE, 0, 0, &sub);
	hr = ::OleCreateStaticFromData(data, IID_IOleObject, OLERENDER_FORMAT, &fe, NULL, sub ? sub : stg, (void**)&obj);
	out("OleCreateStaticFromData", "bitmap", "%s", HR(hr).c_str());
	if (obj) {
		CLSID cls = {};
		HRESULT g = obj->GetUserClassID(&cls);
		out("OleCreateStaticFromData", "userclassid", "%s %s", HR(g).c_str(), Guid(cls).c_str());
		out("OleSetContainedObject", "true", "%s", HR(::OleSetContainedObject(obj, TRUE)).c_str());
		out("OleSetContainedObject", "false", "%s", HR(::OleSetContainedObject(obj, FALSE)).c_str());
		obj->Release();
	}
	HRESULT nh = E_FAIL;
	DWORD ex = Guard([&] { nh = ::OleSetContainedObject(NULL, TRUE); });
	out("OleSetContainedObject", "null", "%s%s", ex ? "" : HR(nh).c_str(), GuardStr(ex).c_str());
	FORMATETC wrong = {CF_DIB, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	obj = NULL;
	hr = ::OleCreateStaticFromData(data, IID_IOleObject, OLERENDER_FORMAT, &wrong, NULL, stg, (void**)&obj);
	out("OleCreateStaticFromData", "unavailableformat", "%s", HR(hr).c_str());
	if (obj)
		obj->Release();
	data->Release();
	::DeleteObject(bmp);
	if (sub)
		sub->Release();
	if (stg)
		stg->Release();
	if (lb) {
		STATSTG st = {};
		lb->Stat(&st, STATFLAG_NONAME);
		out("CreateILockBytesOnHGlobal", "~size_after", "%llu", st.cbSize.QuadPart);
		lb->Release();
	}
	::OleUninitialize();
}

TEST(ole_KnownFolders)
{
	// Preferences.cpp:2832 and FileInfoDialog.cpp.
	struct { const char *n; KNOWNFOLDERID id; const wchar_t *suffix; } c[] = {
		{"LocalAppData", FOLDERID_LocalAppData, L"\\AppData\\Local"},
		{"Downloads", FOLDERID_Downloads, L"\\Downloads"},
		{"PublicDownloads", FOLDERID_PublicDownloads, L"\\Users\\Public\\Downloads"},
		{"ProgramData", FOLDERID_ProgramData, L"C:\\ProgramData"},
		{"ProgramFiles", FOLDERID_ProgramFiles, sizeof(void*) == 8 ? L"C:\\Program Files" : L"C:\\Program Files (x86)"},
		{"RoamingAppData", FOLDERID_RoamingAppData, L"\\AppData\\Roaming"},
		{"Documents", FOLDERID_Documents, L"\\Documents"},
	};
	for (auto &x : c) {
		PWSTR p = NULL;
		HRESULT hr = ::SHGetKnownFolderPath(x.id, 0, NULL, &p);
		bool suffix = false;
		if (p) {
			size_t n = wcslen(p), m = wcslen(x.suffix);
			suffix = n >= m && !_wcsicmp(p + n - m, x.suffix);
		}
		out("SHGetKnownFolderPath", x.n, "%s expected_shape=%d exists=%d", HR(hr).c_str(), suffix, p && ::PathFileExistsW(p));
		out("SHGetKnownFolderPath", (std::string("~") + x.n).c_str(), "%s", Q(p).c_str());
		::CoTaskMemFree(p);
	}
	PWSTR p = NULL;
	HRESULT hr = ::SHGetKnownFolderPath(GUID_NULL, 0, NULL, &p);
	out("SHGetKnownFolderPath", "nullguid", "%s out=%s", HR(hr).c_str(), p ? "nonnull" : "NULL");
	::CoTaskMemFree(p);
}

TEST(ole_ShellFileInfo)
{
	::CoInitialize(NULL);
	const std::wstring f = TDir() + L"song.mp3", d = TDir() + L"folder";
	WriteWholeFile(f, "ID3", 3);
	::CreateDirectoryW(d.c_str(), NULL);
	SHFILEINFOW fi = {};
	DWORD_PTR r = ::SHGetFileInfoW(f.c_str(), 0, &fi, sizeof fi, SHGFI_ATTRIBUTES);
	out("SHGetFileInfoW", "attributes.file", "r=%d link=%d folder=%d filesys=%d", r != 0, (fi.dwAttributes & SFGAO_LINK) != 0,
		(fi.dwAttributes & SFGAO_FOLDER) != 0, (fi.dwAttributes & SFGAO_FILESYSTEM) != 0);
	r = ::SHGetFileInfoW(d.c_str(), 0, &fi, sizeof fi, SHGFI_ATTRIBUTES);
	out("SHGetFileInfoW", "attributes.dir", "r=%d folder=%d", r != 0, (fi.dwAttributes & SFGAO_FOLDER) != 0);
	r = ::SHGetFileInfoW((TDir() + L"missing").c_str(), 0, &fi, sizeof fi, SHGFI_ATTRIBUTES);
	out("SHGetFileInfoW", "attributes.missing", "r=%d", r != 0);
	// DirectoryTreeCtrl.cpp:253 - icon + display name of a folder.
	fi = {};
	r = ::SHGetFileInfoW(d.c_str(), 0, &fi, sizeof fi, SHGFI_SMALLICON | SHGFI_ICON | SHGFI_OPENICON | SHGFI_DISPLAYNAME);
	out("SHGetFileInfoW", "icon.displayname", "r=%d icon=%d name=%s", r != 0, fi.hIcon != NULL, Q(fi.szDisplayName).c_str());
	if (fi.hIcon)
		::DestroyIcon(fi.hIcon);
	fi = {};
	r = ::SHGetFileInfoW(L"C:\\", 0, &fi, sizeof fi, SHGFI_SMALLICON | SHGFI_ICON | SHGFI_DISPLAYNAME);
	out("SHGetFileInfoW", "root", "r=%d icon=%d", r != 0, fi.hIcon != NULL);
	out("SHGetFileInfoW", "~root.name", "%s", Q(fi.szDisplayName).c_str());
	if (fi.hIcon)
		::DestroyIcon(fi.hIcon);
	// Emule.cpp:1025 / DirectoryTreeCtrl.cpp:193 - the system image list.
	r = ::SHGetFileInfoW(L".", 0, &fi, sizeof fi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
	out("SHGetFileInfoW", "sysimagelist.small", "himl=%d", r != 0);
	r = ::SHGetFileInfoW(L"x.mp3", FILE_ATTRIBUTE_NORMAL, &fi, sizeof fi, SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
	out("SHGetFileInfoW", "sysimagelist.usefileattributes", "himl=%d", r != 0);
	r = ::SHGetFileInfoW(L"x.mp3", FILE_ATTRIBUTE_NORMAL, &fi, sizeof fi, SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME);
	out("SHGetFileInfoW", "~typename.mp3", "r=%d %s", r != 0, Q(fi.szTypeName).c_str());
	r = ::SHGetFileInfoW(f.c_str(), 0, &fi, 8, SHGFI_ATTRIBUTES);
	out("SHGetFileInfoW", "smallstruct", "r=%d", r != 0);
	::CoUninitialize();
}

TEST(ole_IDList)
{
	::CoInitialize(NULL);
	PIDLIST_ABSOLUTE pidl = NULL;
	HRESULT hr = ::SHParseDisplayName(TDir().c_str(), NULL, &pidl, 0, NULL);
	wchar_t path[MAX_PATH] = L"#";
	BOOL r = pidl ? ::SHGetPathFromIDListW(pidl, path) : FALSE;
	out("SHGetPathFromIDListW", "tdir", "parse=%s r=%d %s", HR(hr).c_str(), r, QN(path).c_str());
	::CoTaskMemFree(pidl);
	pidl = NULL;
	hr = ::SHGetKnownFolderIDList(FOLDERID_ControlPanelFolder, 0, NULL, &pidl);
	wcscpy_s(path, L"#");
	r = pidl ? ::SHGetPathFromIDListW(pidl, path) : FALSE;
	out("SHGetPathFromIDListW", "virtualfolder", "get=%s r=%d %s", HR(hr).c_str(), r, Q(path).c_str());
	::CoTaskMemFree(pidl);
	pidl = NULL;
	hr = ::SHGetKnownFolderIDList(FOLDERID_Desktop, 0, NULL, &pidl);
	r = pidl ? ::SHGetPathFromIDListW(pidl, path) : FALSE;
	out("SHGetPathFromIDListW", "desktop", "r=%d endswith=%d", r, r && wcslen(path) > 8 && !_wcsicmp(path + wcslen(path) - 8, L"\\Desktop"));
	::CoTaskMemFree(pidl);
	ITEMIDLIST empty = {};
	r = ::SHGetPathFromIDListW(&empty, path);
	out("SHGetPathFromIDListW", "emptypidl", "r=%d", r);
	::CoUninitialize();
}

static int CALLBACK BrowseCb(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
	const wchar_t *want = (const wchar_t*)data;
	if (msg == BFFM_INITIALIZED) {
		::SendMessageW(h, BFFM_SETSELECTIONW, TRUE, (LPARAM)want);
		::SetTimer(h, 1, 1500, NULL);	// fall back to OK even if no SELCHANGED comes
	} else if (msg == BFFM_SELCHANGED) {
		wchar_t p[MAX_PATH] = L"";
		::SHGetPathFromIDListW((PCIDLIST_ABSOLUTE)lp, p);
		std::wstring w(want);
		if (!w.empty() && w.back() == L'\\')
			w.pop_back();
		if (!_wcsicmp(p, w.c_str()))
			::PostMessageW(h, WM_COMMAND, IDOK, 0);
	}
	return 0;
}

TEST_T(ole_SHBrowseForFolder, 60000)
{
	// OtherFunctions.cpp SelectDir: STA, BIF_NEWDIALOGSTYLE and friends,
	// initial selection set from the callback. The dialog is driven to OK
	// programmatically; the result must be the folder that was selected.
	::CoInitialize(NULL);
	::CreateDirectoryW((TDir() + L"pick me").c_str(), NULL);
	std::wstring want = TDir() + L"pick me";
	// Safety net: whatever happens, close dialogs after 20 s.
	HANDLE guard = ::CreateThread(NULL, 0, [](LPVOID) -> DWORD {
		::Sleep(20000);
		DialogCloser dc(0);
		::Sleep(2000);
		return 0;
	}, NULL, 0, NULL);
	for (UINT flags : {(UINT)(BIF_VALIDATE | BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS | BIF_SHAREABLE | BIF_DONTGOBELOWDOMAIN), (UINT)BIF_RETURNONLYFSDIRS}) {
		BROWSEINFOW bi = {};
		bi.lpszTitle = L"symrepro";
		bi.ulFlags = flags;
		bi.lpfn = BrowseCb;
		bi.lParam = (LPARAM)want.c_str();
		DWORD t0 = ::GetTickCount();
		PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderW(&bi);
		wchar_t path[MAX_PATH] = L"";
		BOOL r = pidl ? ::SHGetPathFromIDListW(pidl, path) : FALSE;
		out("SHBrowseForFolderW", Fmt("flags%x", flags).c_str(), "pidl=%s path=%d %s", pidl ? "ok" : "NULL", r, QN(path).c_str());
		out("SHBrowseForFolderW", Fmt("~flags%x.ms", flags).c_str(), "%lu", ::GetTickCount() - t0);
		::CoTaskMemFree(pidl);
	}
	::TerminateThread(guard, 0);
	::CloseHandle(guard);
	::CoUninitialize();
}

TEST(ole_ExtractIconEx)
{
	// Emule.cpp:1170 - icons from a skin resource file.
	std::wstring self = SelfPath();
	HICON lg = NULL, sm = NULL;
	UINT n = ::ExtractIconExW(self.c_str(), 0, &lg, &sm, 1);
	ICONINFO ii = {};
	BITMAP bm = {};
	if (lg && ::GetIconInfo(lg, &ii)) {
		::GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
		if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
		if (ii.hbmMask) ::DeleteObject(ii.hbmMask);
	}
	out("ExtractIconExW", "self.0", "n=%u large=%d sbuf=%d large_w=%ld", n, lg != NULL, sm != NULL, bm.bmWidth);
	if (lg) ::DestroyIcon(lg);
	if (sm) ::DestroyIcon(sm);
	n = ::ExtractIconExW(self.c_str(), -1, NULL, NULL, 0);
	out("ExtractIconExW", "self.count", "%u", n);
	lg = sm = NULL;
	n = ::ExtractIconExW(self.c_str(), 5, &lg, &sm, 1);
	out("ExtractIconExW", "self.outofrange", "n=%u large=%d sbuf=%d", n, lg != NULL, sm != NULL);
	lg = NULL;
	n = ::ExtractIconExW(self.c_str(), 0, &lg, NULL, 1);
	out("ExtractIconExW", "self.largeonly", "n=%u large=%d", n, lg != NULL);
	if (lg) ::DestroyIcon(lg);
	// A bare .ico file, as a skin may ship.
	HMODULE me = ::GetModuleHandleW(NULL);
	HRSRC grp = ::FindResourceW(me, MAKEINTRESOURCEW(103), RT_GROUP_ICON);
	out("ExtractIconExW", "fixture.groupicon", "%s", grp ? "ok" : "NULL");
	WriteWholeFile(TDir() + L"plain.txt", "x", 1);
	lg = sm = NULL;
	n = ::ExtractIconExW((TDir() + L"plain.txt").c_str(), 0, &lg, &sm, 1);
	out("ExtractIconExW", "notanicon", "n=%u large=%d sbuf=%d", n, lg != NULL, sm != NULL);
	n = ::ExtractIconExW((TDir() + L"missing.ico").c_str(), 0, &lg, &sm, 1);
	out("ExtractIconExW", "missing", "n=%u", n);
}

TEST(ole_DragQueryFile)
{
	// SharedFilesCtrl.cpp:1708 - files dropped from Explorer.
	const wchar_t *files[] = {L"C:\\a\\first.txt", L"C:\\\u00e9\\second file.mp3", L"D:\\third"};
	size_t chars = 0;
	for (const wchar_t *f : files)
		chars += wcslen(f) + 1;
	HGLOBAL h = ::GlobalAlloc(GHND, sizeof(DROPFILES) + (chars + 1) * sizeof(wchar_t));
	DROPFILES *df = (DROPFILES*)::GlobalLock(h);
	df->pFiles = sizeof(DROPFILES);
	df->fWide = TRUE;
	wchar_t *p = (wchar_t*)(df + 1);
	for (const wchar_t *f : files) {
		wcscpy_s(p, chars + 1 - (p - (wchar_t*)(df + 1)), f);
		p += wcslen(f) + 1;
	}
	::GlobalUnlock(h);
	HDROP hd = (HDROP)h;
	UINT n = ::DragQueryFileW(hd, UINT_MAX, NULL, 0);
	out("DragQueryFileW", "count", "%u", n);
	for (UINT i = 0; i < n + 1; ++i) {
		UINT len = ::DragQueryFileW(hd, i, NULL, 0);
		wchar_t buf[MAX_PATH] = L"#";
		UINT got = ::DragQueryFileW(hd, i, buf, MAX_PATH);
		out("DragQueryFileW", Fmt("item%u", i).c_str(), "len=%u got=%u %s", len, got, Q(buf).c_str());
	}
	wchar_t sbuf[5] = L"####";
	UINT got = ::DragQueryFileW(hd, 1, sbuf, 5);
	out("DragQueryFileW", "small", "got=%u %s", got, Q(sbuf).c_str());
	// ANSI drop list read through the W function.
	HGLOBAL ha = ::GlobalAlloc(GHND, sizeof(DROPFILES) + 32);
	DROPFILES *da = (DROPFILES*)::GlobalLock(ha);
	da->pFiles = sizeof(DROPFILES);
	da->fWide = FALSE;
	memcpy(da + 1, "C:\\ansi\xe9.txt\0\0", 15);
	::GlobalUnlock(ha);
	wchar_t buf[MAX_PATH] = L"#";
	got = ::DragQueryFileW((HDROP)ha, 0, buf, MAX_PATH);
	out("DragQueryFileW", "ansilist", "got=%u %s", got, Q(buf).c_str());
	::GlobalFree(ha);
	::GlobalFree(h);
}

HELPER(exit_with)
{
	return argc > 0 ? _wtoi(argv[0]) : 0;
}

TEST_T(ole_ShellExecute, 60000)
{
	::CoInitialize(NULL);
	::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	std::wstring self = SelfPath();
	// OtherFunctions.cpp:323-338 / Preview.cpp:292.
	struct { const char *n; const wchar_t *verb; std::wstring file, params; } c[] = {
		{"exe.open", L"open", self, L"--helper exit_with 3"},
		{"exe.nullverb", NULL, self, L"--helper exit_with 4"},
		{"missing.file", NULL, TDir() + L"missing.xyz", L""},
		{"missing.dir", L"open", TDir() + L"nodir\\x.exe", L""},
		{"noassoc", NULL, TDir() + L"file.symrepronoext", L""},
		{"badverb", L"nosuchverb", self, L""},
		{"unknownscheme", NULL, L"symreprounknown://x", L""},
	};
	WriteWholeFile(TDir() + L"file.symrepronoext", "x", 1);
	for (auto &x : c) {
		DialogCloser dc;
		INT_PTR r = (INT_PTR)::ShellExecuteW(NULL, x.verb, x.file.c_str(), x.params.empty() ? NULL : x.params.c_str(), NULL, SW_HIDE);
		::Sleep(1500);
		out("ShellExecuteW", x.n, "%s dialogs=%s", r > 32 ? "ok(>32)" : Fmt("%Id", r).c_str(), dc.Seen().c_str());
	}
	// Preview.cpp:70-106 / ArchiveRecovery.cpp:192 - SEE_MASK_NOCLOSEPROCESS
	// and wait on the process handle.
	SHELLEXECUTEINFOW se = {sizeof se};
	se.fMask = SEE_MASK_NOCLOSEPROCESS;
	se.lpVerb = L"open";
	se.lpFile = self.c_str();
	se.lpParameters = L"--helper exit_with 5";
	se.lpDirectory = TDir().c_str();
	se.nShow = SW_HIDE;
	BOOL r = ::ShellExecuteExW(&se);
	DWORD code = 0;
	if (se.hProcess) {
		::WaitForSingleObject(se.hProcess, 10000);
		::GetExitCodeProcess(se.hProcess, &code);
		::CloseHandle(se.hProcess);
	}
	out("ShellExecuteExW", "exe.wait", "%s hInstApp=%s hProcess=%s exit=%lu", B(r).c_str(), (INT_PTR)se.hInstApp > 32 ? ">32" : Fmt("%Id", (INT_PTR)se.hInstApp).c_str(),
		se.hProcess ? "ok" : "NULL", code);
	se = {sizeof se};
	se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	se.lpFile = L"C:\\srt\\definitely_missing.exe";
	se.nShow = SW_HIDE;
	::SetLastError(0);
	r = ::ShellExecuteExW(&se);
	out("ShellExecuteExW", "missing", "%s hInstApp=%Id hProcess=%s", B(r).c_str(), (INT_PTR)se.hInstApp, se.hProcess ? "ok" : "NULL");
	se = {sizeof se};
	se.fMask = SEE_MASK_NOCLOSEPROCESS;
	se.lpFile = self.c_str();
	se.lpParameters = L"--helper exit_with 0";
	se.lpDirectory = L"C:\\srt\\no_such_dir_sr";
	se.nShow = SW_HIDE;
	::SetLastError(0);
	r = ::ShellExecuteExW(&se);
	if (se.hProcess) {
		::WaitForSingleObject(se.hProcess, 10000);
		::CloseHandle(se.hProcess);
	}
	out("ShellExecuteExW", "baddirectory", "%s", B(r).c_str());
	::CoUninitialize();
}

TEST(ole_NotifyIcon)
{
	// TrayDialog.cpp: NIM_ADD / NIM_MODIFY / NIM_DELETE with a full-size
	// NOTIFYICONDATAW (patch 88zm).
	HWND w = ::CreateWindowW(L"STATIC", L"symrepro", 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
	NOTIFYICONDATAW nid = {};
	nid.cbSize = sizeof nid;
	nid.hWnd = w;
	nid.uID = 1;
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	nid.uCallbackMessage = WM_USER + 7;
	nid.hIcon = ::LoadIconW(NULL, IDI_APPLICATION);
	wcscpy_s(nid.szTip, L"symrepro");
	::SetLastError(0);
	BOOL add = ::Shell_NotifyIconW(NIM_ADD, &nid);
	DWORD e = ::GetLastError();
	out("Shell_NotifyIconW", "~add", "r=%d err=%lu", add, e);
	wcscpy_s(nid.szTip, L"symrepro 2");
	BOOL mod = ::Shell_NotifyIconW(NIM_MODIFY, &nid);
	BOOL del = ::Shell_NotifyIconW(NIM_DELETE, &nid);
	BOOL del2 = ::Shell_NotifyIconW(NIM_DELETE, &nid);
	out("Shell_NotifyIconW", "consistency", "modify_matches_add=%d delete_matches_add=%d delete_again=%d", mod == add, del == add, del2);
	NOTIFYICONDATAW old = nid;
	old.cbSize = NOTIFYICONDATAW_V2_SIZE;
	BOOL add2 = ::Shell_NotifyIconW(NIM_ADD, &old);
	out("Shell_NotifyIconW", "v2size.matches_full", "%d", add2 == add);
	::Shell_NotifyIconW(NIM_DELETE, &old);
	nid.cbSize = 12;
	BOOL bad = ::Shell_NotifyIconW(NIM_ADD, &nid);
	out("Shell_NotifyIconW", "badsize", "%d", bad);
	if (bad)
		::Shell_NotifyIconW(NIM_DELETE, &nid);
	APPBARDATA ab = {sizeof ab};
	UINT_PTR st = ::SHAppBarMessage(ABM_GETSTATE, &ab);
	UINT_PTR pos = ::SHAppBarMessage(ABM_GETTASKBARPOS, &ab);
	out("SHAppBarMessage", "~taskbar", "state=%Iu pos=%Iu edge=%u rect=%ld,%ld,%ld,%ld", st, pos, ab.uEdge, ab.rc.left, ab.rc.top, ab.rc.right, ab.rc.bottom);
	::DestroyWindow(w);
}

TEST(ole_FindMimeFromData)
{
	// MediaInfo.cpp:2789 - sniffing only (no file name), FMFD_ENABLEMIMESNIFFING
	// | FMFD_IGNOREMIMETEXTPLAIN, on the first bytes of a shared file.
	::CoInitialize(NULL);
	struct { const char *n; std::string data; } c[] = {
		{"png", std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16)},
		{"jpeg", std::string("\xff\xd8\xff\xe0\0\x10JFIF\0", 11)},
		{"gif", "GIF89a\x01\0\x01\0"},
		{"bmp", std::string("BM\x36\0\0\0\0\0\0\0\x36\0\0\0", 14)},
		{"zip", std::string("PK\x03\x04\x14\0\0\0", 8)},
		{"gzip", std::string("\x1f\x8b\x08\0\0\0\0\0", 8)},
		{"pdf", "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"},
		{"rar", std::string("Rar!\x1a\x07\0", 7)},
		{"7z", std::string("7z\xbc\xaf\x27\x1c\0\x04", 8)},
		{"mp3.id3", std::string("ID3\x03\0\0\0\0\0\0", 10)},
		{"mp3.frame", std::string("\xff\xfb\x90\x64\0\0\0\0", 8)},
		{"avi", std::string("RIFF\0\0\0\0AVI LIST", 16)},
		{"wav", std::string("RIFF\0\0\0\0WAVEfmt ", 16)},
		{"mkv", std::string("\x1a\x45\xdf\xa3\x93\x42\x82\x88matroska", 16)},
		{"mp4", std::string("\0\0\0\x18" "ftypmp42\0\0\0\0", 16)},
		{"mpeg", std::string("\0\0\x01\xba\x44\0\x04\0", 8)},
		{"exe", std::string("MZ\x90\0\x03\0\0\0\x04\0\0\0\xff\xff", 14)},
		{"ogg", std::string("OggS\0\x02\0\0\0\0", 10)},
		{"flac", "fLaC\0\0\0\x22"},
		{"html", "<html><head><title>x</title></head><body></body></html>"},
		{"xml", "<?xml version=\"1.0\"?><a/>"},
		{"text", "just some plain ascii text in a file\r\n"},
		{"utf16text", std::string("\xff\xfeh\0e\0l\0l\0o\0", 12)},
		{"binary", std::string("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x00\xfe\xfd", 12)},
		{"empty", ""},
		{"onebyte", "x"},
		{"ps", "%!PS-Adobe-3.0\n"},
		{"tiff", std::string("II*\0\x08\0\0\0", 8)},
		{"ico", std::string("\0\0\x01\0\x01\0\x10\x10", 8)},
		{"wmv", std::string("\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\0\xaa\0\x62\xce\x6c", 16)},
	};
	for (auto &x : c) {
		LPWSTR mime = NULL;
		HRESULT hr = ::FindMimeFromData(NULL, NULL, (LPVOID)x.data.data(), (DWORD)x.data.size(), NULL,
			FMFD_ENABLEMIMESNIFFING | FMFD_IGNOREMIMETEXTPLAIN, &mime, 0);
		out("FindMimeFromData", x.n, "%s %s", HR(hr).c_str(), Q(mime).c_str());
		::CoTaskMemFree(mime);
	}
	LPWSTR mime = NULL;
	HRESULT hr = ::FindMimeFromData(NULL, L"file.mp3", NULL, 0, NULL, 0, &mime, 0);
	out("FindMimeFromData", "byname.mp3", "%s %s", HR(hr).c_str(), Q(mime).c_str());
	::CoTaskMemFree(mime);
	mime = NULL;
	hr = ::FindMimeFromData(NULL, NULL, NULL, 0, NULL, 0, &mime, 0);
	out("FindMimeFromData", "nothing", "%s", HR(hr).c_str());
	::CoTaskMemFree(mime);
	::CoUninitialize();
}
