# ovrepro — eMule's overlapped send, on its own

The third specimen of the dual bench, after [`wsrepro/`](../wsrepro) and
[`iocprepro/`](../iocprepro). Same method: reproduce eMule's use of one Win32
subsystem construct for construct, with nothing else around it, and run the same
binary on real Windows and under Wine. Windows is the oracle.

## The question

eMule uploads through overlapped `WSASend`. Each socket keeps one outstanding
send (`srchybrid/EMSocket.cpp`, `CEMSocket::SendOv`) whose `WSAOVERLAPPED`
carries an event **shared by every socket** — the throttler's
`m_eventSocketAvailable` — and a flag, `m_bPendingSendOv`, that stays raised
until somebody collects the completion with `WSAGetOverlappedResult`.

In the field, sockets are found with the completion ready and the flag still
raised: **53 episodes measured, 58% of them ending with the upload slot dead**.
Two candidates, and the running application cannot tell them apart:

**The platform** — a completion that is never reported, the overlapped-send
version of the lost `FD_ACCEPT` edge that `wsrepro` found.

**eMule** — because the only collector, `CEMSocket::IsBusyExtensiveCheck()`, is
reached from exactly one place, `UploadBandwidthThrottler.cpp:393`, and that
place is guarded twice:

```cpp
for (INT_PTR i = nDiagWindow; --i >= 0;) {        // only the active window
    ThrottledFileSocket *pSocket = m_StandardOrder_list[i];
    if (pSocket->HasQueues()) {                   // only with something queued
        ++nCanSend;
        nBusy += static_cast<uint32>(pSocket->IsBusyExtensiveCheck());
    }
```

Every other path — the trickle loop, the equal-for-all loop, the remaining-
bandwidth loop — asks `IsBusyQuickCheck()`, which only reads the raised flag and
skips the socket. So a send that finishes while its queue is empty, on a socket
outside the window, has nobody left to collect it: **the flag that hides it is
the same flag that keeps the collector away.**

## Modes

| mode | what it does | what a finding looks like |
|---|---|---|
| `contract` | deterministic probes of the overlapped-send contract | a `PROBE` line that differs between Windows and Wine |
| `emule` | the throttler's two gates, reproduced | a dead slot — and if it happens on **both** platforms, the defect is eMule's |
| `poll` | every socket with an outstanding send collected every loop | any stuck completion at all, because nothing here can leave one behind |

`emule` is expected to kill slots. That is the experiment: the same result on
Windows and under Wine attributes the defect to eMule's arrangement and
justifies a correction; a difference between the two platforms attributes it to
the implementation instead. `poll` is the control — it is also the shape of the
proposed correction.

Among the contract probes, one matters beyond the comparison: **does `CancelIo`
called from another thread cancel anything?** `CancelIo` cancels I/O issued *by
the calling thread*, and `CEMSocket::CleanUpOverlappedSendOperation` calls it
from whichever thread is tearing the socket down — then waits five times twenty
milliseconds and gives up, freeing the buffers the send may still be using.

## Running it

```
cmake -S ovrepro -B build-ovrepro -A x64
cmake --build build-ovrepro --config Release
build-ovrepro/Release/ovrepro.exe --mode contract
build-ovrepro/Release/ovrepro.exe --mode emule --duration-s 60
build-ovrepro/Release/ovrepro.exe --mode poll --duration-s 60
```

`--slots` upload slots, `--window` how many of them the collector may look at
(the throttler's active window), `--peer-stall-ms` how long the peer stops
reading each second — which is what makes a send go pending in the first place.

Exit code 1 means something was found. `.github/workflows/build-ovrepro.yml`
builds ARM64/x64/Win32, runs all three modes on the Windows runner, then runs
the same x64 binary under Wine on Linux and diffs the two probe tables.

## Results — 14 September 2026, run 34857127764

One x64 binary, built once on the Windows runner and run on both sides of the
bench in the same CI run; the Win32 build ran on the Windows runner as a third
leg.

**The probe table: 9 answers out of 9 identical** on Windows x64, Windows Win32
and Wine 9.0 on Linux x86_64.

Under load, with 8 slots and a collector window of 3:

| | dead slots | episodes | worst spell | short sends | errors |
|---|---|---|---|---|---|
| Windows x64, `emule` | **5 of 8** | 12 | 58,640 ms | 0 | 0 |
| Windows Win32, `emule` | **5 of 8** | 17 | 58,610 ms | 0 | 0 |
| Wine 9.0 x86_64, `emule` | **5 of 8** | 1,277 | 59,976 ms | 0 | 0 |
| Windows x64, `poll` | 0 | 0 | 0 ms | 0 | 0 |
| Windows Win32, `poll` | 0 | 1 | 0 ms | 0 | 0 |
| Wine 9.0 x86_64, `poll` | 0 | 9 | 0 ms | 0 | 0 |

The five dead slots are slots 3 to 7 — every one outside the collector's window
— on all three legs, killed within seconds of the start and never collected
again. The three inside the window recover every time.

**The defect is eMule's.** The same arrangement kills the same slots on real
Windows as under Wine, and the correction that `poll` stands for — collect every
socket that has an outstanding send, whatever its queue and wherever it sits —
leaves none dead anywhere. Nothing in the implementation underneath is involved.

### Two things the run says beyond that

**`CancelIo` from another thread cancels nothing** — `cancelled=no-still-pending`
on both implementations, against `cancelled=YES` from the issuing thread. That is
the documented rule, and `CEMSocket::CleanUpOverlappedSendOperation(true)` is
called from `CEMSocket::~CEMSocket`, while the send it is trying to cancel was
issued by the throttler thread. So the cancel does not take, the loop that
follows spins its five turns of twenty milliseconds, gives up - and the code then
runs `delete[] m_aBufferSend[i].buf` on buffers the system is still sending from.

**Wine never completes an overlapped send synchronously.** On Windows 96% of the
sends completed immediately (`immediate=8021` of 8369); under Wine not one did -
every single send went pending. eMule handles both, so this is not a defect. But
it decides how often the defect above can fire: 1,277 uncollected-completion
episodes under Wine against 12 on Windows, a hundredfold, because a send that
completes synchronously is never left for anyone to collect. It is also why the
cancel-that-does-not-cancel has a window open all the time here and almost never
on Windows.

### What the first run cost

The first attempt reported four differences between Windows and Wine. None was
real: on Windows the 8 MB send never went pending at all, so every probe that
needed an outstanding send was measuring nothing. The cause was `SO_RCVBUF` set
on the accepted socket instead of on the listening one, where it is inherited
from - set too late, receive-window auto-tuning had already swallowed the lot.
Establishing an outstanding send is now a measurement of its own, reported as
`pending=`, and it took three sends of 4 MB on Windows against one under Wine.
