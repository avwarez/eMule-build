// Strings, locales, code pages - KERNEL32 / ADVAPI32.
//
// Locale-dependent calls are made with explicit LCIDs (en-US, it-IT) so the
// two platforms are asked the same question; the user/system-default forms
// that eMule actually passes are printed too, but marked '~', because they
// answer for whatever locale the machine is set to.
#include "harness.h"
#include <stdio.h>

static const LCID kEnUS = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);
static const LCID kItIT = MAKELCID(MAKELANGID(LANG_ITALIAN, SUBLANG_ITALIAN), SORT_DEFAULT);

// Strings of the kind eMule sorts: file names, user names, server names.
static const wchar_t *const kSortW[] = {
	L"", L"a", L"A", L"b", L"B", L"ab", L"a b", L"a-b", L"a_b", L"a.b", L"a'b",
	L"file2", L"file10", L"File10", L"01", L"1", L"10", L"9",
	L"ä", L"Ä", L"ae", L"æ", L"ß", L"ss", L"é", L"e", L"è",
	L"ö", L"oe", L"ı", L"İ", L"i", L"I", L"ﬁ", L"fi",
	L"[x]", L"(x)", L"{x}", L"~x", L"#x", L"!x", L"файл",
	L"文件", L"あ", L"ア", L"x́", L"ý", L"a b", L"—",
};

static char Sign(int r)
{
	switch (r) {
	case CSTR_LESS_THAN: return '<';
	case CSTR_EQUAL: return '=';
	case CSTR_GREATER_THAN: return '>';
	default: return '0';
	}
}

static void CompareMatrixW(const char *cas, LCID lcid, DWORD flags)
{
	for (size_t i = 0; i < _countof(kSortW); ++i) {
		std::string row;
		for (size_t j = 0; j < _countof(kSortW); ++j)
			row += Sign(::CompareStringW(lcid, flags, kSortW[i], -1, kSortW[j], -1));
		out("CompareStringW", Fmt("%s[%s]", cas, Q(kSortW[i]).c_str()).c_str(), "%s", row.c_str());
	}
}

TEST(string_CompareStringW)
{
	CompareMatrixW("enUS.0", kEnUS, 0);
	CompareMatrixW("enUS.ignorecase", kEnUS, NORM_IGNORECASE);
	CompareMatrixW("itIT.0", kItIT, 0);
	CompareMatrixW("itIT.ignorecase", kItIT, NORM_IGNORECASE);
	// Explicit lengths, embedded NUL, and the error path.
	out("CompareStringW", "len.prefix", "%c", Sign(::CompareStringW(kEnUS, 0, L"abc", 2, L"ab", -1)));
	out("CompareStringW", "len.nul", "%c", Sign(::CompareStringW(kEnUS, 0, L"a\0b", 3, L"a\0c", 3)));
	::SetLastError(0);
	int r = ::CompareStringW(kEnUS, 0, NULL, -1, L"a", -1);
	out("CompareStringW", "null", "r=%d err=%lu", r, ::GetLastError());
	::SetLastError(0);
	r = ::CompareStringW(kEnUS, 0x80000000, L"a", -1, L"a", -1);
	out("CompareStringW", "badflags", "r=%d err=%lu", r, ::GetLastError());
}

TEST(string_CompareStringA)
{
	// CP1252 bytes, as eMule's ANSI comparisons see them.
	static const char *const s[] = {"", "a", "A", "b", "ab", "a-b", "a_b", "file2", "file10",
		"\xe4", "\xc4", "ae", "\xdf", "ss", "\xe9", "e", "\x80", "\x9c", "oe", "[x]", "(x)"};
	for (LCID lcid : {kEnUS, kItIT})
		for (DWORD flags : {0ul, (DWORD)NORM_IGNORECASE}) {
			for (size_t i = 0; i < _countof(s); ++i) {
				std::string row;
				for (size_t j = 0; j < _countof(s); ++j)
					row += Sign(::CompareStringA(lcid, flags, s[i], -1, s[j], -1));
				out("CompareStringA", Fmt("%04lx.%lu[%s]", lcid, flags, QA(s[i]).c_str()).c_str(), "%s", row.c_str());
			}
		}
}

