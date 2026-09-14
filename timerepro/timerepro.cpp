// timerepro.cpp - a standalone specimen for the clock eMule runs on.
//
// WHY THIS EXISTS
// ---------------
// The fifth specimen of the dual bench. Everything eMule does is driven by a
// timer, and its upload throttler is driven by a very short one:
//
//     #define TIME_BETWEEN_UPLOAD_LOOPS 1          UploadBandwidthThrottler.cpp:573
//
// One millisecond. The loop computes how long to wait from the bandwidth it is
// allowed, clamps it to that minimum, and waits:
//
//     DWORD timeSinceLastLoop = timeGetTime() - lastLoopTick;
//     if (timeSinceLastLoop < sleepTime)
//         ::WaitForSingleObject(m_eventSocketAvailable, sleepTime - timeSinceLastLoop);
//
// Two assumptions are buried in there. That a wait of one millisecond takes
// about a millisecond - and that timeGetTime() can tell one millisecond from
// none. Neither is a property of the program: both belong to the platform, and
// on Windows both depend on whether anybody has raised the system timer
// resolution. eMule can raise it, with timeBeginPeriod (Emule.cpp:574) - but
// only if the HighresTimer preference is on, and its default is FALSE
// (Preferences.cpp:2310).
//
// So the ordinary configuration asks for a one-millisecond loop from a clock
// nobody has sharpened. What that actually delivers is what this measures, on
// both implementations.
//
// The second assumption is the sharper one. eMule's own loop reads:
//
//     } else if (timeSinceLastLoop == 0) {
//         // no time has passed, so don't add any bytes. Shouldn't happen.
//
// If the clock is coarse enough that loops keep seeing no elapsed time, that
// branch stops being the exception it is commented as, and the throttler
// spends nothing on those turns.
//
// WHAT IT MEASURES
// ---------------
//   --mode contract  the granularity of every clock eMule reads and the real
//                    cost of every wait it performs, before and after
//                    timeBeginPeriod, as timestamp-free PROBE lines for the
//                    workflow to DIFF between Windows and Wine.
//   --mode throttle  eMule's loop with nothing in it but its timing: how many
//                    turns a second it really gets at each requested period,
//                    and how many of them see no elapsed time at all.
//
// Elapsed time is measured with QueryPerformanceCounter throughout, because a
// ruler made of the thing being measured is no ruler; the contract mode checks
// that ruler against the wall clock before anything else uses it.

#include <afxwin.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "winmm.lib")

CWinApp theApp;

// UploadBandwidthThrottler.cpp:573
#define TIME_BETWEEN_UPLOAD_LOOPS 1

/////////////////////////////////////////////////////////////////////////////
// Options

enum EMode
{
	MODE_CONTRACT = 0,
	MODE_THROTTLE
};

static const char *const s_pszModeNames[] = { "contract", "throttle" };

struct SOptions
{
	int nMode;
	int nSamples;      // how many times each wait is repeated
	int nSecondsEach;  // throttle: seconds per requested period
};

static SOptions s_opt = { MODE_CONTRACT, 200, 3 };

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
	printf("PROBE %-24s ", pszName);
	va_list args;
	va_start(args, pszFmt);
	vprintf(pszFmt, args);
	va_end(args);
	printf("\n");
	fflush(stdout);
	::LeaveCriticalSection(&s_csLog);
}

/////////////////////////////////////////////////////////////////////////////
// The ruler

static LARGE_INTEGER s_liFreq;

static inline LONGLONG Now()
{
	LARGE_INTEGER li;
	::QueryPerformanceCounter(&li);
	return li.QuadPart;
}

static inline double ToUs(LONGLONG nTicks)
{
	return (double)nTicks * 1000000.0 / (double)s_liFreq.QuadPart;
}

