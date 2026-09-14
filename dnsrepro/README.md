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

## Results

Not yet run.
