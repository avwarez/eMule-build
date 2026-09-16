# wsrepro - a reproducer for the web-interface listener freeze

## The question it answers

eMule's web interface stops answering after some hours of uptime. The
observed state, measured on the running process, is precise: connections
complete their TCP handshake and sit in the kernel backlog, the process issues
no `accept()` at all, and everything else in eMule keeps running normally.

The listening thread in `srchybrid/WebSocket.cpp` waits on an auto-reset event
armed with `WSAEventSelect(hSocket, hEvent, FD_ACCEPT)` and drains with
`accept()` until `WSAEWOULDBLOCK`. It never calls `WSAEnumNetworkEvents` - the
connection loop thirty lines higher up in the same file does. `FD_ACCEPT` is an
edge: Winsock disables the notification when it delivers it, and only `accept()`
re-enables it. So a single lost edge closes a circle that nothing can reopen -
the event is never signalled, the wait never returns, `accept()` is never
called, and the notification is never re-armed.

What cannot be settled by reading the source is **whose** defect that is: the
pattern eMule uses, or the Winsock implementation underneath it (the machine
where the fault shows runs Wine). This program settles it by running eMule's
listener, and nothing else, on the platform in question.

## What it is

A single translation unit that reproduces the listener call for call - same
`WSASocket`, same `listen(SOMAXCONN)`, same `CreateEvent(NULL, FALSE, TRUE, NULL)`,
same `WSAEventSelect(FD_ACCEPT)`, same `WaitForMultipleObjects(..., INFINITE)`,
same inner drain loop, same `new CWinThread(...)` per accepted connection - plus
client threads to hammer it and a monitor that watches for the field symptom.

It is built with MSVC and static MFC, like eMule, so the constructs under test
are the real ones and not an approximation of them.

## Modes

| `--mode` | listener variant | what it tests |
|---|---|---|
| `emule` | eMule's code, unchanged | does the pattern freeze on this platform? |
| `enum` | adds `WSAEnumNetworkEvents` after the wait | is the missing call the cause? |
| `clearinherit` | clears the event association the accepted socket inherits | is the inheritance the cause? |
| `timeout` | wait with a 1000 ms timeout instead of `INFINITE` | does a bounded wait mask it? |
| `asyncselect` | `WSAAsyncSelect` + a helper window and a message loop | is the OTHER notification path in eMule affected too? |
| `asynccounter` | the same path, with `CListenSocket`'s counter-driven accept | does eMule's MAIN listener have the fault, or only the web one? |
| `poll` | no notification at all: level-triggered `select()` | does the readiness model that cannot lose an edge survive here? |
| `fdwrite` | `send()` until `WSAEWOULDBLOCK`, then wait for `FD_WRITE` | is it `accept()` specifically, or any re-enabling call that fails? |
| `fdread` | `recv()` drained to `WSAEWOULDBLOCK`, then wait for `FD_READ` | the third and last drain shape in eMule |
| `iocp` | `AcceptEx` on a completion port | does the model Microsoft recommends behave the same on both implementations? |

`emule` reproduces; `enum` and `clearinherit` each remove one candidate cause;
`timeout` tests the only remedy that is under eMule's control. `asyncselect`
is the blast-radius question: `WSAEventSelect` appears in exactly two places in
eMule, both in `WebSocket.cpp`, while every other socket in the program - the
main listener on the eD2K port included - is notified through `WSAAsyncSelect`
and a hidden helper window. If that path loses notifications too, the exposure
is the whole program rather than the web interface. A mode
that stops freezing while `emule` freezes has named the mechanism.

## The `iocp` mode

The other nine modes all test one family: **readiness**. `WSAAsyncSelect` and
`WSAEventSelect` announce that a call may now be made; the announcement is an
edge, delivered once, disabled by its own delivery, and re-enabled only by a
call that fails. Every fault this bench has found lives in that shape.

`iocp` is the other family: **completion**. `AcceptEx` is issued in advance, the
kernel performs the accept, and the packet that arrives carries the result. No
edge, nothing to re-arm, no failing call in the loop - so the fault the other
modes hunt cannot exist here by construction, and the question becomes a
different one.

What it measures instead is the shape patch `97i` was written for.
`GetQueuedCompletionStatus` has three outcomes, not two:

| outcome | meaning |
|---|---|
| `TRUE`, packet | the operation completed |
| `FALSE`, packet | the operation **failed**; the packet and the socket are still the caller's to dispose of |
| `FALSE`, no packet | nothing was dequeued |

