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

## Results — 14 September 2026, run 34847674228

One x64 binary, built once on the Windows runner and run on both sides of the
bench in the same CI run.

**The probe table: 25 answers out of 25 identical.** Windows and Wine 9.0 on
Linux x86_64 agree on every one — association and double association, the
out-parameters of a timed-out `GetQueuedCompletionStatus`, verbatim delivery and
FIFO order of posted packets, the four edges around end-of-file, `CancelIo`,
`CloseHandle` with I/O in flight, and every 64-bit offset including the one that
straddles 4 GB.

Under load, nothing lost anywhere:

| | reads completed | packets posted | lost | ledger |
|---|---|---|---|---|
| Windows x64, `strict` | 160,750 | 162,600 | 0 | all zero |
| Windows x64, `emule` | 154,569 | 85,415 | 0 | all zero |
| Windows Win32, `strict` | 130,668 | 132,399 | 0 | all zero |
| Windows Win32, `emule` | 129,781 | 94,872 | 0 | all zero |
| Wine 9.0 x86_64, `strict` | 379,372 | 391,225 | 0 | all zero |
| Wine 9.0 x86_64, `emule` | 451,585 | 18,060 | 0 | all zero |

Roughly 1.4 million completions and 890,000 hand-posted packets, and not one
went missing, arrived twice, carried the wrong completion key, returned the
wrong byte count or returned the wrong bytes. **No difference found.** Which is
a result: the completion port is not where the remaining upload defect lives,
and the shape that was fatal on the listening socket is not fatal here.

Two things worth keeping from the run:

- `read-past-eof` answers a question about eMule rather than about Wine. A
  failing overlapped read **does** queue a completion packet, on both
  implementations: `GetQueuedCompletionStatus` returns FALSE with `lpOverlapped`
  pointing at the request and `GetLastError()` 38 (`ERROR_HANDLE_EOF`). That is
  exactly the packet eMule's inner drain loop drops - its `do..while` ends on
  the first failed dequeue and never processes what it just took off the port,
  leaving the request on `m_listPendingIO` forever. The hazard the `UPDIAG`
  comment in `UploadDiskIOThread.cpp` describes is real, it is eMule's, and it
  is the same on both platforms.
- Wine is the faster of the two here (380-450k reads against 130-160k), which is
  the Linux page cache against the runner's disk, not a property of the port.

### What the first two runs cost, and why they are worth recording

Both earlier runs reported a stall in `strict` mode - on **Windows**, with
`posted == taken`. Neither was a finding; both were this harness parking its own
thread, and the books are what said so:

1. The pool of overlapped slots is something eMule does not have, so a cycle
   could run out of slots and leave requests behind. Fixed by capping the
   producers on queued + outstanding, and counted from then on (`leftover=`).
2. The drain loop swallows wakeups. A packet posted while a cycle is past its
   `StartReads()` and still draining is taken, counted, and does nothing. eMule
   is immune because its work source is the upload list itself - every cycle
   walks all of it - while this harness reads from a queue, where the same
   swallow loses the work.

The second one is why the autopsy was rewritten to weigh the ledger before the
kick: a hand-posted packet resumes a parked thread in *every* one of these
cases, so "the kick helped" can never be the evidence for "a packet was lost".
