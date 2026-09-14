// filerepro.cpp - a standalone specimen for the life of an eMule part file.
//
// WHY THIS EXISTS
// ---------------
// The fourth specimen of the dual bench, and the one with the widest surface:
// 25 of the Win32 functions eMule actually calls are about files, and they are
// the ones an emulation layer has to map onto a completely different
// filesystem. What eMule does to a download is create a .part file, grow it,
// write blocks into it, ask how much room it really takes, and finally move it
// to the incoming directory - each step through a call whose answer it trusts.
//
// Three of those answers change what eMule DOES, not just what it reports:
//
//   GetVolumeInformation - eMule asks for the filesystem NAME and branches on
//   it (OtherFunctions.cpp:2766, IsNTFSVolume), because it compensates the NTFS
//   daylight-saving file time behaviour when scanning shared files. Report
//   something other than "NTFS" and it takes the other branch, and the dates of
//   every shared file shift by an hour.
//
//   GetCompressedFileSize - eMule's GetDiskFileSize (OtherFunctions.cpp:2620)
//   uses it to learn how much disk a part file really occupies, which is not
//   its length when the file is sparse. It also reads GetLastError() without
//   having cleared it first.
//
//   MoveFileWithProgress - the completion of a download (PartFile.cpp:2859),
//   with an explicit retry for ERROR_SHARING_VIOLATION, which is Windows
//   refusing to move a file somebody still holds open. A platform that allows
//   that move instead takes the retry - and the refusal that protects the file -
//   out of the picture.
//
// WHAT IT MEASURES
// ---------------
//   --mode contract    a fixed table of deterministic probes, printed as
//                      timestamp-free PROBE lines for the workflow to DIFF
//                      between Windows and Wine. Windows is the oracle.
//   --mode lifecycle   the whole life of a part file, over and over, with the
//                      content verified byte for byte at the end: create, grow,
//                      write blocks at random offsets, flush, close, ask its
//                      size on disk, move it, read it back, delete it.
//
// Built and run by .github/workflows/build-filerepro.yml on the Windows runner
// and, with the very same x64 binary, under Wine on the Linux runner.

#include <afxwin.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>

CWinApp theApp;

/////////////////////////////////////////////////////////////////////////////
// Options

enum EMode
{
	MODE_CONTRACT = 0,
	MODE_LIFECYCLE
};

static const char *const s_pszModeNames[] = { "contract", "lifecycle" };

struct SOptions
{
	int nMode;
	int nRounds;      // lifecycle: how many part files to live through
	int nFileMB;      // lifecycle: size of one part file
	int nBlockKB;     // lifecycle: size of one written block
	int nBigGB;       // contract: how far out the sparse file goes
};

static SOptions s_opt = { MODE_CONTRACT, 20, 8, 64, 4 };

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
// Content that verifies itself: a byte's value is a function of its offset, so
// a block read back from anywhere proves both its content and its place.

static inline BYTE PatternByte(ULONGLONG uOff)
{
	ULONGLONG x = uOff + 0x9E3779B97F4A7C15ull;
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	return (BYTE)(x >> 33);
}

static void FillPattern(BYTE *pBuf, ULONGLONG uOff, DWORD dwLen)
{
	for (DWORD i = 0; i < dwLen; ++i)
		pBuf[i] = PatternByte(uOff + i);
}

static bool CheckPattern(const BYTE *pBuf, ULONGLONG uOff, DWORD dwLen)
{
	for (DWORD i = 0; i < dwLen; ++i)
		if (pBuf[i] != PatternByte(uOff + i))
			return false;
	return true;
}

/////////////////////////////////////////////////////////////////////////////
// eMule's own helpers, copied so that the probe measures what eMule measures

// OtherFunctions.cpp:751
static ULONGLONG GetFreeDiskSpaceX(LPCTSTR pDirectory)
{
	ULARGE_INTEGER nFreeDiskSpace;
	return ::GetDiskFreeSpaceEx(pDirectory, &nFreeDiskSpace, NULL, NULL) ? nFreeDiskSpace.QuadPart : 0;
}

