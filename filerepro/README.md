# filerepro — the life of an eMule part file, on its own

The fourth specimen of the dual bench, after [`wsrepro/`](../wsrepro),
[`iocprepro/`](../iocprepro) and [`ovrepro/`](../ovrepro). Same method:
reproduce eMule's use of one Win32 subsystem construct for construct, with
nothing else around it, and run the same binary on real Windows and under Wine.
Windows is the oracle.

## Why files, and why this is the widest of the four

Twenty-five of the Win32 functions eMule actually calls are about files, and
they are the ones an emulation layer has to map onto a filesystem that works
nothing like the one they were written for. What eMule does to a download is
create a `.part` file, give it its final length before any of it has arrived,
write blocks into it in whatever order they turn up, ask how much room it really
takes, and finally move it to the incoming directory.

Three of those answers change what eMule **does**, not just what it reports:

**`GetVolumeInformation`** — eMule asks for the filesystem *name* and compares it
with `"NTFS"` (`OtherFunctions.cpp:2766`, `IsNTFSVolume`) because it compensates
the NTFS daylight-saving file time behaviour on every shared file. Report
something else and it takes the other branch, and the dates of every shared file
shift by an hour.

**`GetCompressedFileSize`** — `GetDiskFileSize` (`OtherFunctions.cpp:2620`) uses
it to learn how much disk a part file really occupies, which is not its length
when the file is sparse. It also reads `GetLastError()` without having cleared
it first, so its failure path depends on what the previous unrelated call left
behind.

**`MoveFileWithProgress`** — the completion of a download
(`PartFile.cpp:2859`), with an explicit retry for `ERROR_SHARING_VIOLATION`,
which is Windows refusing to move a file somebody still holds open. A platform
that allows that move takes both the retry and the protection out of the
picture.

## Modes

| mode | what it does | what a finding looks like |
|---|---|---|
| `contract` | 15 deterministic probes of the calls above and their neighbours | a `PROBE` line that differs between Windows and Wine |
| `lifecycle` | the whole life of a part file, over and over, verified byte for byte | any non-zero count: a write, a move, a length or a byte that did not survive |

## Running it

```
cmake -S filerepro -B build-filerepro -A x64
cmake --build build-filerepro --config Release
build-filerepro/Release/filerepro.exe --mode contract
build-filerepro/Release/filerepro.exe --mode lifecycle --rounds 20
```

Exit code 1 means something was found. `.github/workflows/build-filerepro.yml`
builds ARM64/x64/Win32, runs both modes on the Windows runner, then runs the
same x64 binary under Wine on Linux and diffs the two probe tables.

## Results — 14 September 2026, run 34859897602

One x64 binary, built once on the Windows runner and run on both sides of the
bench in the same CI run; Win32 ran on the Windows runner as a third leg.

**The `lifecycle` mode is clean everywhere**: 20 part files per leg, created,
given their final length, filled out of order, flushed, moved and read back byte
for byte — `create=0 size=0 write=0 flush=0 ondisk=0 move=0 length=0 verify=0
delete=0` on Windows x64, Windows Win32 and Wine 9.0 alike. Nothing eMule does
to the *contents* of a part file comes out differently.

**The probe table: 12 of 17 identical, 5 different.** Four of them are about the
same thing — sparse files — and one is an error number.

| probe | Windows | Wine 9.0 |
|---|---|---|
| `volume-info` | `sparse=1` | `sparse=0` |
| `attributes-sparse` | `sparse-flag=1` | `sparse-flag=0` |
| `disksize-sparse` | `just-the-written-part` | `full-length` |
| `filetime-creation` | `kept=1` | `kept=0` |
| `long-path` | `err=3` | `err=2` |

### Sparse files: eMule is told the opposite of the truth

`FSCTL_SET_SPARSE` **succeeds** under Wine, and the file really is sparse — the
`sparse-costs-disk` probe asks the disk itself and both platforms answer
`no-just-the-blocks`, so a 4 GB part file with 4 KB in it costs 4 KB on either.
But everything eMule can *ask* about it says otherwise:

- `GetVolumeInformation` does not report `FILE_SUPPORTS_SPARSE_FILES`;
- `GetFileAttributes` never returns `FILE_ATTRIBUTE_SPARSE_FILE`;
- `GetCompressedFileSize` returns the length, not the room taken.

Which lands in three places in eMule:

- `CPartFile::IsNormalFile()` (`PartFile.h:149`) reads exactly that attribute, so
  under Wine it is always true. At `PartFile.cpp:4028` it is one of the
  conditions for allocating a part file to its full length, and at
  `DownloadQueue.cpp:1031` it decides which side of a disk-space check a file
  falls on.
- `GetDiskFileSize` (`OtherFunctions.cpp:2620`) over-reports, and
  `PartFileConvert.cpp:291` uses it as `spaceneeded` — so importing a sparse
  part file asks for its whole length in free space rather than what it occupies.

No data is at risk and no disk is eaten. What is wrong is eMule's picture of its
own files.

### The creation time that is accepted and dropped

`SetFileTime` returns success under Wine and the creation time does not change.
eMule writes one back at `PartFile.cpp:427` when it detects NTFS time
tunnelling, inside a `VERIFY(...)` — which passes, because the call did return
success. So the correction silently does nothing, and the `if (m_tLastModified -
m_tCreated > 1) //tunnelling!` branch fires again on every open, forever.

### What the first two runs cost

The first run reported `move-open` as a difference: Windows refusing to move a
file that is open, Wine allowing it. That was **not** a platform difference — the
probe before it had deleted the same file through a live handle, which on
Windows leaves the name in a delete-pending state, so the move failed for a
reason that had nothing to do with the question. Given a file of its own, both
platforms move an open file happily, because eMule opens part files with
`FILE_SHARE_DELETE`. eMule's `ERROR_SHARING_VIOLATION` retry at
`PartFile.cpp:2862` is about *other* processes holding the file, not about the
platform.

The first run also read `disksize-sparse` before flushing, and NTFS allocates a
cached write when it reaches the disk — so Windows answered `0` for a file that
had 4 KB in it. Both probes now stand on their own, and the error numbers are
reported as numbers rather than as names I chose for them.