// Coarse on purpose. Two correct runs of the same platform must land in the
// same bucket, so the boundaries sit where nothing real is expected to be: the
// answers being separated here differ by a factor of ten, not by a percent.
static const char *Bucket(double dUs)
{
	if (dUs <= 1000.0)
		return "<=1ms";
	if (dUs <= 4000.0)
		return "<=4ms";
	if (dUs <= 10000.0)
		return "<=10ms";
	if (dUs <= 20000.0)
		return "<=20ms";
	return ">20ms";
}

static int CompareDouble(const void *a, const void *b)
{
	const double x = *(const double*)a;
	const double y = *(const double*)b;
	return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

// The median, not the mean: one descheduled sample on a shared machine moves a
// mean into the next bucket and says nothing about the clock.
static double Median(double *pValues, int nCount)
{
	qsort(pValues, (size_t)nCount, sizeof(double), CompareDouble);
	return nCount ? pValues[nCount / 2] : 0.0;
}

/////////////////////////////////////////////////////////////////////////////
// Measurements

// The smallest step the clock can show: watch it until it changes, over and
// over, and keep the smallest change seen.
static double SmallestStepUs(DWORD (WINAPI *pfnClock)(void), int nChanges)
{
	double dSmallest = 1e12;
	for (int i = 0; i < nChanges; ++i) {
		const DWORD dwStart = pfnClock();
		const LONGLONG nT0 = Now();
		DWORD dwNow;
		do {
			dwNow = pfnClock();
		} while (dwNow == dwStart);
		const double dUs = ToUs(Now() - nT0);
		if (dUs < dSmallest)
			dSmallest = dUs;
	}
	return dSmallest;
}

static DWORD WINAPI ClockTimeGetTime(void)
{
	return ::timeGetTime();
}

static DWORD WINAPI ClockGetTickCount(void)
{
	return ::GetTickCount();
}

static double MedianSleepUs(DWORD dwMs, int nSamples, double *pScratch)
{
	for (int i = 0; i < nSamples; ++i) {
		const LONGLONG nT0 = Now();
		::Sleep(dwMs);
		pScratch[i] = ToUs(Now() - nT0);
	}
	return Median(pScratch, nSamples);
}

// What the throttler actually calls: a wait on an event nobody is going to set.
static double MedianWaitUs(HANDLE hEvent, DWORD dwMs, int nSamples, double *pScratch)
{
	for (int i = 0; i < nSamples; ++i) {
		const LONGLONG nT0 = Now();
		::WaitForSingleObject(hEvent, dwMs);
		pScratch[i] = ToUs(Now() - nT0);
	}
	return Median(pScratch, nSamples);
}

/////////////////////////////////////////////////////////////////////////////
// Mode "contract"

static const char *Agreement(double dRatio)
{
	const double dOff = (dRatio > 1.0 ? dRatio - 1.0 : 1.0 - dRatio) * 100.0;
	if (dOff <= 2.0)
		return "within-2pct";
	if (dOff <= 10.0)
		return "within-10pct";
	return "OFF";
}

static int RunContract()
{
	double *pScratch = new double[s_opt.nSamples];
	HANDLE hEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);   // never set, as the throttler's usually is not

	// 0. The ruler, before anything leans on it. QueryPerformanceCounter is
	//    checked against the wall clock, which is the one clock in the process
	//    that cannot have been sharpened by anybody.
	{
		FILETIME ft0, ft1;
		::GetSystemTimeAsFileTime(&ft0);
		const LONGLONG nQ0 = Now();
		::Sleep(2000);
		::GetSystemTimeAsFileTime(&ft1);
		const LONGLONG nQ1 = Now();
		const ULONGLONG u0 = ((ULONGLONG)ft0.dwHighDateTime << 32) | ft0.dwLowDateTime;
		const ULONGLONG u1 = ((ULONGLONG)ft1.dwHighDateTime << 32) | ft1.dwLowDateTime;
		const double dWallUs = (double)(u1 - u0) / 10.0;
		const double dQpcUs = ToUs(nQ1 - nQ0);
		Probe("qpc-ruler", "freq=%s wallclock=%s"
			, s_liFreq.QuadPart >= 1000000 ? "at-least-1mhz" : "SLOW"
			, dWallUs > 0 ? Agreement(dQpcUs / dWallUs) : "n/a");
		Log("note: qpc freq=%I64d, 2s measured as %.1fms by qpc and %.1fms by the wall clock"
			, s_liFreq.QuadPart, dQpcUs / 1000.0, dWallUs / 1000.0);
	}

	// 1. Never goes backwards.
	{
		LONGLONG nPrev = Now();
		bool bMonotonic = true;
		for (int i = 0; i < 1000000 && bMonotonic; ++i) {
			const LONGLONG nNow = Now();
			if (nNow < nPrev)
				bMonotonic = false;
			nPrev = nNow;
		}
		Probe("qpc-monotonic", "ok=%d", bMonotonic);
	}

	// 2. What the multimedia timer says it can do - the numbers eMule reads at
	//    Emule.cpp:571 before deciding what to ask for.
	{
		TIMECAPS tc;
		::ZeroMemory(&tc, sizeof tc);
		const MMRESULT mm = ::timeGetDevCaps(&tc, sizeof(TIMECAPS));
		Probe("devcaps", "ok=%d min=%u max=%u", mm == TIMERR_NOERROR, tc.wPeriodMin, tc.wPeriodMax);
	}

	// 3 and 4. The state eMule actually runs in here: HighresTimer defaults to
	//    false, so nothing has been raised.
	{
		Probe("step-timegettime-default", "%s", Bucket(SmallestStepUs(ClockTimeGetTime, 20)));
		Probe("step-gettickcount-default", "%s", Bucket(SmallestStepUs(ClockGetTickCount, 20)));
		const double dSleep0 = MedianSleepUs(0, s_opt.nSamples, pScratch);
		const double dSleep1 = MedianSleepUs(1, s_opt.nSamples, pScratch);
		const double dWait1 = MedianWaitUs(hEvent, 1, s_opt.nSamples, pScratch);
		const double dWait5 = MedianWaitUs(hEvent, 5, s_opt.nSamples, pScratch);
		Probe("sleep0-default", "%s", Bucket(dSleep0));
		Probe("sleep1-default", "%s", Bucket(dSleep1));
		Probe("wait1-default", "%s", Bucket(dWait1));
		Probe("wait5-default", "%s", Bucket(dWait5));
		Log("note: default medians - sleep(0) %.0fus, sleep(1) %.0fus, wait(1) %.0fus, wait(5) %.0fus"
			, dSleep0, dSleep1, dWait1, dWait5);
	}

	// 5. And the state eMule runs in when the preference is turned on.
	{
		TIMECAPS tc;
		::ZeroMemory(&tc, sizeof tc);
		UINT wRes = 1;
		if (::timeGetDevCaps(&tc, sizeof(TIMECAPS)) == TIMERR_NOERROR)
			wRes = min(max(tc.wPeriodMin, 1u), tc.wPeriodMax);   // Emule.cpp:572, verbatim
		const MMRESULT mm = ::timeBeginPeriod(wRes);
		Probe("begin-period", "asked=%u ok=%d", wRes, mm == TIMERR_NOERROR);

		const double dStep = SmallestStepUs(ClockTimeGetTime, 20);
		const double dSleep1 = MedianSleepUs(1, s_opt.nSamples, pScratch);
		const double dWait1 = MedianWaitUs(hEvent, 1, s_opt.nSamples, pScratch);
		Probe("step-timegettime-highres", "%s", Bucket(dStep));
		Probe("sleep1-highres", "%s", Bucket(dSleep1));
		Probe("wait1-highres", "%s", Bucket(dWait1));
		Log("note: highres medians - step %.0fus, sleep(1) %.0fus, wait(1) %.0fus", dStep, dSleep1, dWait1);

		const MMRESULT mmEnd = ::timeEndPeriod(wRes);
		Probe("end-period", "ok=%d", mmEnd == TIMERR_NOERROR);
	}

	// 6. The three clocks eMule reads, measured against each other over the
	//    same two seconds. A drift between them would put its rate arithmetic
	//    out by whatever the drift is.
	{
		const DWORD dwT0 = ::timeGetTime();
		const DWORD dwG0 = ::GetTickCount();
		const LONGLONG nQ0 = Now();
		::Sleep(2000);
		const DWORD dwT1 = ::timeGetTime();
		const DWORD dwG1 = ::GetTickCount();
		const LONGLONG nQ1 = Now();
		const double dQpcMs = ToUs(nQ1 - nQ0) / 1000.0;
		Probe("clock-agreement", "timegettime=%s gettickcount=%s"
			, dQpcMs > 0 ? Agreement((double)(dwT1 - dwT0) / dQpcMs) : "n/a"
			, dQpcMs > 0 ? Agreement((double)(dwG1 - dwG0) / dQpcMs) : "n/a");
	}

	// 7. Local time, system time and the zone that is supposed to separate
	//    them. eMule stamps everything it logs and shares with these.
	{
		SYSTEMTIME stLocal, stUtc;
		TIME_ZONE_INFORMATION tzi;
		::GetLocalTime(&stLocal);
		::GetSystemTime(&stUtc);
		const DWORD dwZone = ::GetTimeZoneInformation(&tzi);
		FILETIME ftLocal, ftUtc;
		const bool bConv = ::SystemTimeToFileTime(&stLocal, &ftLocal) && ::SystemTimeToFileTime(&stUtc, &ftUtc);
		LONGLONG nDiffMin = 0;
		if (bConv) {
			const LONGLONG nL = ((LONGLONG)ftLocal.dwHighDateTime << 32) | ftLocal.dwLowDateTime;
			const LONGLONG nU = ((LONGLONG)ftUtc.dwHighDateTime << 32) | ftUtc.dwLowDateTime;
			nDiffMin = (nL - nU) / (10LL * 1000 * 1000 * 60);
		}
		const LONG nExpected = -(tzi.Bias + (dwZone == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : 0));
		Probe("localtime", "zone-known=%d matches-bias=%d"
			, dwZone != TIME_ZONE_ID_INVALID
			, bConv && (nDiffMin - nExpected <= 1 && nExpected - nDiffMin <= 1));
	}

	::CloseHandle(hEvent);
	delete[] pScratch;
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Mode "throttle": eMule's loop with nothing in it but its timing

static const char *RateBucket(double dLoopsPerSec)
{
	if (dLoopsPerSec >= 500.0)
		return "at-least-500";
	if (dLoopsPerSec >= 200.0)
		return "at-least-200";
	if (dLoopsPerSec >= 100.0)
		return "at-least-100";
	if (dLoopsPerSec >= 50.0)
		return "at-least-50";
	return "under-50";
}

static const char *ShareBucket(double dPercent)
{
	if (dPercent < 1.0)
		return "none";
	if (dPercent < 10.0)
		return "under-10pct";
	if (dPercent < 50.0)
		return "under-50pct";
	return "most-of-them";
}

// UploadBandwidthThrottler::RunInternal, with the sockets and the bandwidth
// arithmetic taken out and only the timing left in.
static void ThrottleRound(HANDLE hEvent, DWORD dwRequestedMs, const char *pszLabel)
{
	DWORD sleepTime = dwRequestedMs;
	if (sleepTime < TIME_BETWEEN_UPLOAD_LOOPS)
		sleepTime = TIME_BETWEEN_UPLOAD_LOOPS;

	DWORD lastLoopTick = ::timeGetTime();
	LONGLONG nLoops = 0, nZeroElapsed = 0;
	const LONGLONG nStart = Now();
	const LONGLONG nEnd = nStart + (LONGLONG)s_opt.nSecondsEach * s_liFreq.QuadPart;

	while (Now() < nEnd) {
		const DWORD timeSinceLastLoop = ::timeGetTime() - lastLoopTick;
		if (timeSinceLastLoop < sleepTime)
			::WaitForSingleObject(hEvent, sleepTime - timeSinceLastLoop);

		const DWORD thisLoopTick = ::timeGetTime();
		// eMule reads the gap here and, when it is zero, takes the branch its
		// own comment calls "Shouldn't happen" - and spends no bytes.
		if (thisLoopTick - lastLoopTick == 0)
			++nZeroElapsed;
		lastLoopTick = thisLoopTick;
		++nLoops;
	}

	const double dSeconds = ToUs(Now() - nStart) / 1000000.0;
	const double dRate = dSeconds > 0 ? (double)nLoops / dSeconds : 0.0;
	const double dZero = nLoops ? (double)nZeroElapsed * 100.0 / (double)nLoops : 0.0;

	char szName[64];
	sprintf(szName, "loops-%ums-%s", dwRequestedMs, pszLabel);
	Probe(szName, "rate=%s asked=%u noelapsed=%s", RateBucket(dRate), 1000 / dwRequestedMs, ShareBucket(dZero));
	Log("note: %s - %I64d loops in %.2fs = %.0f/s (asked %u/s), %I64d with no elapsed time (%.1f%%)"
		, szName, nLoops, dSeconds, dRate, 1000 / dwRequestedMs, nZeroElapsed, dZero);
}

static int RunThrottle()
{
	HANDLE hEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	static const DWORD adwPeriods[] = { 1, 2, 5, 10 };

	// First as eMule runs by default: nothing has raised the timer.
	for (int i = 0; i < (int)_countof(adwPeriods); ++i)
		ThrottleRound(hEvent, adwPeriods[i], "default");

	TIMECAPS tc;
	::ZeroMemory(&tc, sizeof tc);
	UINT wRes = 1;
	if (::timeGetDevCaps(&tc, sizeof(TIMECAPS)) == TIMERR_NOERROR)
		wRes = min(max(tc.wPeriodMin, 1u), tc.wPeriodMax);
	const bool bRaised = (::timeBeginPeriod(wRes) == TIMERR_NOERROR);
	Log("note: timeBeginPeriod(%u) %s", wRes, bRaised ? "succeeded" : "FAILED");

	// Then as it runs with HighresTimer turned on.
	for (int i = 0; i < (int)_countof(adwPeriods); ++i)
		ThrottleRound(hEvent, adwPeriods[i], "highres");

	if (bRaised)
		::timeEndPeriod(wRes);
	::CloseHandle(hEvent);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////

static void Usage()
{
	printf("timerepro - the clock eMule runs on, on its own\n"
		"\n"
		"  --mode <contract|throttle>  what to run (default contract)\n"
		"  --samples <n>       how many times each wait is repeated (default 200)\n"
		"  --seconds-each <n>  throttle: seconds per requested period (default 3)\n"
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
		} else if (!strcmp(p, "--samples") && bHasVal)
			s_opt.nSamples = atoi(argv[++i]);
		else if (!strcmp(p, "--seconds-each") && bHasVal)
			s_opt.nSecondsEach = atoi(argv[++i]);
		else {
			Usage();
			return false;
		}
	}
	return s_opt.nSamples > 0 && s_opt.nSecondsEach > 0;
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
	if (!::QueryPerformanceFrequency(&s_liFreq) || s_liFreq.QuadPart <= 0) {
		printf("no performance counter to measure with\n");
		return 2;
	}

	Log("start: mode=%s samples=%d", s_pszModeNames[s_opt.nMode], s_opt.nSamples);

	const int nExit = (s_opt.nMode == MODE_CONTRACT) ? RunContract() : RunThrottle();

	Log("exit=%d", nExit);
	::DeleteCriticalSection(&s_csLog);
	return nExit;
}
