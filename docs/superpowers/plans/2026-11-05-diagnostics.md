# Portable Diagnostics Implementation Plan

> **For agentic workers:** use the applicable execution and TDD skills. Steps use checkbox syntax for tracking.

**Goal:** Deliver a self-contained diagnostic EXE for disk copying and verified-vs-native 7-Zip extraction comparisons.

**Architecture:** A new diagnostics backend and Win32 frontend compile the same extraction/core source files with a static CRT for portability. Optional atomic counters instrument the shared pipeline without changing normal extraction behavior.

**Tech Stack:** C++20, Win32, BCrypt, embedded 7-Zip, xmake, existing JSON parser.

**Spec:** docs/superpowers/specs/2026-11-05-diagnostics-design.md

## Constraints
- Dataset sizes 10/20/30 GiB; default 10; four matching extraction threads.
- Unique retained run folders, no deletion/overwrite/elevation/network.
- Exact output metadata inventory (names/types/counts/sizes), without per-file SHA-256 or payload rereads. Keep archive SHA-256 for the real pipeline comparison; separately time metadata checks.
- Explicit order/cache caveats, resource/cost logs, cancellation and space checks.
- Do not commit/push or modify unrelated pending UI changes.

## Review Focus
- Aliased/same-volume parents and insufficient free space must not start a misleading comparison.
- Cancellation must stop child processes and leave truthful partial reports.
- Paths containing Unicode, spaces, or long names must not corrupt command arguments or JSON.
- Native-vs-pipeline timings must exclude output verification and identify different validation work.
- All declared data bytes must actually be written, copied, and verified.

## Tasks
1. [x] Workload/config API and tests: exact 10/20/30 GiB budgets, safe relative paths, profile distribution, invalid inputs.
2. [x] Optional pipeline telemetry and regression tests: counters match validated/output bytes, corruption still fails, normal null-counter behavior unchanged.
3. [x] Backend: generation/copy/CNG hashes, safe workspace/space checks, supervised 7-Zip, shared pipeline, reports/resource samples; smoke integration with corrupt-output tests.
4. [x] Frontend: source/destination pickers, size/threads/order, reverse pass toggle, responsive progress/log, Stop/Open Logs, headless smoke CLI.
5. [x] Build/tests/review: portable target built, core/diagnostics/UI suites passed, complete smoke comparisons passed; full 10 GiB preparation tested but benchmark deliberately stopped during hashing (see ledger); packaged to E:\Test builds.

Independent write scopes: frontend Main.cpp; pipeline instrumentation header/cpp and focused core tests; coordinator owns Diagnostics.h, Benchmark.cpp, workload tests, build wiring and integration.

## Ledger
- Pre-flight: GUI and backend consume Diagnostics.h; pipeline counters have a separate atomic contract. No UI-refactor files need edits.
- Ruling: retain all test outputs instead of automated cleanup, avoiding deletion risk and allowing inspection; preflight includes all retained copies.
- Ruling: no commits, although skill templates suggest frequent commits; user requested an executable, not Git operations.
- Verified 64 MiB complete copy/archive/extract/verify runs in both orders, copied-data corruption rejection, native cancellation, and report-write failure. Core/workload/UI tests passed.
- Review fixes: test fault injection now targets only exclusively owned folders/current log path; final logical totals are set after the sampler joins; unknown volume identities are rejected; log stream state is mutex-protected.
- GUI startup reproduced a common-control initialization failure; requesting only ICC_PROGRESS_CLASS corrected it. GUI control tree/defaults observed; full-screen game prevented activation and screenshot QA.
- Full trial generated all 10 GiB/200,006 files, then was intentionally terminated during source hashing after about 30,000 files. No completed full-size A/B results claimed. Partial data/logs retained; final verification uses bounded complete integration runs.
- Final packaged EXE SHA-256: 5C8A3A18BBBE37089D41E0658424DA160D9D83EF00C34815AC553ABBD6D8CFA7. Its --smoke --reverse run exited 0 and verified all outputs; report logs/ModlistBench-2026-11-05_11-39-59-48640-7234094928061738477 under E:\Test builds\Modlist-Diagnostics-20261105. ZIP contains only EXE, README, and 7-Zip license, no generated data.
- Follow-up folder failure: reproduced the reported existing-parent rejection with a failing regression test. Missing nested parents are now created on Start, while pre-cancel creates none. File collisions preserve original contents and name the source/destination, full path, and Windows error. Selected paths are included in failure reports. Integration tests, workload/core tests passed; corrected build packaged separately under E:\Test builds\Modlist-Diagnostics-20261105-folder-fix.
- Follow-up speed-only request: remove source_sha256_read and per-file post-copy/post-extraction hashes. Keep metadata-only checks, without treating accounted file sizes as bytes actually read. Archive/embedded-tool hashes and real pipeline block validation are unchanged. New regression first failed on the old source hash stage; tests now check absent source hashing, zero payload reads in metadata stages, retained archive hashing, acceptance of a same-size mutation, and rejection of a truncated copy.
- Verified speed-only build: workload/core/UI/integration suites passed. Packaged EXE --smoke --reverse exited 0, performed all four extractions, and every metadata stage recorded zero process read bytes. Report: E:\Test builds\Modlist-Diagnostics-20261105-no-file-sha\logs\ModlistBench-2026-11-05_12-11-59-16052-12757887974744242933. EXE SHA-256: 6FC6C19E74D405F5335B22AF08E84FDF874481E217B1295383148C91CA29721F. No full-size benchmark or Git operation performed for this change.
- Disk-only follow-up: replace editable folder paths/Browse with available local disk combos; auto-use X:\ModlistDiagnostics plus unique backend run IDs. Source defaults to EXE drive, destination to the other available drive with most free space. Default size changed to 10 GiB; CLI folder arguments retained. A regression first failed on the old 20 GiB default; unit checks also enforce disk-root mapping and rejection of arbitrary folders/UNC roots.
- Disk-only verification: workload/core/UI/integration tests passed. Desktop screenshot and interactions confirmed E:/D: selectors, 10 GiB default, and same-disk rejection. Starting from the GUI created E:\ModlistDiagnostics and D:\ModlistDiagnostics with unique run directories; Stop returned to enabled controls and recorded cancellation. This short GUI check retained a 2 GiB partial source file, not a completed 10 GiB run. Packaged under E:\Test builds\Modlist-Diagnostics-20261105-disks-10gb; SHA-256 861E198739C8B2966E04DDECF4C8CF976A15480FEAAF33F9F4E18854A7D1081C. No Git operation requested/performed.
- Tester report 22:21:53: source Toshiba HDD E:, destination Kingston NVMe C:, 16 GiB RAM. Copy completed (bulk 69.74 MiB/s, small 6.51 MiB/s); archive creation cancelled after 929.26s. Late samples show about 1.55 MiB/s input reads despite mostly zero output writes. Root cause of misleading 0 MiB/s display: compression reused an output-write proxy against an uncompressed-input total. Reproduced with failing integration assertion. Fix uses input reads for compression, preserves output writes for native extraction, exposes CPU/RAM/read/write activity, and logs 15s heartbeats. Counter resets are clamped; child progress is capped below 100% while still running. Compression settings/placement and installer extraction code are unchanged.