TEST(string_LCMapStringW)
{
	// The whole BMP through LCMAP_LOWERCASE, en-US, one character at a time -
	// exactly what gen_wclwrtab in kademlia/io/DataIO.cpp does. Kad keyword
	// hashing depends on this table being identical on every node.
	unsigned changed = 0, notOne = 0;
	for (unsigned blk = 0; blk < 0x100; ++blk) {
		std::string line;
		for (unsigned lo = 0; lo < 0x100; ++lo) {
			WCHAR ch = (WCHAR)((blk << 8) | lo), wch = ch;
			int r = ::LCMapStringW(kEnUS, LCMAP_LOWERCASE, &wch, 1, &wch, 1);
			if (r != 1) {
				++notOne;
				line += Fmt(" %04x:r%d", ch, r);
			} else if (wch != ch) {
				++changed;
				line += Fmt(" %04x>%04x", ch, wch);
			}
		}
		if (!line.empty())
			out("LCMapStringW", Fmt("lower.blk%02x", blk).c_str(), "%s", line.c_str() + 1);
	}
	out("LCMapStringW", "lower.total", "changed=%u r!=1:%u", changed, notOne);
	// Whole-string forms.
	WCHAR buf[64];
	int r = ::LCMapStringW(kEnUS, LCMAP_LOWERCASE, L"ÀBC İ ΣΣ", -1, buf, _countof(buf));
	out("LCMapStringW", "lower.str", "r=%d %s", r, Q(buf).c_str());
	r = ::LCMapStringW(kEnUS, LCMAP_UPPERCASE, L"àbc ß ı ς", -1, buf, _countof(buf));
	out("LCMapStringW", "upper.str", "r=%d %s", r, Q(buf).c_str());
	r = ::LCMapStringW(kEnUS, LCMAP_LOWERCASE, L"ABC", -1, NULL, 0);
	out("LCMapStringW", "sizequery", "r=%d", r);
	::SetLastError(0);
	r = ::LCMapStringW(kEnUS, LCMAP_LOWERCASE, L"ABCDEF", -1, buf, 3);
	out("LCMapStringW", "tooshort", "r=%d err=%lu", r, ::GetLastError());
}

TEST(string_WideCharToMultiByte)
{
	// eMule's CUnicodeToUTF8 / CUnicodeToMultiByte (StringConversion.h): first a
	// size query with -1, then the conversion; and the explicit-length form.
	static const wchar_t *const s[] = {L"", L"abc", L"äöüß", L"€", L"文件名",
		L"\U0001F600", L"\xD800", L"a\xDC00z", L"ф", L"ÿĀ", L"\x80\x9f"};
	for (UINT cp : {(UINT)CP_UTF8, 1252u, 1251u, 932u, 437u, (UINT)CP_ACP}) {
		for (const wchar_t *w : s) {
			int n = ::WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
			char buf[64] = {0};
			BOOL usedDef = FALSE;
			int m = ::WideCharToMultiByte(cp, 0, w, -1, buf, sizeof buf, NULL, cp == CP_UTF8 ? NULL : &usedDef);
			const char *cas = cp == CP_ACP ? "~acp" : "cp";
			out("WideCharToMultiByte", Fmt("%s%u%s", cas, cp == CP_ACP ? 0 : cp, Q(w).c_str()).c_str(),
				"size=%d conv=%d def=%d %s", n, m, usedDef, Hex(buf, m > 0 ? m : 0).c_str());
		}
	}
	// Buffer too small, explicit length without terminator, invalid-chars flag.
	char b[4];
	::SetLastError(0);
	int r = ::WideCharToMultiByte(CP_UTF8, 0, L"abcdef", -1, b, sizeof b, NULL, NULL);
	out("WideCharToMultiByte", "small", "r=%d err=%lu", r, ::GetLastError());
	r = ::WideCharToMultiByte(CP_UTF8, 0, L"abcdef", 3, b, sizeof b, NULL, NULL);
	out("WideCharToMultiByte", "len3", "r=%d %s", r, Hex(b, r > 0 ? r : 0).c_str());
	::SetLastError(0);
	r = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, L"a\xD800z", -1, NULL, 0, NULL, NULL);
	out("WideCharToMultiByte", "invalid", "r=%d err=%lu", r, ::GetLastError());
	::SetLastError(0);
	r = ::WideCharToMultiByte(CP_UTF8, 0, L"a", -1, b, sizeof b, "?", NULL);
	out("WideCharToMultiByte", "utf8.defchar", "r=%d err=%lu", r, ::GetLastError());
}

TEST(string_GetACP)
{
	out("GetACP", "~value", "%u", ::GetACP());
	out("GetThreadLocale", "~value", "%04lx", ::GetThreadLocale());
	out("GetUserDefaultLCID", "~value", "%04lx", ::GetUserDefaultLCID());
	out("GetUserDefaultUILanguage", "~value", "%04x", ::GetUserDefaultUILanguage());
}

