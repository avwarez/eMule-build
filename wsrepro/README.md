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

`emule` reproduces; the other three each remove one candidate cause. A mode
that stops freezing while `emule` freezes has named the mechanism.

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

Throttled to ~90 connections/second with `--delay-ms 20`, which is what the
Windows runner can sustain without exhausting its ephemeral ports - the same
command line on both sides:

| where | mode | trials | froze | wait cycles | empty drains |
|---|---|---|---|---|---|
| Wine 11.0, aarch64 | `emule` | 3 | **3** | - | - |
| Windows runner, x64 | `emule` | 3 | 0 | 11,906 | 2,882 |
| Windows runner, Win32 | `emule` | 3 | 0 | 11,979 | 2,823 |

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

**Only a bounded wait survives.** `timeout` mode is eMule's listener with
`INFINITE` replaced by 1000 ms, and it did not freeze once. It does not prevent
the lost edge; it stops the lost edge from being permanent.

The reading of this is that the pattern eMule uses is legitimate and works on
Windows, and that this platform loses a `FD_ACCEPT` edge in the narrow window
around an `accept()` that returns `WSAEWOULDBLOCK`. The corollary for eMule is
narrower than a fix: an unbounded wait with no way back is what turns someone
else's lost edge into a dead web interface.