They are counted separately (`compok`, `compfail`, `compnone`) and tied together
by one identity, printed as `residual` on the closing `IOCP:` line:

```
posted == compok + compfail + postfail + inflight
```

It must hold at every instant on any conforming implementation. A non-zero
`residual` is a completion packet that was never delivered - the completion-model
equivalent of a lost edge, on the model that is not supposed to have any.

The autopsy for this mode cannot set an event or post a message, because there
is neither. It asks each outstanding `AcceptEx` directly, with
`WSAGetOverlappedResult`, which bypasses the port entirely:

- some have **already finished** while the listener was still parked -> the
  packets never reached the port: **lost completion**;
- all **still pending** while connections wait in the backlog -> `AcceptEx`
  itself is not taking them, which is a different defect;
- none in flight at all -> the harness ran out of posted accepts, not a
  platform finding. Re-run with a larger `--iocp-accepts`.

Two further things are deliberately part of the comparison rather than
smoothed over:

- `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` is **not** set, so an `AcceptEx` that
  returns `TRUE` immediately must still queue a packet. `syncok` counts those,
  and whether the two implementations agree about them is a finding either way.
- `SO_UPDATE_ACCEPT_CONTEXT` is applied to every accepted socket and its
  refusals are counted in `updfail`, rather than being ignored the way most
  code ignores them.

Extra switches: `--iocp-accepts <n>` (outstanding operations, default 8,
max 64) and `--iocp-threads <n>` (threads pulling from the port, default 1,
max 8). Sharing one port between several threads is the one thing this model
can do that no other mode here can.

## The autopsy

When the monitor sees the symptom - no `accept()` for `--stall-ms` while
clients are connected and waiting - it stops the run and asks the two questions
the field evidence could not separate:

1. Is the listening thread alive? (`WaitForSingleObject(m_hThread, 0)`)
2. If alive, does a bare `SetEvent` on its own event resume accepting?

A yes to both is a lost notification, demonstrated rather than inferred: the
thread was parked in the wait with a full backlog, and one signal emptied it.

## Running it

```
wsrepro.exe --mode emule --port 4711 --hammers 16 --duration-s 300
```

Exit code `0` = no freeze observed, `1` = freeze observed, `2` = could not
start. Pick a port nothing else is using; the default matches eMule's only for
familiarity.

## Building

Its own CMake project, on purpose: a test case must not be able to break the
application build.

```
cmake -S wsrepro -B build-wsrepro -A ARM64
cmake --build build-wsrepro --config Release
```

The `Build wsrepro (listener reproducer)` workflow does exactly that for
ARM64, x64 and Win32 on every push that touches this directory, and leaves the
exe as a run artifact.

## The bench

Two halves, and until 2026-09-14 only one of them existed. The workflow builds
the exe on a Windows runner and runs it there; a second job then downloads
**that same binary** and runs it under Wine on Linux, with the architecture
held constant. Before that, every comparison was x64-on-Windows against
ARM64-on-Wine, which varies the operating system and the architecture at once -
so a disagreement could always have been the architecture. Now it cannot be.

The Wine on the Linux runner is 9.0 (whatever the image ships); the machine the
fault was found on runs 11.0. That is a second axis, and a useful one.

## What it found (2026-09-14)

Same source, same compiler, same static MFC. One machine runs Windows (the CI
runner), the other runs Wine 11.0 on aarch64 - the machine where the field
fault appears. Two clients in every run.

Unthrottled (clients connecting back to back, roughly 500/s):

| where | mode | trials | froze |
|---|---|---|---|
| Wine 11.0, aarch64 | `emule` | 10 | **10** |
| Wine 11.0, aarch64 | `enum` | 10 | **10** |
| Wine 11.0, aarch64 | `clearinherit` | 10 | **10** |
| Wine 11.0, aarch64 | `timeout` | 10 | 0 |
| Wine 11.0, aarch64 | `asyncselect` | 10 | **10** |
| Wine 11.0, aarch64 | `poll` | 10 | 0 |

Throttled to ~90 connections/second with `--delay-ms 20`, which is what the
Windows runner can sustain without exhausting its ephemeral ports - the same
command line on both sides:

