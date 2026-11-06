# Portable Installer Diagnostics

Approved direction: the user requested a portable test EXE generating 10-30 GiB,
transferring it between disks, logging measurements, and comparing the actual
installer extraction pipeline with ordinary 7-Zip. They explicitly asked to build it.

Use the existing C++20/Win32 toolchain and embedded 7-Zip tools, not a .NET or
browser dependency. Default to 10 GiB, with 10/20/30 GiB controls and four matching
decoder threads. The user selects source/destination disks, not folders. The GUI
maps each disk to X:\ModlistDiagnostics and the backend creates unique run folders
below it. Existing files are never overwritten. Scripted CLI parent paths remain
supported.

Generate real, deterministic mixed-compressibility bytes: 80% in files no larger
than 2 GiB, 20% distributed across 200,000 small files, plus empty/Unicode entries.
Measure generation, bulk copy, small-file copy, copy metadata checks,
LZMA2 archive creation, archive hashing, each extraction, and each output metadata check.
Run both extractors against the identical split archive, same destination volume,
and same thread count. Allow either order and an optional reverse-order repeat.
Do not claim cold-cache measurements or disable security software/cache globally.

Reuse PipelinedSevenZipExtractor, adding opt-in atomic diagnostic counters only.
Collect read/cache/hash waits and output create/directory/write/close costs.
Ordinary 7-Zip runs as a supervised child process, with resource sampling and logs.
Per the user's follow-up, do not hash or reread each payload file. Every output
is checked against the expected names, types, counts, and sizes only. This does
not detect same-size content changes. Keep archive-block SHA-256 validation in
the real pipeline comparison, and report metadata rates as files/sec, not payload
throughput.

Log CPU/RAM, volume/free-space/model information where available, per-second
speed/resource samples, average/peak sampled speeds, file rates, precise errors,
commands, order, tool versions/hashes, and summaries in TXT/CSV/JSON.

Never overwrite or delete existing user files. Use exclusively created unique
run directories, retain generated data, preflight worst-case space (source 2x,
destination 3x or 5x for a reverse repeat), and preserve logs after errors/cancel.
No automatic elevation or network access. Cancellation must stop child processes.

Alternatives considered: copy-only diagnostics cannot investigate decoder overhead;
a separate simulated extractor would not test production code. The selected
approach combines a standalone utility with the actual shared extraction backend.

Deliver a ready EXE and README in a new folder below E:\Test builds. No release,
commit, or push is requested. Existing uncommitted installer UI changes are unrelated.
