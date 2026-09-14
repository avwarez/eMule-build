# dnsrepro — asynchronous name resolution, and the two UDP sockets

The sixth and last specimen of the dual bench. It closes the one suspicion left
on the map of what eMule asks of Win32.

## The question

eMule resolves a server's name like this:

```cpp
pDNSReq->m_hDNSTask = WSAAsyncGetHostByName(m_hWndResolveMessage, WM_DNSLOOKUPDONE
    , pszHostAddressA, pDNSReq->m_DnsHostBuffer, sizeof pDNSReq->m_DnsHostBuffer);
                                                          // UDPSocket.cpp:794
```

The answer does not come back from the call. It comes back as a **window
message**, with the error in the high word of `lParam` — the same notification
machinery that lost `FD_ACCEPT` edges under Wine (see [`wsrepro/`](../wsrepro)).
A message that never arrives leaves the request on eMule's list for good, and
the packets queued behind it are never sent.

And the answer is written into a buffer owned by the request object, whose
destructor is:

```cpp
~SServerDNSRequest()
{
    if (m_hDNSTask)
        WSACancelAsyncRequest(m_hDNSTask);
    ...                                                   // UDPSocket.cpp:84
```

so the buffer goes away as soon as the cancel returns. If a cancelled request
can still be written to — as the overlapped send in [`ovrepro/`](../ovrepro)
turned out to be — that is a write into freed memory arriving from the network.
The `dns-cancel` probe poisons the buffer with `0xCC` before cancelling and
looks at it afterwards.

The other half is the two UDP sockets: `SendTo`/`ReceiveFrom`, datagram
boundaries, truncation, a zero-length datagram (which eMule's receive loop
treats as nothing to do), and what a datagram sent to nobody does to the next
receive on the sending socket.

## Modes

| mode | what it does | what a finding looks like |
|---|---|---|
| `contract` | 12 deterministic probes | a `PROBE` line that differs between Windows and Wine |
| `stress` | many outstanding resolutions at once, over and over | an answer that never arrives |

`localhost` is used throughout the stress mode on purpose: it is answered from
the hosts file, so what is measured is the delivery of the answer and not the
speed of somebody's resolver.

## Running it

```
cmake -S dnsrepro -B build-dnsrepro -A x64
cmake --build build-dnsrepro --config Release
build-dnsrepro/Release/dnsrepro.exe --mode contract
build-dnsrepro/Release/dnsrepro.exe --mode stress --rounds 200
```

Exit code 1 means something was found. `.github/workflows/build-dnsrepro.yml`
builds ARM64/x64/Win32, runs both modes on the Windows runner, then runs the
same x64 binary under Wine on Linux and diffs the two probe tables.

## Results — 14 September 2026, run 34868566010

One x64 binary on both sides of the bench, Win32 as a third leg.

**The message path does not lose answers.** 1,600 resolutions per leg, eight
outstanding at a time, 200 rounds: `missing=0` everywhere. Worst round 16 ms on
Windows x64, 32 ms on Win32, 2 ms under Wine. Whatever happened to `FD_ACCEPT`
does not happen here — and this was the last place the same shape could have
been hiding.

Eleven of thirteen probes identical. The two that are not:

| probe | Windows | Wine 9.0 |
|---|---|---|
| `dns-cancel-race` | `late-messages=none written-after-cancel=no` | `late-messages=all written-after-cancel=YES` |
| `udp-unreachable` | `next-receive=connreset` | `next-receive=nothing` |

### `WSACancelAsyncRequest` does not cancel — and the answer lands in freed memory

Fifty tries each, cancelling a lookup of `localhost` immediately after starting
it, with the answer buffer re-poisoned **after** the cancel returns so that only
a later write can show:

- Windows: 50 tries, **0** late messages, **0** buffers written.
- Wine: 50 tries, **50** late messages, **50** buffers written.

The cancel returns success on both. On Wine the resolver then writes the answer
into the buffer anyway.

That buffer is a member of eMule's request object, and the object is deleted
right after the cancel:

```cpp
~SServerDNSRequest()
{
    if (m_hDNSTask)
        WSACancelAsyncRequest(m_hDNSTask);      // UDPSocket.cpp:84
```

so on this platform the resolver writes a `hostent` into memory eMule has
already freed. And it is not only a shutdown path — eMule sweeps its own
pending requests:

```cpp
// Just for safety.
// Ensure that there are no stalled DNS queries and/or packets hanging endlessly in the queue.
if (curTick >= pDNSReq->m_dwCreated + MIN2MS(2)) {
    delete pDNSReq;                             // UDPSocket.cpp:739
```

Any name that takes longer than two minutes to resolve is freed while its lookup
is still live.

The *message* that follows is harmless: `CUDPSocket::DnsLookupDone` looks the
task handle up in its list, does not find it, and logs `Unknown DNS task
completed`. It is the write that is not.

**Not seen in the field here.** That log line does not appear in any of the 197
log files on the machine this investigation started from — most likely because
this installation reaches its servers by address rather than by name, so
`WSAAsyncGetHostByName` is never called at all.

### A datagram sent to nobody

Windows turns the ICMP port-unreachable that comes back into `WSAECONNRESET` on
the *next* receive of the sending socket; Wine reports nothing and the receive
simply finds no data. `CUDPSocket::OnReceive` logs a failed receive and carries
on, so neither behaviour breaks anything — but on Windows eMule occasionally
logs a receive failure that under Wine it never will.

### What the first run cost

The first `dns-cancel` probe cancelled the lookup of a name that does not
resolve, so there was never an answer to write and `buffer-written=0` proved
very little. `dns-cancel-race` asks the same question of a name that does
resolve, re-poisons the buffer after the cancel, and repeats it fifty times to
catch the race from both sides.