| where | mode | trials | froze | wait cycles | empty drains |
|---|---|---|---|---|---|
| Wine 11.0, aarch64 | `emule` | 3 | **3** | - | - |
| Windows runner, x64 | `emule` | 3 | 0 | 11,906 | 2,882 |
| Windows runner, Win32 | `emule` | 3 | 0 | 11,979 | 2,823 |
| Windows runner, x64 | `asyncselect` | 3 | 0 | 11,943 | 3,105 |

And the decisive one - one binary, one CI run, two operating systems:

| same x64 binary, run 34834... | mode | trials | froze |
|---|---|---|---|
| Windows runner | `emule` | 3 | 0 (11,827 cycles, 3,137 empty drains) |
| Wine 9.0 on Linux x86_64 | `emule` | 3 | **3** (after 8, 212 and 468 connections) |
| Windows runner | `asyncselect` | 3 | 0 |
| Wine 9.0 on Linux x86_64 | `asyncselect` | 3 | **3** |
| Windows runner | `poll` | 3 | 0 |
| Wine 9.0 on Linux x86_64 | `poll` | 3 | 0 (15,814 accepted, 0 missed readiness) |

So the fault is not specific to the ARM64 port of Wine, and not specific to
Wine 11: the same loop dies on Wine 9.0 on x86_64 with the same autopsy - the
thread parked, the backlog full, one `SetEvent` emptying it. The architecture
is out of the picture entirely, since both sides of that table are the same
x64 executable.
| Windows runner, Win32 | `asyncselect` | 3 | 0 | 11,798 | 2,743 |

The empty-drain column matters: it counts the wakeups where `accept()` found
nothing and returned `WSAEWOULDBLOCK`, which is the window the fault lives in.
Windows went through that window about 5,700 times without losing a
notification. Wine did not survive 200.

Under Wine the freeze arrives after **6 to 194 connections** - seconds, not
hours. The autopsy is the same every time: the listening thread is alive,
parked in `WaitForMultipleObjects`, with connections waiting in the backlog,
and a single `SetEvent` on its own event resumes accepting immediately. The
`FD_ACCEPT` edge was never delivered.

Three things follow.

**The trigger is the empty drain.** The freeze only happens when the listener
reaches `accept()` returning `WSAEWOULDBLOCK` and a connection arrives around
that moment. Saturate the port (8 or more clients with no pause) and the
listener never leaves the inner drain loop - 10,500 connections, two wakeups,
no freeze. Slow the clients down so the listener parks between connections and
it dies within seconds. That is why the fault looks like it needs hours of
uptime: it needs an idle-ish port, which is the normal state of a web
interface nobody is using.

**`WSAEnumNetworkEvents` is not the answer.** The `enum` mode adds the call
eMule omits - the documented pattern, and the one eMule itself uses for
connections in the same file - and froze 10 times out of 10. So the omission,
real as it is, is not what breaks here: the notification is lost below the
level any of these calls can see. The same goes for the inherited event
association (`clearinherit`, 10 out of 10).

**Both notification paths lose it.** `asyncselect` replaces the event with what
every other socket in eMule uses - `WSAAsyncSelect`, a hidden helper window and
a message loop - and froze 10 times out of 10. The autopsy there posts the
`FD_ACCEPT` message by hand, which is precisely what
`CListenSocket::ReStartListening()` already does when it calls `OnAccept(0)`
itself, and the backlog empties immediately. So this is not a property of
`WSAEventSelect`: the notification is lost below both of them.

**The defect is one call: an `accept()` that fails with `WSAEWOULDBLOCK`.**
`--one-shot` takes exactly one connection per notification and leaves - never
issuing a failing accept, which is what `CListenSocket::OnAccept` does in eMule.
It does not freeze, on either notification path:

| variant | failing `accept()` calls | trials | froze |
|---|---|---|---|
| `emule` (drains to WSAEWOULDBLOCK) | thousands | 10 | **10** |
| `asyncselect` (drains) | thousands | 10 | **10** |
| `emule --one-shot` | 1 (the initial spurious wakeup) | 10 | 0 |
| `asyncselect --one-shot` | 0 | 10 | 0 |
| `poll` (drains) | thousands | 10 | 0 |

The last row is what pins it down. `poll` drains to `WSAEWOULDBLOCK` like the
first two and still never freezes - because `select()` never consults the
notification state that the failing `accept()` corrupts. So the failing call is
the one that does the damage, and the notification APIs are its victims, not
its cause: neither `WSAEventSelect`, nor `WSAAsyncSelect`, nor the wait, nor the
message loop behaves incorrectly anywhere in this.

