# iocprepro — eMule's completion-port loop, on its own

The second specimen of the dual bench, after [`wsrepro/`](../wsrepro). Same
method: take one Win32 subsystem eMule depends on, reproduce eMule's use of it
construct for construct with nothing else around it, and run the same binary on
real Windows and under Wine. Windows is the oracle — the specimen never has to
encode what the correct answer is, only notice that the two implementations
gave different ones.

## Why completion ports, and why now

`wsrepro` ended by naming the shape that broke: a thread parked in an unbounded
wait, woken only by a notification something else must deliver, with a drain
loop that runs until the source says "nothing left". Two of eMule's threads are
built on exactly that shape over an I/O completion port:

- `srchybrid/UploadDiskIOThread.cpp` — reads the file blocks being uploaded
- `srchybrid/PartFileWriteThread.cpp` — writes the blocks being downloaded

Both run the same loop:

```cpp
while (m_Run
    && ::GetQueuedCompletionStatus(m_hPort, &dw, &key, (LPOVERLAPPED*)&pCurIO, INFINITE)
    && key)
{
    ...start new overlapped I/O...
    do { ...take one completion... }
    while (::GetQueuedCompletionStatus(m_hPort, &dw, &key, (LPOVERLAPPED*)&pCurIO, 0));
    if (InterlockedExchange8(&m_bNewData, 0) && m_listPendingIO.IsEmpty())
        PostQueuedCompletionStatus(m_hPort, 0, WAKEUP, NULL);
}
```

One lost packet — a completion, or one of the hand-posted wakeups — parks that
thread forever, and the transfers it serves stop. It is the web-interface
failure again, one subsystem over.

## Modes

| mode | what it does | what a finding looks like |
|---|---|---|
| `contract` | a fixed table of deterministic probes of what the API actually does | a `PROBE` line that differs between Windows and Wine |
| `strict` | stress with exact accounting: one request, one posted packet, both counted | `posted != taken`, or `issued != completed` |
| `emule` | eMule's loop reproduced faithfully, race included | a stall — attributed by the autopsy |
| `bigoff` | reads and writes straddling the 4 GB offset boundary, content verified | wrong bytes, or a block that landed 4 GB lower down |

`contract` is the one that pays fastest. It prints lines with no timestamps, no
pointers and no run-dependent numbers, so the workflow can simply `diff` the
Windows table against the Wine one. A red cross on that step means the bench
found something, not that the bench is broken.

The probes ask, among others:

- what `GetQueuedCompletionStatus` leaves in its out-parameters when it times
  out — eMule reads `completionKey` *after* the drain loop has ended on exactly
  that timeout, and decides whether it was asked to terminate from what it finds
  there;
- whether a `ReadFile` that fails synchronously still queues a packet — which
  decides whether eMule's pending list leaks an entry or double-frees one;
- what `CloseHandle` does to I/O still in flight, which is eMule's documented
  shutdown path ("*Improper termination of asynchronous I/O follows...*");
- whether `CancelIo` produces a packet for every cancelled request — the
  question behind the loop at `EMSocket.cpp:1329`;
- whether association, double association, verbatim delivery of posted packets
  and delivery order behave the same on both.

## Why `emule` and `strict` are separate modes

`CUploadDiskIOThread::WakeUpCall()` reads the I/O thread's state without a lock:

```cpp
if (m_Run == RUN_IDLE && m_listPendingIO.IsEmpty())
    PostQueuedCompletionStatus(m_hPort, 0, WAKEUP_SRC(nDiagSrc), NULL);
else
    InterlockedExchange8(&m_bNewData, 1);
```

and the I/O thread tests and clears `m_bNewData` at the end of its cycle. If the
producer's store lands just after that test, the flag is set, nobody posts, and
the thread parks with work waiting. That window exists on Windows too — so
`emule` mode can stall on a correct platform, and its autopsy says so in as many
words. `strict` removes the race entirely: every request posts its own counted
packet. A shortfall there is the platform, with nothing left to interpret.

## Running it

```
cmake -S iocprepro -B build-iocprepro -A x64
cmake --build build-iocprepro --config Release
build-iocprepro/Release/iocprepro.exe --mode contract
build-iocprepro/Release/iocprepro.exe --mode strict --duration-s 60
```

Exit code 1 means something was found: a stall, a lost packet, or a ledger entry
that cannot happen on a correct implementation. `.github/workflows/build-iocprepro.yml`
builds ARM64/x64/Win32, runs all four modes on the Windows runner, then runs the
same x64 binary under Wine on Linux and diffs the two probe tables.

## Results

Not yet run.