TEST(string_SetThreadLocale)
{
	// I18n.cpp: SetThreadLocale(lcid) for the chosen UI language, then
	// GetThreadLocale() feeds CompareString.
	for (LCID l : {kItIT, kEnUS, (LCID)MAKELCID(MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN), SORT_DEFAULT),
			(LCID)MAKELCID(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED), SORT_DEFAULT), (LCID)0x12345678}) {
		BOOL ok = ::SetThreadLocale(l);
		std::string r = B(ok);
		out("SetThreadLocale", Fmt("%08lx", l).c_str(), "%s now=%04lx", r.c_str(), ::GetThreadLocale());
	}
}

TEST(string_GetLocaleInfoW)
{
	// I18n.cpp asks LOCALE_IDEFAULTANSICODEPAGE into a 6-char buffer;
	// PPgGeneral.cpp asks LOCALE_SLANGUAGE for every language eMule ships.
	static const WORD langs[] = {
		MAKELANGID(LANG_ARABIC, SUBLANG_DEFAULT), MAKELANGID(LANG_BASQUE, SUBLANG_DEFAULT),
		MAKELANGID(LANG_BULGARIAN, SUBLANG_DEFAULT), MAKELANGID(LANG_CATALAN, SUBLANG_DEFAULT),
		MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED), MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL),
		MAKELANGID(LANG_CROATIAN, SUBLANG_DEFAULT), MAKELANGID(LANG_CZECH, SUBLANG_DEFAULT),
		MAKELANGID(LANG_DANISH, SUBLANG_DEFAULT), MAKELANGID(LANG_DUTCH, SUBLANG_DEFAULT),
		MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), MAKELANGID(LANG_ESTONIAN, SUBLANG_DEFAULT),
		MAKELANGID(LANG_FARSI, SUBLANG_DEFAULT), MAKELANGID(LANG_FINNISH, SUBLANG_DEFAULT),
		MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH), MAKELANGID(LANG_GALICIAN, SUBLANG_DEFAULT),
		MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN), MAKELANGID(LANG_GREEK, SUBLANG_DEFAULT),
		MAKELANGID(LANG_HEBREW, SUBLANG_DEFAULT), MAKELANGID(LANG_HUNGARIAN, SUBLANG_DEFAULT),
		MAKELANGID(LANG_ITALIAN, SUBLANG_ITALIAN), MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT),
		MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT), MAKELANGID(LANG_LATVIAN, SUBLANG_DEFAULT),
		MAKELANGID(LANG_LITHUANIAN, SUBLANG_DEFAULT), MAKELANGID(LANG_NORWEGIAN, SUBLANG_NORWEGIAN_BOKMAL),
		MAKELANGID(LANG_POLISH, SUBLANG_DEFAULT), MAKELANGID(LANG_PORTUGUESE, SUBLANG_PORTUGUESE),
		MAKELANGID(LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN), MAKELANGID(LANG_ROMANIAN, SUBLANG_DEFAULT),
		MAKELANGID(LANG_RUSSIAN, SUBLANG_DEFAULT), MAKELANGID(LANG_SLOVAK, SUBLANG_DEFAULT),
		MAKELANGID(LANG_SLOVENIAN, SUBLANG_DEFAULT), MAKELANGID(LANG_SPANISH, SUBLANG_SPANISH_MODERN),
		MAKELANGID(LANG_SWEDISH, SUBLANG_DEFAULT), MAKELANGID(LANG_THAI, SUBLANG_DEFAULT),
		MAKELANGID(LANG_TURKISH, SUBLANG_DEFAULT), MAKELANGID(LANG_UKRAINIAN, SUBLANG_DEFAULT),
		MAKELANGID(LANG_VIETNAMESE, SUBLANG_DEFAULT), MAKELANGID(LANG_WELSH, SUBLANG_DEFAULT),
	};
	for (WORD lang : langs) {
		LCID lcid = MAKELCID(lang, SORT_DEFAULT);
		WCHAR cp[6] = L"";
		int r1 = ::GetLocaleInfoW(lcid, LOCALE_IDEFAULTANSICODEPAGE, cp, 6);
		WCHAR name[128] = L"";
		int r2 = ::GetLocaleInfoW(lcid, LOCALE_SLANGUAGE, name, _countof(name));
		WCHAR eng[128] = L"";
		int r3 = ::GetLocaleInfoW(lcid, LOCALE_SENGLANGUAGE, eng, _countof(eng));
		out("GetLocaleInfoW", Fmt("%04x", lang).c_str(), "acp=%d:%s slang=%d:%s eng=%d:%s",
			r1, Q(cp).c_str(), r2, Q(name).c_str(), r3, Q(eng).c_str());
	}
	WCHAR small[2];
	::SetLastError(0);
	int r = ::GetLocaleInfoW(kEnUS, LOCALE_SLANGUAGE, small, 2);
	out("GetLocaleInfoW", "tooshort", "r=%d err=%lu", r, ::GetLastError());
	r = ::GetLocaleInfoW(kEnUS, LOCALE_SLANGUAGE, NULL, 0);
	out("GetLocaleInfoW", "sizequery", "r=%d", r);
}

