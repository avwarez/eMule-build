This build is eMule 0.72a with the changes below, compiled for x64, Win32 and ARM64.

### Uploads

- **Disk read errors:** a disk read that fails or never completes stops only the upload it was for; the other uploads go on. In 0.72a a single failed read stopped every upload until eMule was restarted.
- **Troubled files:** a shared file whose reads fail or take 20 seconds or more is set to Very low upload priority until the user changes it. The log names the file, the range and the error.
- **Cancelled reads:** when an upload ends while one of its reads is still pending, the file is not marked as faulty and its priority is not lowered.
- **Slot handover:** a client that eMule told to stand down is not given an upload slot again while that same connection is still open.
- **Complete sources:** the count of complete sources for a shared file now includes the first part of the file.

### Downloads

- **Write failures:** a download whose data cannot be written is paused instead of set to error, and the data already received stays in memory. A full or failing disk pauses every download on it; downloads on other disks go on.
- **Resuming:** a download paused by a write failure starts writing again when it is resumed by hand, beginning with the data that could not be written before.
- **Periodic flush:** the periodic flush of download data to disk runs in the background, so a slow disk no longer freezes the window.
- **Completion:** when a download completes, the final hash check starts only after all of its data has reached the disk.
- **Reserved names:** a download whose name is a reserved Windows device name (CON, NUL, COM1…) no longer overflows a buffer when it completes.

### PowerShare

- **Per-file switch:** a complete shared file can be set to PowerShare. Requests for it go ahead of the normal upload queue.
- **When it is refused:** PowerShare is refused for a file that many others already have complete. It is allowed while some part is seen on at most one of the clients asking for it, or while the complete sources are no more than its parts.
- **Low priority:** a file whose upload priority is set by hand to Low or Very low is held out of PowerShare until the priority is raised, and the log says so.
- **Keeping the slot:** a PowerShare client keeps its slot while nobody who ranks above ordinary clients is waiting. A LowID client that is not connected no longer holds a slot back.
- **MorphXT settings:** the PowerShare setting stored by MorphXT in `part.met` and `known.met` is read.

### Network

- **Port mapping:** port mapping (UPnP IGD, NAT-PMP, PCP) is done with libplum, which is now the default method. The old Windows UPnP service method has been removed, along with its two Tweaks options.
- **Windows Firewall check:** at startup eMule checks whether Windows Firewall lets other clients reach its TCP and UDP ports, and the log says which rule or network profile allows or blocks each port.
- **Firewall bar:** if inbound connections are blocked, a bar at the top of the window offers «Allow», which adds the needed rules with administrator rights, or «Ignore».
- **Send after close:** a network send that completes after its connection has already closed no longer writes into freed memory. This was a cause of random crashes.
- **Short UDP packets:** a UDP packet from a server too short to hold an opcode is no longer read past its end.
- **Banned clients:** a banned client that keeps sending data no longer keeps the interface busy.
- **IP filter through the buddy:** a firewalled client no longer answers a request that its Kad buddy forwards from a filtered address or a banned client. Before, it replied and updated that client's place in the upload queue.

### NAT traversal (beta)

