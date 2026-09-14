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

## Results

Not yet run.