TEST(string_GetDateTimeFormat)
{
	SYSTEMTIME st = {2026, 2, 0, 3, 7, 5, 9, 0};	// 3 Feb 2026 07:05:09
	for (LCID l : {kEnUS, kItIT}) {
		WCHAR d[100] = L"", t[100] = L"";
		int rd = ::GetDateFormatW(l, DATE_SHORTDATE, &st, NULL, d, 100);
		int rt = ::GetTimeFormatW(l, 0, &st, NULL, t, 100);
		out("GetDateFormatW", Fmt("short.%04lx", l).c_str(), "r=%d %s", rd, Q(d).c_str());
		out("GetTimeFormatW", Fmt("default.%04lx", l).c_str(), "r=%d %s", rt, Q(t).c_str());
		rd = ::GetDateFormatW(l, DATE_LONGDATE, &st, NULL, d, 100);
		out("GetDateFormatW", Fmt("long.%04lx", l).c_str(), "r=%d %s", rd, Q(d).c_str());
		rd = ::GetDateFormatW(l, 0, &st, L"yyyy'-'MM'-'dd ddd", d, 100);
		out("GetDateFormatW", Fmt("picture.%04lx", l).c_str(), "r=%d %s", rd, Q(d).c_str());
	}
	WCHAR d[100] = L"", t[100] = L"";
	int rd = ::GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, d, 100);
	int rt = ::GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, t, 100);
	out("GetDateFormatW", "~userdefault", "r=%d %s", rd, Q(d).c_str());
	out("GetTimeFormatW", "~userdefault", "r=%d %s", rt, Q(t).c_str());
	SYSTEMTIME bad = {2026, 13, 0, 40, 25, 61, 61, 0};
	::SetLastError(0);
	rd = ::GetDateFormatW(kEnUS, DATE_SHORTDATE, &bad, NULL, d, 100);
	out("GetDateFormatW", "invalid", "r=%d err=%lu", rd, ::GetLastError());
	::SetLastError(0);
	rt = ::GetTimeFormatW(kEnUS, 0, &bad, NULL, t, 100);
	out("GetTimeFormatW", "invalid", "r=%d err=%lu", rt, ::GetLastError());
}

TEST(string_GetNumberFormatW)
{
	// OtherFunctions.cpp CastItoUIXBytes & co: a NUMBERFMT with explicit
	// separators, so the locale only supplies what the struct leaves out.
	NUMBERFMT nf = {0};
	nf.NumDigits = 2;
	nf.LeadingZero = 1;
	nf.Grouping = 3;
	nf.lpDecimalSep = const_cast<LPWSTR>(L".");
	nf.lpThousandSep = const_cast<LPWSTR>(L",");
	nf.NegativeOrder = 1;
	static const wchar_t *const vals[] = {L"0", L"1", L"1234567.891", L"-1234.5", L"0.005", L".5", L"12345678901234567890",
		L"1e5", L"abc", L"", L" 12", L"1,234", L"999.995"};
	for (const wchar_t *v : vals) {
		WCHAR buf[80] = L"";
		::SetLastError(0);
		int r = ::GetNumberFormatW(kEnUS, 0, v, &nf, buf, 80);
		out("GetNumberFormatW", Fmt("nf%s", Q(v).c_str()).c_str(), "r=%d err=%lu %s", r, r ? 0 : ::GetLastError(), Q(buf).c_str());
		r = ::GetNumberFormatW(kItIT, 0, v, NULL, buf, 80);
		out("GetNumberFormatW", Fmt("itIT%s", Q(v).c_str()).c_str(), "r=%d %s", r, Q(buf).c_str());
	}
	WCHAR buf[80] = L"";
	int r = ::GetNumberFormatW(LOCALE_SYSTEM_DEFAULT, 0, L"1234.5", &nf, buf, 80);
	out("GetNumberFormatW", "~systemdefault", "r=%d %s", r, Q(buf).c_str());
	nf.Grouping = 32;	// "3;2" Indian grouping
	r = ::GetNumberFormatW(kEnUS, 0, L"123456789", &nf, buf, 80);
	out("GetNumberFormatW", "grouping32", "r=%d %s", r, Q(buf).c_str());
}

