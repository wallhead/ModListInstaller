# Modlist Diagnostics

Portable Windows x64 diagnostic tool. No installation, administrator rights,
browser runtime, or Internet connection required. The EXE embeds the same 7-Zip
tools and extraction pipeline as the installer.

## Run

1. Open `modlist-diagnostics.exe`.
2. Choose the source disk for generated data/archives and a different destination
   disk for copying/extraction. There are no folder fields or Browse buttons.
   The tool creates `X:\ModlistDiagnostics\ModlistBench-*` on each selected disk.
   Existing files are never replaced; errors name the path and Windows error.
3. Select 10, 20, or 30 GiB of real file content (default 10 GiB). Leave both
   extraction methods at the default four threads for the first comparison.
4. Choose which extractor runs first; optionally repeat in reverse order.
5. Start. Stop cancels work and retains partial data and logs. Closing the window
   during a test requests cancellation and waits for the worker to stop.

The dataset contains 80% bulk bytes, 20% bytes across 200,000 small files, empty
files/directories, and a Unicode filename. It is synthetic mixed-compressibility
data, not a copy of a real modlist. No existing files are overwritten or deleted.

**Disk space:** source reserves about twice the chosen size plus 1 GiB;
destination reserves three times plus 1 GiB, or five times plus 1 GiB with the
reverse repeat. Small-file allocation rounding adds overhead. The default 10 GiB
test needs at least roughly 21 GiB source / 31 GiB destination free, or
51 GiB destination with repeat. Exact preflight checks use each volume's cluster
size. Choose short parent paths; generated full paths must fit in 240 characters.
Only identifiable local volumes are supported, not network shares.

All data is retained in unique `ModlistBench-*` subfolders below `ModlistDiagnostics`
on both selected disks. The source defaults to the EXE's disk; the destination
defaults to the other available disk with the most free space.
Review results first, then remove only those generated folders manually when no
longer needed. Nothing is cleaned automatically. Tests can take a long time on
HDDs because they deliberately create, copy, and extract many small files.

## What Is Measured

Generation, bulk copy, small-file copy, copy metadata checks,
archive creation, archive hashing, verified pipeline extraction, ordinary
`7z.exe` extraction, and metadata checks of each extracted tree are separate stages.
Per-file payload SHA-256 is disabled: generated, copied, and extracted payload
files are not reread for content checks. Outputs are checked for names, types,
file/directory counts, and sizes only. Same-size content changes are not detected.
Both extractors use the same archive and thread count. Metadata stages report
file rates only, not payload throughput.

Pipeline extraction includes asynchronous archive SHA-256 validation; ordinary
7-Zip extraction does not. Post-extraction output verification is excluded from
both extraction timings. Archives use LZMA2, level 5, 32 MiB dictionary, word size
32, 8 GiB solid blocks, and 4092 MiB volumes.

Windows caches are not flushed. Earlier generation, copying, compression, and
verification warm caches; order matters. A reverse-order repeat helps expose
order effects but does not make this a cold-cache benchmark. Avoid other heavy
disk activity while comparing. Sampled peaks are approximately one-second rates,
not hardware specifications. Compression live progress/peaks use child input-read
bytes; ordinary 7-Zip extraction uses unpacked output-write bytes. Both are
process-I/O proxies, not physical disk throughput. Final averages use the known
logical payload size. The window shows CPU, RAM, and separate read/write rates,
so buffered archive output does not make active input reading look like a stall.
Child-process progress stays below 100% until successful exit. Activity is also
logged every 15 seconds, including CPU-only work and no measured activity.

## Logs To Send Back

Use **Open Logs** after the test. Each run gets its own folder below `logs`
beside the EXE (or `%LOCALAPPDATA%\ModlistDiagnostics\logs` if not writable).

- `results.txt`: stage times, average/peak sampled speeds, and file rates.
- `results.json`: configuration, drive/CPU/RAM details, status, tool hashes,
  per-stage resource totals, and pipeline counters.
- `samples.csv`: per-second speed, file count, process CPU, RAM, and read/write I/O.
- `diagnostics.log`: stages, archive validation progress, exact commands/errors.
- `7zip-create.log` / `7zip-1.log`: ordinary 7-Zip output and version.
- `source-inventory.jsonl`: generated relative paths, types, and sizes (no hashes).

Send the first four files, plus 7-Zip logs when there is an error. Logs include
selected absolute folder paths and hardware model information; review before
sharing. No data is uploaded automatically.

Pipeline counters distinguish block/cache waits, hashing, output file creation,
directory checks, writes, and closes. Timings overlap and must not be summed.
Read/write counters are process/OS-request bytes, not physical device traffic.

## Command Line / Developer Tests

```powershell
.\modlist-diagnostics.exe --run --source "D:\Bench" --destination "E:\Bench" --gib 10 --threads 4
```

Add `--sevenzip-first` or `--reverse` as needed. `--smoke` uses 64 MiB and 512
small files; same-volume parents are permitted only in smoke mode. Exit codes:
0 passed, 1 failed, 2 cancelled. CLI runs without a window. PowerShell callers
should explicitly wait for the GUI-subsystem process using `Start-Process -Wait`.
The CLI retains explicit `--source` / `--destination` parent paths for scripting;
the desktop interface uses disk selection only.

Build from `InstallerApp` with `scripts\build-diagnostics.ps1`. It runs the fast
workload/core tests and packages the EXE, license, and this README. It does not
start a heavy benchmark or modify installer/packer distribution files.

For integration tests, build `diagnostics_integration_tests`, then run it with
two short, existing, disposable parent folders. It checks automatic nested-folder
creation, non-destructive invalid-path failures, and runs 64 MiB comparisons in
both orders, checks output metadata without content rereads, rejects wrong-sized copies,
tests native-child cancellation and report-write failure, and retains its data.