// OtherFunctions.cpp:2620, verbatim - including reading GetLastError() without
// having set it to zero first, which is what makes the failure path a lottery.
static ULONGLONG GetDiskFileSize(LPCTSTR pszFilePath)
{
	ULONGLONG ullCompFileSize;
	((LPDWORD)&ullCompFileSize)[0] = ::GetCompressedFileSize(pszFilePath, &((LPDWORD)&ullCompFileSize)[1]);
	if (((LPDWORD)&ullCompFileSize)[0] != INVALID_FILE_SIZE || ::GetLastError() == NO_ERROR)
		return ullCompFileSize;

	WIN32_FIND_DATA fd;
	HANDLE hFind = ::FindFirstFile(pszFilePath, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return 0;
	::FindClose(hFind);

	return ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
}

/////////////////////////////////////////////////////////////////////////////
// Somewhere to work

static TCHAR s_szDir[MAX_PATH];

static bool MakeWorkDir()
{
	TCHAR szTemp[MAX_PATH];
	if (!::GetTempPath(MAX_PATH, szTemp))
		return false;
	_sntprintf_s(s_szDir, _countof(s_szDir), _TRUNCATE, _T("%sfilerepro%u"), szTemp, ::GetCurrentProcessId());
	return ::CreateDirectory(s_szDir, NULL) != FALSE;
}

static void PathIn(TCHAR *pszOut, size_t nOut, LPCTSTR pszName)
{
	_sntprintf_s(pszOut, nOut, _TRUNCATE, _T("%s\\%s"), s_szDir, pszName);
}

// The way eMule opens a part file it is working on: shared for everything,
// because its own reading and writing threads both hold it.
static HANDLE OpenPartFile(LPCTSTR pszPath, DWORD dwAccess, DWORD dwCreate)
{
	return ::CreateFile(pszPath, dwAccess
		, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
		, NULL, dwCreate, FILE_ATTRIBUTE_NORMAL, NULL);
}

static bool WritePattern(HANDLE hFile, ULONGLONG uOff, DWORD dwLen, BYTE *pBuf)
{
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)uOff;
	if (!::SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
		return false;
	FillPattern(pBuf, uOff, dwLen);
	DWORD dwWritten = 0;
	return ::WriteFile(hFile, pBuf, dwLen, &dwWritten, NULL) && dwWritten == dwLen;
}

static bool ReadAndCheck(HANDLE hFile, ULONGLONG uOff, DWORD dwLen, BYTE *pBuf)
{
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)uOff;
	if (!::SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
		return false;
	DWORD dwRead = 0;
	if (!::ReadFile(hFile, pBuf, dwLen, &dwRead, NULL) || dwRead != dwLen)
		return false;
	return CheckPattern(pBuf, uOff, dwLen);
}

// The progress callback eMule passes to MoveFileWithProgress. Whether it is
// ever called is part of what is being measured.
static volatile LONG s_nProgressCalls = 0;

static DWORD CALLBACK MoveProgressRoutine(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER
	, DWORD, DWORD, HANDLE, HANDLE, LPVOID)
{
	::InterlockedIncrement(&s_nProgressCalls);
	return PROGRESS_CONTINUE;
}

/////////////////////////////////////////////////////////////////////////////
// Mode "contract": deterministic probes

static int RunContract()
{
	int nExit = 0;
	BYTE *pBuf = new BYTE[64 * 1024];
	TCHAR szA[MAX_PATH], szB[MAX_PATH];

	// 1. The one eMule branches on. It asks for the filesystem NAME and
	//    compares it with "NTFS" to decide whether to compensate the daylight
	//    saving behaviour of file times on every shared file.
	{
		TCHAR szRoot[MAX_PATH];
		::lstrcpyn(szRoot, s_szDir, 4);          // "C:\"
		DWORD dwMaxComponent = 0, dwFlags = 0;
		TCHAR szFsName[MAX_PATH + 1];
		szFsName[0] = 0;
		const BOOL bOk = ::GetVolumeInformation(szRoot, NULL, 0, NULL, &dwMaxComponent, &dwFlags, szFsName, _countof(szFsName));
		Probe("volume-info", "ok=%d fs=%ls ntfs=%d maxcomp=%u sparse=%d"
			, bOk != 0, szFsName, ::lstrcmpi(szFsName, _T("NTFS")) == 0, dwMaxComponent
			, (dwFlags & FILE_SUPPORTS_SPARSE_FILES) != 0);
	}

	// 2. Free space, and whether it notices a file being put on the disk.
	{
		const ULONGLONG uBefore = GetFreeDiskSpaceX(s_szDir);
		PathIn(szA, _countof(szA), _T("space.tmp"));
		HANDLE h = OpenPartFile(szA, GENERIC_WRITE, CREATE_ALWAYS);
		bool bWrote = (h != INVALID_HANDLE_VALUE);
		for (int i = 0; bWrote && i < 512; ++i)
			bWrote = WritePattern(h, (ULONGLONG)i * 65536, 65536, pBuf);
		if (h != INVALID_HANDLE_VALUE) {
			::FlushFileBuffers(h);
			::CloseHandle(h);
		}
		const ULONGLONG uAfter = GetFreeDiskSpaceX(s_szDir);
		Probe("freespace", "reported=%d wrote32mb=%d dropped=%s"
			, uBefore > 0, bWrote
			, (uBefore == 0 || uAfter == 0) ? "n/a" : (uBefore - uAfter >= 16 * 1024 * 1024 ? "yes" : "no"));
		::DeleteFile(szA);
	}

	// 3. eMule's GetDiskFileSize on an ordinary file: it must be the length.
	{
		PathIn(szA, _countof(szA), _T("plain.tmp"));
		HANDLE h = OpenPartFile(szA, GENERIC_WRITE, CREATE_ALWAYS);
		bool bOk = (h != INVALID_HANDLE_VALUE) && WritePattern(h, 0, 65536, pBuf);
		if (h != INVALID_HANDLE_VALUE)
			::CloseHandle(h);
		const ULONGLONG uDisk = GetDiskFileSize(szA);
		Probe("disksize-plain", "written=%d ondisk=%s", bOk, uDisk == 65536 ? "exact" : (uDisk > 65536 ? "rounded-up" : "WRONG"));
		::DeleteFile(szA);
	}

	// 4. The same on a sparse file - a part file that has been given its final
	//    length before any of it has arrived. What eMule wants here is the room
	//    it really takes, not the length.
	{
		PathIn(szA, _countof(szA), _T("sparse.part"));
		const ULONGLONG uFreeBefore = GetFreeDiskSpaceX(s_szDir);
		HANDLE h = OpenPartFile(szA, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
		DWORD dwRet = 0;
		const BOOL bSparse = (h != INVALID_HANDLE_VALUE)
			&& ::DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwRet, NULL);
		LARGE_INTEGER liSize;
		liSize.QuadPart = (LONGLONG)s_opt.nBigGB << 30;
		const BOOL bSized = (h != INVALID_HANDLE_VALUE)
			&& ::SetFilePointerEx(h, liSize, NULL, FILE_BEGIN) && ::SetEndOfFile(h);
		const bool bWrote = bSized && WritePattern(h, (ULONGLONG)liSize.QuadPart - 4096, 4096, pBuf);
		// Flushed before asking: a cached write is allocated when it reaches the
		// disk, so the room the file takes is not settled until then.
		if (h != INVALID_HANDLE_VALUE) {
			::FlushFileBuffers(h);
			::CloseHandle(h);
		}

		const ULONGLONG uDisk = GetDiskFileSize(szA);
		Probe("disksize-sparse", "sparse=%d sized=%d wrote=%d ondisk=%s"
			, bSparse != 0, bSized != 0, bWrote
			, uDisk == 0 ? "zero"
				: (uDisk <= 1024 * 1024 ? "just-the-written-part"
				: (uDisk < (ULONGLONG)liSize.QuadPart / 2 ? "much-less" : "full-length")));

		// What the file costs is a different question from what the API says it
		// costs, and only one of the two can be answered by asking the file. The
		// disk itself answers the other.
		const ULONGLONG uFreeAfter = GetFreeDiskSpaceX(s_szDir);
		Probe("sparse-costs-disk", "measurable=%d eaten=%s"
			, uFreeBefore > 0 && uFreeAfter > 0
			, (uFreeBefore == 0 || uFreeAfter == 0) ? "n/a"
				: (uFreeBefore - uFreeAfter > (ULONGLONG)1 << 30 ? "yes-the-whole-length" : "no-just-the-blocks"));

		// The same file through the other two calls eMule uses on it.
		WIN32_FIND_DATA fd;
		HANDLE hFind = ::FindFirstFile(szA, &fd);
		const bool bFound = (hFind != INVALID_HANDLE_VALUE);
		ULONGLONG uFound = 0;
		if (bFound) {
			uFound = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
			::FindClose(hFind);
		}
		Probe("findfirst-length", "found=%d length=%s", bFound, uFound == (ULONGLONG)liSize.QuadPart ? "exact" : "WRONG");

		const DWORD dwAttr = ::GetFileAttributes(szA);
		Probe("attributes-sparse", "ok=%d sparse-flag=%d"
			, dwAttr != INVALID_FILE_ATTRIBUTES
			, dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_SPARSE_FILE) != 0);
		::DeleteFile(szA);
	}

	// 5. The failure path of eMule's helper, which reads GetLastError() without
	//    having cleared it: on a file that is not there, the answer depends on
	//    what the last unrelated call happened to leave behind.
	{
		PathIn(szA, _countof(szA), _T("not-here.tmp"));
		::SetLastError(ERROR_SUCCESS);
		const ULONGLONG uDisk = GetDiskFileSize(szA);
		Probe("disksize-missing", "returns=%s", uDisk == 0 ? "zero" : "NON-ZERO");
	}

	// 6. The creation time eMule writes back when it finds the NTFS tunnelling
	//    behaviour has given a new file an old creation date.
	{
		PathIn(szA, _countof(szA), _T("times.tmp"));
		HANDLE h = OpenPartFile(szA, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
		SYSTEMTIME st = { 2001, 2, 0, 3, 4, 5, 6, 0 };   // 2001-02-03 04:05:06
		FILETIME ftWanted;
		BOOL bSet = FALSE;
		if (h != INVALID_HANDLE_VALUE && ::SystemTimeToFileTime(&st, &ftWanted))
			bSet = ::SetFileTime(h, &ftWanted, NULL, NULL);
		FILETIME ftCreate, ftAccess, ftWrite;
		const BOOL bGot = (h != INVALID_HANDLE_VALUE) && ::GetFileTime(h, &ftCreate, &ftAccess, &ftWrite);
		if (h != INVALID_HANDLE_VALUE)
			::CloseHandle(h);
		Probe("filetime-creation", "set=%d read=%d kept=%d"
			, bSet != 0, bGot != 0
			, bGot && ::CompareFileTime(&ftCreate, &ftWanted) == 0);
		::DeleteFile(szA);
	}

	// 7. What eMule holds a part file open with, against the two operations it
	//    performs on a finished download. On Windows a file open like this can
	//    be neither deleted nor moved, and eMule has an explicit retry for the
	//    refusal; a platform that allows either takes that protection away.
	{
		PathIn(szA, _countof(szA), _T("held1.part"));
		HANDLE h1 = OpenPartFile(szA, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
		if (h1 != INVALID_HANDLE_VALUE) {
			WritePattern(h1, 0, 65536, pBuf);
			::SetLastError(ERROR_SUCCESS);
			const BOOL bDeleted = ::DeleteFile(szA);
			const DWORD dwErr = ::GetLastError();
			Probe("delete-open", "deleted=%d err=%u", bDeleted != 0, bDeleted ? 0 : dwErr);
			::CloseHandle(h1);
		} else
			Probe("delete-open", "skipped");
		::DeleteFile(szA);

		// A separate file, untouched by the probe above.
		PathIn(szA, _countof(szA), _T("held2.part"));
		PathIn(szB, _countof(szB), _T("held2.done"));
		HANDLE h2 = OpenPartFile(szA, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
		if (h2 != INVALID_HANDLE_VALUE) {
			WritePattern(h2, 0, 65536, pBuf);
			::SetLastError(ERROR_SUCCESS);
			const BOOL bMoved = ::MoveFileWithProgress(szA, szB, MoveProgressRoutine, NULL, MOVEFILE_COPY_ALLOWED);
			const DWORD dwErr = ::GetLastError();
			// The error number itself, not a name for it: eMule retries on
			// exactly ERROR_SHARING_VIOLATION (32) and on nothing else.
			Probe("move-open", "moved=%d err=%u", bMoved != 0, bMoved ? 0 : dwErr);
			::CloseHandle(h2);
		} else
			Probe("move-open", "skipped");
		::DeleteFile(szA);
		::DeleteFile(szB);
	}

	// 8. The completion of a download, on a file nobody is holding.
	{
		PathIn(szA, _countof(szA), _T("done.part"));
		PathIn(szB, _countof(szB), _T("done.avi"));
		HANDLE h = OpenPartFile(szA, GENERIC_WRITE, CREATE_ALWAYS);
		bool bWrote = (h != INVALID_HANDLE_VALUE);
		for (int i = 0; bWrote && i < 64; ++i)
			bWrote = WritePattern(h, (ULONGLONG)i * 65536, 65536, pBuf);
		if (h != INVALID_HANDLE_VALUE)
			::CloseHandle(h);

		::InterlockedExchange(&s_nProgressCalls, 0);
		const BOOL bMoved = ::MoveFileWithProgress(szA, szB, MoveProgressRoutine, NULL, MOVEFILE_COPY_ALLOWED);

		HANDLE hCheck = OpenPartFile(szB, GENERIC_READ, OPEN_EXISTING);
		bool bIntact = (hCheck != INVALID_HANDLE_VALUE);
		for (int i = 0; bIntact && i < 64; ++i)
			bIntact = ReadAndCheck(hCheck, (ULONGLONG)i * 65536, 65536, pBuf);
		if (hCheck != INVALID_HANDLE_VALUE)
			::CloseHandle(hCheck);

		Probe("move-progress", "moved=%d intact=%d gone=%d callback=%s"
			, bMoved != 0, bIntact
			, ::GetFileAttributes(szA) == INVALID_FILE_ATTRIBUTES
			, s_nProgressCalls > 0 ? "called" : "not-called");
		if (bMoved && !bIntact)
			nExit = 1;
		::DeleteFile(szA);
		::DeleteFile(szB);
	}

	// 9. Moving onto a name that is already taken.
	{
		PathIn(szA, _countof(szA), _T("src.tmp"));
		PathIn(szB, _countof(szB), _T("dst.tmp"));
		HANDLE h1 = OpenPartFile(szA, GENERIC_WRITE, CREATE_ALWAYS);
		if (h1 != INVALID_HANDLE_VALUE) {
			WritePattern(h1, 0, 4096, pBuf);
			::CloseHandle(h1);
		}
		HANDLE h2 = OpenPartFile(szB, GENERIC_WRITE, CREATE_ALWAYS);
		if (h2 != INVALID_HANDLE_VALUE)
			::CloseHandle(h2);

		::SetLastError(ERROR_SUCCESS);
		const BOOL bPlain = ::MoveFile(szA, szB);
		const DWORD dwPlainErr = ::GetLastError();
		const BOOL bReplace = bPlain ? TRUE : ::MoveFileEx(szA, szB, MOVEFILE_REPLACE_EXISTING);
		Probe("move-replace", "plain=%d plainerr=%u withflag=%d"
			, bPlain != 0, bPlain ? 0 : dwPlainErr, bReplace != 0);
		::DeleteFile(szA);
		::DeleteFile(szB);
	}

	// 10. The handle-side view eMule uses for its file dates and sizes.
	{
		PathIn(szA, _countof(szA), _T("info.tmp"));
		HANDLE h = OpenPartFile(szA, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
		BY_HANDLE_FILE_INFORMATION fi;
		::ZeroMemory(&fi, sizeof fi);
		bool bOk = false;
		if (h != INVALID_HANDLE_VALUE) {
			WritePattern(h, 0, 12345, pBuf);
			::FlushFileBuffers(h);
			bOk = ::GetFileInformationByHandle(h, &fi) != FALSE;
		}
		const ULONGLONG uSize = ((ULONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
		Probe("getfileinfo", "ok=%d size=%s links=%s index=%s"
			, bOk, uSize == 12345 ? "exact" : "WRONG"
			, fi.nNumberOfLinks == 1 ? "one" : "other"
			, (fi.nFileIndexHigh | fi.nFileIndexLow) ? "set" : "zero");
		Probe("flush", "ok=%d", h != INVALID_HANDLE_VALUE && ::FlushFileBuffers(h));
		if (h != INVALID_HANDLE_VALUE)
			::CloseHandle(h);
		::DeleteFile(szA);
	}

	// 11. A path past MAX_PATH. eMule has a message for this case, which means
	//     it expects the attempt to fail rather than to work.
	{
		TCHAR szLong[1024];
		int nPos = _sntprintf_s(szLong, _countof(szLong), _TRUNCATE, _T("%s\\"), s_szDir);
		while (nPos < 300 && nPos < (int)_countof(szLong) - 2)
			szLong[nPos++] = _T('x');
		szLong[nPos] = 0;
		::SetLastError(ERROR_SUCCESS);
		HANDLE h = OpenPartFile(szLong, GENERIC_WRITE, CREATE_ALWAYS);
		const DWORD dwErr = ::GetLastError();
		Probe("long-path", "created=%d err=%u", h != INVALID_HANDLE_VALUE
			, h != INVALID_HANDLE_VALUE ? 0 : dwErr);
		if (h != INVALID_HANDLE_VALUE) {
			::CloseHandle(h);
			::DeleteFile(szLong);
		}
	}

	delete[] pBuf;
	return nExit;
}

/////////////////////////////////////////////////////////////////////////////
// Mode "lifecycle": the whole life of a part file, over and over
//
// Create it, give it its final length before any of it has arrived, write the
// blocks in the order they would turn up from the network rather than in order,
// flush, close, ask what it takes on disk, move it to its finished name, and
// only then read every byte back.

static int RunLifecycle()
{
	const ULONGLONG uSize = (ULONGLONG)s_opt.nFileMB << 20;
	const DWORD dwBlock = (DWORD)s_opt.nBlockKB * 1024;
	const int nBlocks = (int)(uSize / dwBlock);
	if (nBlocks < 4) {
		Log("the file is too small for that block size");
		return 2;
	}

	BYTE *pBuf = new BYTE[dwBlock];
	int nFailCreate = 0, nFailSize = 0, nFailWrite = 0, nFailFlush = 0;
	int nFailMove = 0, nFailVerify = 0, nFailDelete = 0, nBadOnDisk = 0, nBadLength = 0;

	for (int nRound = 0; nRound < s_opt.nRounds; ++nRound) {
		TCHAR szPart[MAX_PATH], szDone[MAX_PATH], szName[64];
		_sntprintf_s(szName, _countof(szName), _TRUNCATE, _T("%03d.part"), nRound);
		PathIn(szPart, _countof(szPart), szName);
		_sntprintf_s(szName, _countof(szName), _TRUNCATE, _T("%03d.done"), nRound);
		PathIn(szDone, _countof(szDone), szName);

		HANDLE h = OpenPartFile(szPart, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
		if (h == INVALID_HANDLE_VALUE) {
			++nFailCreate;
			continue;
		}

		LARGE_INTEGER liSize;
		liSize.QuadPart = (LONGLONG)uSize;
		if (!::SetFilePointerEx(h, liSize, NULL, FILE_BEGIN) || !::SetEndOfFile(h))
			++nFailSize;

		// Out of order, as the blocks of a download arrive.
		bool bWritten = true;
		for (int i = 0; i < nBlocks; ++i) {
			const int nBlock = (int)(((ULONGLONG)i * 7919 + nRound) % nBlocks);
			if (!WritePattern(h, (ULONGLONG)nBlock * dwBlock, dwBlock, pBuf)) {
				bWritten = false;
				break;
			}
		}
		if (!bWritten)
			++nFailWrite;
		if (!::FlushFileBuffers(h))
			++nFailFlush;
		::CloseHandle(h);

		const ULONGLONG uOnDisk = GetDiskFileSize(szPart);
		if (uOnDisk != uSize)
			++nBadOnDisk;

		if (!::MoveFileWithProgress(szPart, szDone, MoveProgressRoutine, NULL, MOVEFILE_COPY_ALLOWED)) {
			++nFailMove;
			::DeleteFile(szPart);
			continue;
		}

		HANDLE hDone = OpenPartFile(szDone, GENERIC_READ, OPEN_EXISTING);
		if (hDone == INVALID_HANDLE_VALUE)
			++nFailVerify;
		else {
			LARGE_INTEGER liGot;
			liGot.QuadPart = 0;
			if (!::GetFileSizeEx(hDone, &liGot) || (ULONGLONG)liGot.QuadPart != uSize)
				++nBadLength;
			bool bIntact = true;
			for (int i = 0; bIntact && i < nBlocks; ++i)
				bIntact = ReadAndCheck(hDone, (ULONGLONG)i * dwBlock, dwBlock, pBuf);
			if (!bIntact)
				++nFailVerify;
			::CloseHandle(hDone);
		}

		if (!::DeleteFile(szDone))
			++nFailDelete;
	}

	delete[] pBuf;

	Log("lifecycle: %d rounds of %dMB in %dKB blocks, %ld progress callbacks"
		, s_opt.nRounds, s_opt.nFileMB, s_opt.nBlockKB, s_nProgressCalls);
	Probe("lifecycle", "rounds=%d create=%d size=%d write=%d flush=%d ondisk=%d move=%d length=%d verify=%d delete=%d"
		, s_opt.nRounds, nFailCreate, nFailSize, nFailWrite, nFailFlush
		, nBadOnDisk, nFailMove, nBadLength, nFailVerify, nFailDelete);

	const int nTotal = nFailCreate + nFailSize + nFailWrite + nFailFlush
		+ nBadOnDisk + nFailMove + nBadLength + nFailVerify + nFailDelete;
	if (nTotal == 0) {
		Log("VERDICT: %d part files created, filled out of order, moved and read back byte"
			" for byte with nothing amiss", s_opt.nRounds);
		return 0;
	}
	Log("VERDICT: %d failures across %d part files - see the counts above", nTotal, s_opt.nRounds);
	return 1;
}

/////////////////////////////////////////////////////////////////////////////

static void Usage()
{
	printf("filerepro - the life of an eMule part file, on its own\n"
		"\n"
		"  --mode <contract|lifecycle>  what to run (default contract)\n"
		"  --rounds <n>       lifecycle: how many part files (default 20)\n"
		"  --file-mb <n>      lifecycle: size of one part file (default 8)\n"
		"  --block-kb <n>     lifecycle: size of one written block (default 64)\n"
		"  --big-gb <n>       contract: how far out the sparse file goes (default 4)\n"
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
		} else if (!strcmp(p, "--rounds") && bHasVal)
			s_opt.nRounds = atoi(argv[++i]);
		else if (!strcmp(p, "--file-mb") && bHasVal)
			s_opt.nFileMB = atoi(argv[++i]);
		else if (!strcmp(p, "--block-kb") && bHasVal)
			s_opt.nBlockKB = atoi(argv[++i]);
		else if (!strcmp(p, "--big-gb") && bHasVal)
			s_opt.nBigGB = atoi(argv[++i]);
		else {
			Usage();
			return false;
		}
	}
	return s_opt.nRounds > 0 && s_opt.nFileMB > 0 && s_opt.nBlockKB > 0 && s_opt.nBigGB > 0;
}

// Whatever the run left behind, so a failing trial does not fill the disk.
static void CleanWorkDir()
{
	TCHAR szPattern[MAX_PATH];
	PathIn(szPattern, _countof(szPattern), _T("*"));
	WIN32_FIND_DATA fd;
	HANDLE hFind = ::FindFirstFile(szPattern, &fd);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				TCHAR szPath[MAX_PATH];
				PathIn(szPath, _countof(szPath), fd.cFileName);
				::DeleteFile(szPath);
			}
		} while (::FindNextFile(hFind, &fd));
		::FindClose(hFind);
	}
	::RemoveDirectory(s_szDir);
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
	if (!MakeWorkDir()) {
		printf("could not create the working directory, err=%u\n", ::GetLastError());
		return 2;
	}

	Log("start: mode=%s dir=%ls", s_pszModeNames[s_opt.nMode], s_szDir);

	const int nExit = (s_opt.nMode == MODE_CONTRACT) ? RunContract() : RunLifecycle();

	CleanWorkDir();
	Log("exit=%d", nExit);
	::DeleteCriticalSection(&s_csLog);
	return nExit;
}