TEST(string_MulDiv)
{
	static const int v[][3] = {{10, 96, 72}, {-10, 96, 72}, {11, 96, 72}, {1, 1, 2}, {3, 1, 2}, {-1, 1, 2}, {-3, 1, 2},
		{5, 5, 0}, {0x7fffffff, 2, 1}, {0x7fffffff, 2, 2}, {-0x7fffffff - 1, 1, -1}, {1000000, 1000000, 3}, {7, -1, 2},
		{2, 3, -4}, {-2, -3, -4}, {120, 72, 96}, {-13, 72, 96}};
	for (auto &x : v)
		out("MulDiv", Fmt("%d*%d/%d", x[0], x[1], x[2]).c_str(), "%d", ::MulDiv(x[0], x[1], x[2]));
}

TEST(string_IsTextUnicode)
{
	// HttpDownloadDlg.cpp: IsTextUnicode on WinINet status-callback strings,
	// with uFlags = IS_TEXT_UNICODE_UNICODE_MASK.
	struct { const char *name; const void *p; int n; } c[] = {
		{"ascii", "hello world", 11},
		{"utf16", L"hello world", 22},
		{"utf16bom", "\xff\xfeh\0i\0", 6},
		{"utf16revbom", "\xfe\xff\0h\0i", 6},
		{"odd", "abc", 3},
		{"ip", "192.168.1.1", 11},
		{"utf16ip", L"192.168.1.1", 22},
		{"one", "a", 1},
		{"zeros", "\0\0\0\0", 4},
		{"cjk", L"文件名", 6},
	};
	for (auto &x : c) {
		for (int flags : {IS_TEXT_UNICODE_UNICODE_MASK, IS_TEXT_UNICODE_STATISTICS, IS_TEXT_UNICODE_SIGNATURE, 0xFFFF}) {
			INT f = flags;
			BOOL r = ::IsTextUnicode(x.p, x.n, &f);
			out("IsTextUnicode", Fmt("%s.%04x", x.name, flags).c_str(), "r=%d flags=%04x", r, f);
		}
		BOOL r = ::IsTextUnicode(x.p, x.n, NULL);
		out("IsTextUnicode", Fmt("%s.null", x.name).c_str(), "r=%d", r);
	}
}

TEST(string_FormatMessageW)
{
	// OtherFunctions.cpp GetErrorMessage: FROM_SYSTEM | IGNORE_INSERTS |
	// ALLOCATE_BUFFER, then LocalFree. The text is the platform's own wording
	// and is compared only for presence; what must match is whether a message
	// exists at all for the codes eMule reports.
	static const DWORD codes[] = {0, 2, 3, 5, 32, 80, 112, 183, 1224, 10035, 10038, 10048, 10049, 10053, 10054,
		10060, 10061, 10065, 11001, 12002, 12007, 12029, 0xDEADBEEF};
	for (DWORD e : codes) {
		LPWSTR p = NULL;
		DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&p, 0, NULL);
		DWORD err = n ? 0 : ::GetLastError();
		out("FormatMessageW", Fmt("sys.%lu", e).c_str(), "found=%d err=%lu", n > 0, err);
		if (n)
			out("FormatMessageW", Fmt("~sys.%lu.text", e).c_str(), "%s", Q(p).c_str());
		HLOCAL h = ::LocalFree(p);
		out("LocalFree", Fmt("after.%lu", e).c_str(), "%s", h == NULL ? "NULL" : "nonnull");
	}
	// WinINet codes come from the module, as GetErrorMessage does for 12000+.
	HMODULE hWinInet = ::LoadLibraryW(L"wininet.dll");
	for (DWORD e : {12002ul, 12007ul, 12029ul, 12152ul}) {
		LPWSTR p = NULL;
		DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
			hWinInet, e, 0, (LPWSTR)&p, 0, NULL);
		out("FormatMessageW", Fmt("wininet.%lu", e).c_str(), "found=%d", n > 0);
		::LocalFree(p);
	}
	// Inserts, string source.
	WCHAR buf[64];
	DWORD_PTR args[] = {(DWORD_PTR)L"xy", 42};
	DWORD n = ::FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY, L"a %1 b %2!d! c%n", 0, 0, buf, 64, (va_list*)args);
	out("FormatMessageW", "string.inserts", "n=%lu %s", n, Q(buf).c_str());
	out("LocalFree", "null", "%s", ::LocalFree(NULL) == NULL ? "NULL" : "nonnull");
}
