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