- **Downloads between two LowIDs:** two LowID clients can now transfer files to each other; in 0.72a a LowID could not reach another LowID at all. They open a direct UDP path through their NATs (hole punching).
- **Transport:** the eD2K connection runs unchanged over that path, on the client UDP port that Kad uses, carried by KCP as it would be by TCP. Speed limits, queues and obfuscation work as over TCP.
- **NAT discovery:** at startup eMule checks its NAT with public STUN servers, using the natd library, right after UPnP and before connecting to the server and Kad. It takes a few seconds, at most a minute.
- **Startup wait:** until the check is over the Connect buttons are disabled, and the automatic connection starts when it ends. The server login then announces whether other LowIDs can punch through to this client.
- **Log:** the log gives the public address found and whether downloads between LowIDs can use hole punching. With verbose logging on, it also gives the kind of NAT and each step of a punch.
- **Turning it off:** a user with forwarded ports can turn it off at the bottom of Options > Connection ("Use NAT traversal between LowIDs", on by default). From the next start there is no check and no wait.
- **STUN servers:** the servers are read from `config/stun.dat`, one `host:port` per line. If the file is missing or lists fewer than 3 servers, a built-in list is used.
- **Through the server:** the request to punch goes first through the eD2K server, by the NAT callback of Lugdunum servers, when both clients are logged in to it, as eMule does for its callbacks.
- **Through Kad:** otherwise, for example with no server connection or a source on another server, the request goes through the source's Kad buddy, and the answer comes back through the requester's buddy.
- **Kad sources:** the Kad sources of a client that can be punched to say so, in a tag that other clients ignore. A source with an open UDP port is asked directly, without a buddy.
- **Mobile networks:** a client whose NAT picks a new port for each destination, inside a block of ports (as many mobile CGNATs do) or in sequence, can now be punched to by a client whose NAT keeps the same public port.
- **Port search:** that client tries first the port the server or the buddy saw, then the ports nearest to it, more at each round, with no assumption on how an operator lays out its ports.
- **Retries:** a request with no answer goes once more after 3 seconds. A punch that times out is asked for again, up to twice, within the time a TCP connection would take to give up.
- **Filters:** the IP filter and the list of banned clients apply to the addresses a punch goes to or comes from, as they do to any other connection.
- **Closing eMule:** the transfers that go through a punched UDP path now end at once on the other side, as TCP connections do, instead of after about 45 seconds of silence.
- **Limits:** two clients whose NATs both pick a new port for each destination cannot reach each other, nor can a client whose NAT picks ports at random from its whole range. Such a client stays a plain LowID.
- **Direct sources:** a Kad source with an open UDP port is punched to only by a client whose NAT keeps the same public port for every destination.

### Web interface

- **HTTPS on 64-bit:** HTTPS works on 64-bit builds. TLS was given only half of the socket handle.
- **Incoming connections:** the web server accepts every incoming connection instead of missing some under load.
- **Column changes:** a column change with an out-of-range index is ignored instead of writing outside the settings.
- **Search results:** file names in the search results are escaped, so a name with HTML characters can no longer alter the page.

### Stability

- **UPnP shutdown:** UPnP discovery is stopped by waiting for it to end, not by killing its thread.
- **Kad searches:** a Kad search that fails while starting is removed cleanly.
- **Ignored errors:** errors that were silently ignored are now written to the debug log.
- **ARM64 safety:** on ARM64, values shared between threads and data read from network packets at odd addresses are handled safely.
- **Packet logging:** detailed packet logging no longer reads past the end of a short packet.

### Windows and Wine

- **Windows 10 or later required:** Windows XP, Vista, 7 and 8 no longer receive security updates, a risk for a program exposed to the network. This build targets Windows 10, which still has extended support.
- **Old systems removed:** the code for the older systems has been removed: old firewall opener, run-as-user, legacy UI paths, version checks.
- **Current Windows calls:** Windows calls that were looked up at runtime for old systems are now called directly, and deprecated ones are replaced with their current equivalents.
- **Wine:** Wine 11.0 is supported; it is the version this build is tested with. Under Wine, the error text of a failed ping no longer makes eMule exit.

### Languages

- **Translations:** wrong strings fixed and missing strings added in the translations.
- **New strings:** the Windows Firewall bar and the NAT traversal option are translated into 31 of the 35 languages; the other four show them in English.

### Build

- **Toolchain:** built with C++23, Mbed TLS 4 and the current Windows SDK. Native ARM64 build.
- **Warnings:** a large set of compiler warnings fixed: format strings, uninitialized variables, pointer-width casts and others.
- **Third-party licenses:** the licenses of the libraries built into eMule (Crypto++, id3lib, KCP, libplum, Mbed TLS, ResizableLib, zlib) are in the `licenses` folder.
