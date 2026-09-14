# timerepro — the clock eMule runs on, on its own

The fifth specimen of the dual bench. Same method: reproduce eMule's use of one
Win32 subsystem construct for construct, and run the same binary on real Windows
and under Wine. Windows is the oracle.

## The question

Everything eMule does is driven by a timer, and its upload throttler is driven
by a very short one:

```cpp
#define TIME_BETWEEN_UPLOAD_LOOPS 1          // UploadBandwidthThrottler.cpp:573
```

One millisecond. The loop computes how long to wait from the bandwidth it is
allowed, clamps it to that minimum, and waits:

```cpp
DWORD timeSinceLastLoop = timeGetTime() - lastLoopTick;
if (timeSinceLastLoop < sleepTime)
    ::WaitForSingleObject(m_eventSocketAvailable, sleepTime - timeSinceLastLoop);
```

Two assumptions are buried there, and neither belongs to the program:

- that a wait of one millisecond takes about a millisecond;
- that `timeGetTime()` can tell one millisecond from none.

On Windows both depend on whether anybody has raised the system timer
resolution. eMule can — `timeBeginPeriod` at `Emule.cpp:574` — but only if the
`HighresTimer` preference is on, and **its default is `false`**
(`Preferences.cpp:2310`). So the ordinary configuration asks for a
one-millisecond loop from a clock nobody has sharpened.

The second assumption is the sharper one, because eMule's own loop says:

```cpp
} else if (timeSinceLastLoop == 0) {
    // no time has passed, so don't add any bytes. Shouldn't happen.
```

If the clock is coarse enough that loops keep seeing no elapsed time, that
branch stops being the exception its comment calls it, and the throttler spends
nothing on those turns.

## Modes

| mode | what it does | what a finding looks like |
|---|---|---|
| `contract` | the granularity of every clock eMule reads and the real cost of every wait it performs, before and after `timeBeginPeriod` | a `PROBE` line that differs between Windows and Wine |
| `throttle` | eMule's loop with nothing in it but its timing, at 1/2/5/10 ms, with and without the timer raised | a different number of turns per second, or loops that see no elapsed time |

Elapsed time is measured with `QueryPerformanceCounter` throughout — a ruler
made of the thing being measured is no ruler — and the contract mode checks that
ruler against the wall clock before anything else leans on it. Every bucket is
coarse on purpose: the answers being separated differ by a factor of ten, not by
a percent, so two correct runs of the same platform land in the same bucket.

## Running it

```
cmake -S timerepro -B build-timerepro -A x64
cmake --build build-timerepro --config Release
build-timerepro/Release/timerepro.exe --mode contract
build-timerepro/Release/timerepro.exe --mode throttle
```

`.github/workflows/build-timerepro.yml` builds ARM64/x64/Win32, runs both modes
on the Windows runner, then runs the same x64 binary under Wine on Linux and
diffs the two probe tables.

## Results

Not yet run.
