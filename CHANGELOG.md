This build is eMule 0.72a with the changes below, compiled for x64, Win32 and ARM64.

### Uploads

- **Disk read errors:** a disk read that fails or never completes stops only the upload it was for; the other uploads go on. In 0.72a a single failed read stopped every upload until eMule was restarted.
- **Slow files:** a shared file whose reads take 20 seconds or more is moved to Very low priority, so it no longer holds up the other uploads. The log names the file, the range and the error.
- **Cancelled reads:** when an upload ends while one of its reads is still pending, the file is not marked as faulty and its priority is not lowered.
- **Slot handover:** a client that eMule told to stand down is not given an upload slot again while that same connection is still open.
- **Complete sources:** the count of complete sources for a shared file now includes the first part of the file.

### Downloads

- **Write failures:** when download data cannot be written to disk (disk full, drive removed, write error), only the downloads on that volume stop writing, and the data already received stays in memory. Downloads on other disks go on. Writing resumes when the affected downloads are restarted.
- **Periodic flush:** the periodic flush of download data to disk runs in the background, so a slow disk no longer freezes the window.
- **Completion:** when a download completes, the final hash check starts only after all of its data has reached the disk.
- **Reserved names:** a download whose name is a reserved Windows device name (CON, NUL, COM1…) no longer overflows a buffer when it completes.

### PowerShare

- **Per-file switch:** a complete shared file can be set to PowerShare. Requests for it go ahead of the normal upload queue.
- **When it is refused:** PowerShare is refused for a file that many others already have complete. It is allowed while some part of the file is seen on at most one of the clients asking for it.
- **Low priority:** a file whose upload priority is set to Low or Very low is held out of PowerShare, and the log says so.
- **Keeping the slot:** a PowerShare client keeps its slot while nobody who ranks above ordinary clients is waiting. A LowID client that is not connected no longer holds a slot back.
- **MorphXT settings:** the PowerShare setting stored by MorphXT in `part.met` and `known.met` is read.

### Network

- **Port mapping:** port mapping (UPnP IGD, NAT-PMP, PCP) is done with libplum, which is now the default method. The old Windows UPnP service method has been removed, along with its two Tweaks options.
- **Windows Firewall check:** at startup eMule checks whether Windows Firewall lets other clients reach its TCP and UDP ports, and the log says which rule or network profile allows or blocks each port. If inbound connections are blocked, a bar at the top of the window offers «Allow», which adds the needed rules with administrator rights, or «Ignore».
- **Send after close:** a network send that completes after its connection has already closed no longer writes into freed memory. This was a cause of random crashes.
- **Short UDP packets:** a UDP packet from a server too short to hold an opcode is no longer read past its end.
- **Banned clients:** a banned client that keeps sending data no longer keeps the interface busy.

### Web interface

- **HTTPS on 64-bit:** HTTPS works on 64-bit builds. TLS was given only half of the socket handle.
- **Incoming connections:** the web server accepts every incoming connection instead of missing some under load.
- **Column changes:** a column change with an out-of-range index is ignored instead of writing outside the settings.

### Stability

- **UPnP shutdown:** UPnP discovery is stopped by waiting for it to end, not by killing its thread.
- **Kad searches:** a Kad search that fails while starting is removed cleanly.
- **Ignored errors:** errors that were silently ignored are now written to the debug log.
- **ARM64 safety:** on ARM64, values shared between threads and data read from network packets at odd addresses are handled safely.
- **Packet logging:** detailed packet logging no longer reads past the end of a short packet.

### Windows and Wine

- **Windows 10 or later required:** Windows XP, Vista, 7 and 8 are past their end of life and no longer receive security updates, which makes them a risk for a program exposed to the network. This build targets Windows 10, which still has extended support, and the code for the older systems has been removed: old firewall opener, run-as-user, legacy UI paths, version checks.
- **Current Windows calls:** Windows calls that were looked up at runtime for old systems are now called directly, and deprecated ones are replaced with their current equivalents.
- **Wine:** Wine 11.0 is supported; it is the version this build is tested with. Under Wine, the error text of a failed ping no longer makes eMule exit.

### Languages

- **Translations:** wrong strings fixed and missing strings added in the translations.

### Build

- **Toolchain:** built with C++23, MbedTLS 4 and the current Windows SDK. Native ARM64 build.
- **Warnings:** a large set of compiler warnings fixed: format strings, uninitialised variables, pointer-width casts and others.
