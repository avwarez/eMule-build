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

## Results

Not yet run.
