# Installer Performance Findings

Analysis date: 2026-11-05. Log timestamps below are reproduced as recorded.

## Summary

The real installer log confirms that the long-running phase is verified
extraction, not the final move. Both completed runs have a pronounced slowdown
around archive volume `.029`.

The tester reports that ordinary 7-Zip extracts the modlist faster. If archive,
destination, thread count, and cache conditions are comparable, our integration
or its settings should be the primary investigation target. That report is
important, but a timed, controlled native-vs-installer comparison is still missing.

Review follow-up confirmed a read-ahead scheduling flaw: cache hits do not advance
the prefetch window. A second confirmed difference is that native 7-Zip 26.02
preallocates eligible output files on Windows, while our output writer does not.
These take investigation priority over redundant parent-directory calls. Their
contribution to this tester's elapsed time has not yet been measured.
There is insufficient evidence to blame a faulty HDD, the SSD, antivirus, or
SHA-256 alone.

## Evidence Sources

- Actual installer log: `C:\Users\user\Downloads\modlist-installer (6).log`.
- Tester diagnostic report:
  `ModlistBench-2026-11-05_22-21-53-23448-3629032428169849580.7z` in Downloads.
  The relevant members are `diagnostics.log`, `results.json`, `results.txt`, and
  `samples.csv` inside the archive's run folder.
- Local reference benchmark:
  `E:\Test builds\Modlist-Diagnostics-20261105-no-file-sha\logs\ModlistBench-2026-11-05_12-26-01-7628-2297886756954811761\results.json`.
- Two supplied Task Manager screenshots of E:.
- Current working-tree extraction code, with repository HEAD `e9e391d` and
  uncommitted changes. Code line numbers can move after subsequent edits.

The installer and tester benchmark match in package location, Windows profile,
and source/destination drive letters. Hardware details come from the benchmark,
not from the older installer log itself.

## Tester's Hardware And Workload

| Item | Observed value |
| --- | --- |
| CPU | AMD Ryzen 7 5700X, 16 logical processors |
| RAM | Approximately 16 GB |
| Source E: | TOSHIBA HDWD110, SATA HDD |
| Destination C: | KINGSTON SNV2S1000G, NVMe SSD |
| Installer decoder threads | 4 |
| Modlist archive | 47 volumes, 185.2 GB as displayed in the log |
| Unpacked modlist | 470.2 GB as displayed in the log |
| Archive path inspection | 930,365 file/directory paths |

The longest planned destination path was 242 characters against the 240-character
direct-extraction budget. The installer therefore unpacked into `C:\Unpacking`
and then moved the result into `C:\Games\TRUEAE_V54_EXT` on the same volume.

## Actual Installer Timings

| Run | Verified extraction interval | Duration | Post-extraction installation phase |
| --- | --- | --- | --- |
| First | Nov 3 23:12:56 to Nov 4 01:19:28 | 2h 6m 32s | 22s |
| Second | Nov 5 09:20:57 to Nov 5 10:43:24 | 1h 22m 27s | 43s |

The final phase includes folder enumeration and the same-volume move; these are
not timings for copying the full 470 GB to another disk. The log explicitly says
that files will not be copied. Both installations completed successfully.

### Volume .029 Hotspot

| Run | Volume .029 log interval | Duration |
| --- | --- | --- |
| First | Nov 4 00:20:29 to 00:46:17 | 25m 48s |
| Second | Nov 5 10:10:39 to 10:19:53 | 9m 14s |

These intervals run from the volume's validation-start message to its
validation-complete message. They are **not measurements of SHA-256 CPU time**.
Validation, decoding, and output writing are pipelined. Demand for archive blocks
depends on downstream extraction progress, so a long interval can also reflect
decoding, file creation, writes, or waits elsewhere in the pipeline.

The repeated hotspot makes this section a useful profiling target. It does not
by itself identify which component is slow. The substantial difference between
the two runs also means cache state or other environmental conditions matter.

## Diagnostic Benchmark Findings

The synthetic workload contained 10 GiB and 200,006 files, including 200,000
small files. Per-file SHA-256 checks were disabled.

| Operation | Tester | Local reference |
| --- | --- | --- |
| Bulk copy average | 69.74 MiB/s | 151.66 MiB/s |
| Small-file copy average | 6.51 MiB/s | 11.49 MiB/s |
| Small-file copy elapsed | 314.55s | 178.20s |
| Output metadata check | 32.18s | 2.15s |

The metadata check did not read or hash file contents. Its unusually high cost on
the tester's machine makes filesystem operations and filtering worth profiling,
not just bulk SSD bandwidth. Possible contributors include filesystem metadata
handling, security filters, CPU/memory differences, and competing activity.
The logs do not identify a particular filter or prove antivirus is responsible.