**The main eD2K listener is not exposed, and that is measured now rather than
inferred.** `--one-shot` approximates `CListenSocket::OnAccept` by taking one
connection per notification; `asynccounter` *is* it - the
`m_nPendingConnections` counter, its `WSAEWOULDBLOCK` branch with eMule's own
log line, and the accepted sockets left on the listener's own helper window with
`WSAAsyncSelect(FD_READ|FD_WRITE|FD_CLOSE)`, which is what `OnAccept`'s last line
does and what no other mode here models. Run 35033496097, one x64 binary, three
trials of 60 s per leg:

| leg | accepted | notifications | `desync` | `pendmax` | empty accepts | froze |
|---|---|---|---|---|---|---|
| Windows x64 | 4106 / 4240 / 4029 | identical | 0 | 1 | 0 | 0/3 |
| Windows Win32 | 4029 / 4019 / 4033 | identical | 0 | 1 | 0 | 0/3 |
| Wine (same x64 binary) | 5668 / 5666 / 5662 | identical | 0 | 1 | 0 | 0/3 |

41,452 accepts across the three legs and **not one notification unaccounted
for**: `accepted == notifications` exactly, every trial, both platforms. That
equality is the whole test. `FD_ACCEPT` is re-enabled by `accept()` and by
nothing else, so a connection that arrives while the notification is disabled
has to be announced the instant the accept that takes its predecessor re-enables
it; one lost edge and the two counters separate for good.

The control ran in the same job: `emule` froze 3/3 under Wine and `asyncselect`
froze 3/3, so the fault was live while `asynccounter` stayed clean. And the
mechanism shows in one column - `empty accepts` is 0 for the counter on every
leg, against 523 to 1068 per trial for the two drain modes. The counter never
asks for a connection that is not there, so it never enters the window the fault
lives in. Which is also why eMule's "Backlog counter says ..." line has never
appeared in 551,788 lines of field log across 244 files: not luck, structure.

Wine accepted about 37% more connections than Windows in the same 60 seconds
(5,665 against 4,125 on average), which is the `timerepro` result showing up
again on a different bench - short waits cost less there.

**It is `accept()` specifically, not "any failing re-enabling call".** That was
the obvious generalisation and it is wrong. `fdwrite` mode reproduces the other
place where a call has to fail in order to re-arm a notification - `send()`
returning `WSAEWOULDBLOCK` to re-enable `FD_WRITE`, which is how
`CEMSocket::SendStd` works (EMSocket.cpp:646-657, and eMule's own comment there
says so: *"Send() blocked, onsend will be called when ready to send again"*).
`fdread` mode does the same for the read side - `recv()` drained to
`WSAEWOULDBLOCK` to re-arm `FD_READ`, which is the other drain loop in
`WebSocket.cpp`.

| re-enabling call that fails | trials | failing calls | notifications lost | froze |
|---|---|---|---|---|
| `accept()` → `FD_ACCEPT` | 10 | 6-194 before dying | ~1 per 100 | **10** |
| `send()` → `FD_WRITE` | 6 | 667,419 | **0** | 0 |
| `recv()` → `FD_READ` | 6 | 795,780 | **0** | 0 |

That asymmetry has a plausible reading: for `FD_WRITE` a failing call is the
*documented, only* way to re-arm, so it is the path everything exercises; for
`FD_ACCEPT` the re-arming call is a *successful* `accept()`, and the failing one
is a corner nothing normally leans on.

**Level-triggered readiness is immune.** `poll` mode throws the notification
away entirely and asks `select()` whether a connection is waiting right now.
Ten trials under Wine, **145,575 connections accepted, zero freezes**. The
1-second timeout in that loop never rescued anything and the harness proves it
rather than assuming it: it counts every `select()` that reported nothing while
clients were known to be waiting, and that counter finished at **0** in all ten
trials (and in the six Windows control trials). There is no edge to lose, so
there is nothing to recover from.

**Only a bounded wait survives, among the notification-based variants.** `timeout` mode is eMule's listener with
`INFINITE` replaced by 1000 ms, and it did not freeze once. It does not prevent
the lost edge; it stops the lost edge from being permanent.

The reading of this is that the pattern eMule uses is legitimate and works on
Windows, and that this platform loses a `FD_ACCEPT` edge in the narrow window
around an `accept()` that returns `WSAEWOULDBLOCK`. The corollary for eMule is
narrower than a fix: an unbounded wait with no way back is what turns someone
else's lost edge into a dead web interface.
