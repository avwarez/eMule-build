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

## Results — 14 September 2026, run 34865770885

One x64 binary on both sides of the bench, plus the ARM64 build run on the
machine the whole investigation started from (Raspberry Pi, Wine 11 aarch64).

Medians, measured with `QueryPerformanceCounter` (10 MHz on every leg, within
2% of the wall clock, monotonic over a million reads):

| | `Sleep(1)` | `WaitForSingleObject(h,1)` | loops/s at a 1 ms request |
|---|---|---|---|
| Windows x64, default | **13.59 ms** | **13.61 ms** | **104** |
| Windows x64, after `timeBeginPeriod(1)` | 1.65 ms | 1.34 ms | 635 |
| Wine 9.0 x86_64, default | 1.07 ms | 1.11 ms | **901** |
| Wine 9.0 x86_64, after `timeBeginPeriod(1)` | 1.07 ms | 1.11 ms | 905 |
| Wine 11 aarch64 (the real machine), default | 1.06 ms | 1.08 ms | **912** |
| Wine 11 aarch64, after `timeBeginPeriod(1)` | 1.06 ms | 1.10 ms | 906 |

**Wine is already where `timeBeginPeriod` would take it.** Its numbers do not
move when the timer is raised, because there is nothing to raise: a wait of one
millisecond costs one millisecond by default. Windows, left alone, costs 13.6.

For eMule's upload throttler, which asks for a one-millisecond loop from a clock
the default preference never sharpens, that is 104 turns a second on stock
Windows against 900 under Wine — **the emulation is nearly nine times better
than the platform it emulates**, and the real machine matches the runner.

`noelapsed=none` on every leg: the `// no time has passed... Shouldn't happen`
branch at the top of the throttler never fired once, on either implementation.

**So the timer is not behind anything.** Two suspicions die here: that the loop
is starved of turns on this machine, and that turning `HighresTimer` on would
help. It would change nothing — Wine is already there.

### The other differences, and one the buckets hid

`devcaps` reports `max=65535` under Wine against `max=1000000` on Windows. It
does not matter: eMule computes `min(max(tc.wPeriodMin, 1), tc.wPeriodMax)`,
which is 1 either way.

`step-gettickcount-default` lands in three different buckets across the three
legs (`<=20ms` Windows, `<=4ms` Wine 9.0, `<=10ms` Wine 11 aarch64). eMule's
throttler reads `timeGetTime`, not `GetTickCount`, so this only touches the
coarser timekeeping elsewhere in the program.

And one worth admitting: `wait5-default` shows as *identical* — both land in
`<=10ms` — while the underlying medians are 9.57 ms on Windows and 5.13 ms under
Wine, nearly a factor of two apart. The buckets are coarse on purpose, so that
two correct runs of the same platform never disagree; the price is that they
hide differences smaller than the ones they were built to catch. The raw medians
are printed as `note:` lines for exactly this reason.