This is not a controlled comparison of identical machines. The local reference
has approximately 48 GB RAM versus the tester's 16 GB, and a different CPU and
storage setup. Cache conditions differ, so the numbers are clues, not a direct
hardware diagnosis or a universal installer performance ratio.

### Why The Window Showed 0 MiB/s

The tester's diagnostic run was cancelled during `archive_create`, after
15m 29s of compression. It never reached either extraction method.

The old compression display used compressed output bytes written as a proxy
against the uncompressed input total. Output writes were bursty. In the late
650-900 second window, process reads averaged about 1.55 MiB/s while many samples
had zero output writes. CPU activity also continued.

Therefore, zero displayed output speed did not mean the whole process was
inactive. This display bug is separate from the real installer's unpacking speed.
Compression was also reading and writing E: in that preparation stage, which is
not the same workload as extracting archives from E: onto C:.

## What The Disk Screenshots Establish

- One screenshot shows E: at 100% active time, 107 MB/s reading, zero writes,
  and 44.5 ms average response time at that instant.
- The other shows E: mostly idle over its displayed history, with brief bursts.
- The exact application stage and timing relative to each screenshot were not
  established, so they cannot be treated as a synchronized trace of one phase.

Together they do not establish continuous HDD saturation or a faulty disk.
During actual unpacking, bursts followed by idle periods could be consistent
with reading a block and then decoding or writing its contents. A full input
queue, a busy output path, CPU work, or a pipeline wait can all produce idle
source-disk periods. Simultaneous destination-disk and per-core CPU observations
are needed to distinguish them. Low total CPU usage can hide a busy single thread.

## Confirmed Code Findings

Source: [PipelinedSevenZipExtractor.cpp](../InstallerApp/src/extractor/PipelinedSevenZipExtractor.cpp).

### 1. Read-Ahead Does Not Advance On Cache Hits

`VerifiedArchiveStream::GetBlock` updates the access stamp and returns immediately
on a cache hit. Only the miss path queues the requested block and the following
four blocks. With sequential consumption, the worker can finish that batch and
become idle while the decoder consumes cached blocks; a later request then waits
for another load. Current relevant lines: 736-767.

This scheduling flaw is confirmed. It can introduce avoidable demand-read waits,
but the screenshots alone do not establish that it explains their entire idle
intervals or the installation-time gap. No percentage speed improvement has been
measured or promised.

A correction should replenish read-ahead on hits as well as misses, wake the
worker when new work is added, preserve demand priority, and respect the six-block
cache budget. Track an in-flight load to avoid repeatedly queueing it while it
is neither cached nor still in the queued set. Eviction should favour consumed
blocks while protecting the current block and useful read-ahead; backward seeks
and stale speculative work also need coverage. Preserve full SHA-256 validation.

### 2. Native Windows Output Preallocation Is Missing

Our `FileOutStream` exposes `ISequentialOutStream` and writes newly opened files
without first setting their final length. Current relevant lines: 1105-1177.

This differs from the matching upstream 7-Zip 26.02 source:

- [ArchiveExtractCallback.h](https://github.com/ip7z/7zip/blob/26.02/CPP/7zip/UI/Common/ArchiveExtractCallback.h#L79)
  enables `PreAllocateOutFile` by default on Windows.
- [ArchiveExtractCallback.cpp](https://github.com/ip7z/7zip/blob/26.02/CPP/7zip/UI/Common/ArchiveExtractCallback.cpp#L1406)
  sets the known file length before extraction for eligible non-split output
  items larger than 4 KiB, then seeks back to the beginning. On close, it can
  reduce a previously reserved length to the number of bytes actually written.

The implementation difference is confirmed; its performance impact is not.
The upstream "split" condition concerns output items assembled in pieces, not
simply reading a multi-volume `.7z.001` archive set.

Merely exposing `IOutStream::SetSize` does not guarantee the decoder will call it.
The native extraction callback performs preallocation itself. An equivalent
approach here can query item size in `GetStream` and prepare the file explicitly.
It must handle unknown/invalid sizes, allocation failures, cancellation, and
truncating an incomplete reserved file without requesting privileged valid-data
operations. Test this independently from the read-ahead correction.

### 3. Redundant Directory Calls Remain An Optimisation Candidate

`ArchiveExtractCallback::GetStream` calls `CreateOutputDirectories` for every
extracted file's parent, even if it was already created. The helper calls
`std::filesystem::create_directories`. Relevant lines: 67, 1222, 1268.

This is lower priority than the confirmed scheduling flaw. However, dividing
the 32-second metadata check by 200,000 files does not measure a single directory
call: that check also performs path construction, set operations, file type/size
queries, and directory enumeration. It cannot establish an exact upper bound on
the directory-call cost or prove that caching would save a specific number of
minutes. Measure the dedicated directory counter instead.

### 4. Smaller Costs And Qualifications

- `SetExtractionArchive` constructs a filename path on each stream read before
  the reporter can reject an unchanged archive index. Avoidable, but unmeasured.
- Reading and hashing use one worker, but it already issues the next overlapped
  read before hashing the current read buffer. They are not wholly serialized.
- UI delivery uses `PostMessage`, so the producer does not wait for rendering.
  Nevertheless, `ProgressReporter::Notify` invokes the supplied callback
  synchronously under a callback mutex. Callback preparation/locking costs are
  not automatically zero just because the final UI message is asynchronous.

## Recommended Next Work

1. **Benchmark the corrected sliding read-ahead.** The scheduler correction was
   implemented on 2026-11-06 with focused regression tests (see follow-up below).
   Measure before/after with the same archive and thread count; do not estimate
   the gain from deterministic scheduling tests.
2. **Test output preallocation as a separate change**, using the upstream Windows
   policy as a reference. Cover successful extraction, empty files, unknown sizes,
   insufficient space, and incomplete/cancelled output. Compare before/after on
   the same destination filesystem; do not assume it explains the whole gap.
3. **Profile the actual installer**, especially the `.029` region. Record
   aggregate input-read waits, cache misses/reloads, hash time, directory work,
   file-open time, output writes, and closes. Log periodic/archive-level summaries,
   not every filename. Timings overlap and must not be added as independent costs.
4. **Test a per-run directory cache.** Remember only successful directory checks,
   preserve empty directories, keep path-safety checks, and handle a parent being
   removed or made inaccessible. Retain cancellation and meaningful error handling.
   Measure before and after; this optimisation is not yet implemented.
5. **Run a controlled native 7-Zip comparison on his PC.** Use the same archive
   set, output volume, thread count, and tool version where possible. Use unique
   empty output folders and repeat in reverse order. Keep post-processing outside
   extraction timings. Document cache and concurrent disk activity.
6. **If necessary, isolate the two custom components.** A diagnostic-only SDK run
   with ordinary input plus our output callback can help distinguish output costs
   from the verified input stream. This is an experiment, not a reason to ship
   production validation disabled.

For a faster synthetic test setup, compressing the already-copied SSD dataset
while still saving the archive onto the source HDD is worth testing separately.
That changes preparation only; it must not be presented as an installer speed fix.

## Changes Already Made Versus Open Questions

- Diagnostic per-file payload hashes were removed; output checks use metadata.
- The diagnostic UI now defaults to 10 GiB and creates its own folders on selected disks.
- The misleading compression counter has been fixed to use input-read progress,
  with CPU/RAM, separate read/write rates, and periodic activity logging. Both-order
  smoke tests passed. Build: `E:\Test builds\Modlist-Diagnostics-20261105-progress`.
- Optional extraction timing counters exist, but the supplied real installer log
  does not contain those detailed measurements.
- The read-ahead and preallocation review findings were verified against local
  code and upstream 7-Zip 26.02. The read-ahead fix is now implemented locally.
  Output preallocation, directory caching, decoder-thread policy and displayed
  throughput remain unchanged; SHA-256 protection has not been reduced.

**Working conclusion:** measure the corrected read-ahead policy, then test
native-style output preallocation separately. Retain directory caching
as a measured secondary optimisation. These findings strengthen the case for
investigating our integration but do not yet quantify the cause of the tester's
slowdown.

## Implementation Follow-Up: 2026-11-06

The production verified stream now uses `VerifiedBlockScheduler`: requests advance
the window on hits and misses, stale queued speculation is discarded on seeks,
demand joins in-flight loads, and eviction prefers obsolete data over unread
forward blocks. Space is reserved before loading, keeping cached, borrowed and
loading buffers within six chunks. Under borrowed-buffer pressure, essential
demand may displace the farthest unborrowed speculative block. Shutdown is also
visible to overlapped-read cancellation, not just idle worker waits.

The original extracted policy failed the cache-hit prefetch test while existing
core tests still passed. The corrected policy passes nine deterministic cases,
including sequential one-load-per-chunk behavior, upcoming-block preservation,
seeks, demand priority, live-buffer bounds, borrowed-buffer backpressure and final
verification joining in-flight work. These tests use the production helper.

Real split-archive tests cover payload equality, Unicode names, empty directories,
direct/staged extraction, active cancellation, output-open failure, and SHA-256
rejection both in normal input and unused appended tail data. Windows Release,
core, UI and diagnostics tests passed. Diagnostics integration passed from short
parent paths; an initial run under a longer workspace parent correctly failed
its path-length guard before the mutation test could run.

This is correctness/smoke evidence, not a full-modlist before/after performance
result. The existing safe cancel-and-drain I/O path is retained; a deterministic
test forcing destruction during a genuinely pending Windows read is still absent.
