# Verified Extraction Smoke Test: 2026-11-06

Overall outcome: resumed 10 GiB backend smoke test PASSED, including independent
payload verification. The original interrupted attempt remains documented below.

## Scope

Exercise the corrected read-ahead scheduler on a realistic multi-volume workload,
independently verify extracted payloads, and inspect the actual native installer.
Keep archive integrity checks enabled. Do not commit, publish or change decoder,
compression, output allocation or validation policy.

Test installer SHA-256:
`A69D2C5399CB6E078F5C6F240D8702C10DAFA2CB19ACC0455E4FAC0B1DCF8474`.

## Large Workload

- 10 GiB; 200,006 files, including 200,000 small files; Unicode names and 400
  explicit empty directories.
- Source: Seagate ST2000DM008 SATA HDD, E:.
- Destination: Kingston SFYRD2000G NVMe SSD, D:.
- Four decoder threads, 64 MiB SHA-256 chunks; six-buffer scheduling limit.
- Run pipeline/native, then native/pipeline, sequentially on the same archive.
- Windows caches are not flushed; no cold-cache or old-versus-new speedup claim.
- Full per-file SHA-256 comparison uses an independent .NET implementation after
  the timed stages, not inside one extractor's measured interval.

Run identifier: `ModlistBench-2026-11-06_00-11-07-53028-6243007614738809157`.

Report folder:
`InstallerApp/build/windows/x64/release/logs/ModlistBench-2026-11-06_00-11-07-53028-6243007614738809157`.

Status: interrupted after Computer Use was stopped by the physical Escape key.
The owned benchmark process was then stopped; generated files were retained.
Generation, bulk copy, small-file copy and metadata verification completed.
Archive creation had not finished, so neither large extraction comparison nor
large independent payload verification ran. The partial archive is not a usable
installer package and must not be presented as a successful smoke-test result.

## Live Installer

Generated fixture: `E:/Test builds/Smoke-UI-20261106`.
64 MiB, 515 files, 5 archive volumes, Unicode names and empty directories.

| Case | Result |
| --- | --- |
| Automatic flat-package discovery | Passed |
| Direct installation into selected root plus archive name | Passed |
| Successful completion notification | Observed; dialog opened and dismissed normally |
| Close button after success | Passed; process closed normally |
| Independent SHA-256 of every installed file | Passed: 515 files, 67,108,864 bytes |
| One changed byte in copied third volume | Rejected: `SHA256 mismatch in Smoke.7z.003, block 1` |
| Themed validation failure notification | Passed; correct torrent-recheck message and centered OK |
| Escape dismisses failure notification | Passed; returned to responsive main window |
| Live Stop-button test | Not run: Computer Use stopped before the start action |

The success run was started while confirmation was being exchanged; completion
was observed through the actual UI and its log. No automated click is claimed
for that first start. The corruption test was started via Computer Use after
explicit user confirmation. Original archives were not modified.

UI output payload report:
`E:/Test builds/Smoke-UI-20261106/success-payload-verification.json`.

## Reproduction Helpers

`InstallerApp/scripts/prepare-smoke-ui.ps1` splits a known single-volume 64 MiB
fixture into five volumes and prepares independent valid/corrupt copies. It
refuses to overwrite existing fixtures.

`InstallerApp/scripts/verify-smoke.ps1` compares complete directory/file sets,
sizes and SHA-256 payload hashes. Its self-test passed exact-match, same-size
corruption, extra-file and missing-empty-directory cases. Reports use new files
only, and source/destination payloads are read-only.

No production code was changed for this smoke test.

## Other Completed Checks

- Ten consecutive real-archive core-suite runs passed, including cancellation,
  corruption, output-open failure, split volumes and unused-tail validation.
- Nine deterministic production-scheduler cases passed.
- Native UI suite passed, including completion/minimized-window/modal cases.
- Diagnostics workload/configuration suite passed.

The original 10 GiB attempt is incomplete. Its extraction timings are not used.
The successful 64 MiB live-UI result and its independent payload verification
remain valid. The resumed backend pass is recorded below.

## Resumed Backend Pass

On the user's "try more", reused the existing 10 GiB SSD copy to prepare a new
archive on E:. This changes preparation only. All extraction inputs still came
from E: and all outputs went to D:. No desktop automation was used in this pass.

Archive: 5,192,047,444 bytes, two volumes, 78 SHA-256 chunks. The first volume has
a short 60 MiB boundary chunk; the final volume also has a short final chunk.
The full payload contains 200,006 files and 2,404 directories, including 400
explicit empty directories. Chunk size is 64 MiB and decoder budget is four.

| Case | Seconds | Output MiB/s | Result |
| --- | ---: | ---: | --- |
| Pipeline, first in pair 1 | 38.236 | 267.81 | Passed |
| Native 7-Zip, second in pair 1 | 48.474 | 211.25 | Passed |
| Native 7-Zip, first in pair 2 | 41.755 | 245.24 | Passed |
| Pipeline, second in pair 2 | 35.067 | 292.01 | Passed |
| Pipeline cancel at 35% | 7.726 | Not applicable | Expected cancellation; 3,521,118,208 bytes written |
| Fresh pipeline run after cancellation | 33.969 | See JSON | Passed |
| One changed byte in final volume | 23.747 | Not applicable | Expected SHA-256 rejection |

The altered byte was restored in a finally block and the complete volume's
SHA-256 was checked against the original manifest. Missing-volume, wrong-size,
and unsafe-path cases also failed as expected, before creating output folders.

Both successful matched pipeline runs verified all 5,192,047,444 compressed
bytes and wrote all 10,737,418,240 payload bytes. Each loaded 79 chunks: 78 unique
chunks plus one header/tail-seek reload. Aggregate demand waits were 0.148 s and
0.148 s; these overlap other work and are not additive stage costs. OS read-byte
counters are not measurements of physical disk traffic.

This is a warm-cache comparison on this host, not a measured old/new scheduler
speedup or a whole-installer UI timing. DLL timing encloses Extract; native timing
also includes process startup/exit. Both use 7-Zip 26.02 x64 and four threads. The
loaded DLL hash matches the bundled resource DLL. Full payload verification is
outside every extraction timer.

The test-only callback initially wrote synchronous progress logs; small contract
runs showed highly variable elapsed times. That logging was removed before the
large comparison. The quiet contract completed in 0.550 s. No production
extractor or UI behavior was changed for this adjustment.

### Independent Payload Verification

PASSED. Hashed all 200,006 original generated HDD files (10,737,418,240 bytes),
then checked the SSD copy and all five successful extraction trees, including the
run after cancellation. All six trees have identical file/directory sets,
lengths and full SHA-256 payloads. This is 1,200,036 destination file comparisons
and 64,424,509,440 destination bytes (60 GiB), in addition to the original 10 GiB.
All 2,404 directories, including the 400 explicit empty directories, match.

The original-file reference hashing took 680.029 s; destination checks took
3.194-12.465 s per tree. These are test-only verification durations outside
extraction timing, not the installer's normal validation behavior. The installer
continues to validate compressed archive chunks, not every extracted file.
Filesystem timestamps, attributes and ACL equivalence were not asserted.

The verifier uses pooled buffers. Its self-tests reject corruption in later
buffers and short tails, missing empty directories and extra files. A PowerShell
array-expression precedence error in the extended self-test was corrected before
the large verification started; the corrected self-test and full run passed.

### Artifacts

- Ready test package: `E:/Test builds/Smoke-Resume-20261106/package`.
- Extraction/cancellation/corruption report:
  `E:/Test builds/Smoke-Resume-20261106/reports/large/extraction-results.json`.
- Negative cases:
  `E:/Test builds/Smoke-Resume-20261106/reports/large/negative-cases.json`.
- Independent full-payload verification:
  `E:/Test builds/Smoke-Resume-20261106/reports/payload-verification.json`.
- Source payload: `E:/Test builds/Smoke-20261106/ModlistBench-2026-11-06_00-11-07-53028-6243007614738809157/data`.
- Successful outputs: `D:/MI-Smoke-Resume-20261106/large`.

Test-only runner: `InstallerApp/tests/pipeline_smoke.cpp`, with matching xmake and
CMake targets. Preparation/orchestration helpers are
`InstallerApp/scripts/new-smoke-manifest.ps1` and
`InstallerApp/scripts/run-existing-smoke.ps1`. Runtime settings and production
source files were not changed. No commits, pushes or releases were made.

### Final Gates And Limits

- All-target Windows Release build passed.
- Core, scheduler, native UI and diagnostics unit suites passed fresh runs.
- Diagnostics integration passed both extraction orders, cancellation, report
  write failure and invalid-folder cases. Report:
  `InstallerApp/build/windows/x64/release/logs/ModlistBench-2026-11-06_00-40-42-41012-15361970943956639904`.
- Extraction summary assertions passed all seven expected exit codes and archive
  restoration. Payload summary assertions passed all six complete trees and
  exact 10 GiB / 200,006-file / 2,404-directory source totals.
- Ready test package contains only the installer, manifest and two archive parts.
  The deliberately invalid manifest fixtures are retained under reports.
- Installer checksum remains
  `A69D2C5399CB6E078F5C6F240D8702C10DAFA2CB19ACC0455E4FAC0B1DCF8474`.
- Large-workload GUI completion and the physical Stop-button path were not
  exercised in this backend-only resumption. No affected-tester PC result or
  old/new scheduler performance comparison is claimed.
- Generated source, outputs, cancellation/corruption partial outputs and reports
  are retained in their isolated test folders. No existing user data was removed.

## Tooling Review Fix Verification

This later pass addresses R1-R3 from the review of `0e6c612`. It does not change
the production scheduler, decoder thread budget, validation guarantees or UI.
The earlier large-workload measurements above remain historical results, not
fresh results from this pass.

### Source Safety And Archive Selection

`run-existing-smoke.ps1` now copies every referenced archive into a fresh
corruption fixture and mutates only a newly created destination file handle.
The original inputs are opened read-only; no restoration is needed after a
crash. Copy failure stops the helper with an error. This full copy needs extra
space equal to the archive size below the reports folder.

`pipeline_smoke --describe-package` uses the real `ManifestLoader` without
extracting or writing files. Both comparison paths use its first-part selection,
independent of manifest entry order. Explicit legacy first-part selection is
honored by description, but non-chunk legacy input is rejected by this pipeline
benchmark before creating output/report folders.

The description uses JSON to preserve whitespace and UTF-8 in archive names.
Readiness markers in the test driver are published atomically after their
metadata is closed, so a forced-stop test cannot observe incomplete metadata.

Regression tests used a disposable 64 MiB split archive (nine volumes). Passed:
normal success/SHA rejection, reversed `files[]`, leading-space archive names,
injected corruption-run failure, a source-sharing violation that prevents fixture copying, forced
termination during corrupt extraction, explicit legacy first-part description,
and invalid explicit first-part rejection. All original input file hashes,
including each archive volume and manifest, were unchanged after every run.
Only the copied fixture contains the deliberate one-byte change. Copy failure
never starts the corrupt extractor or falls back to the original package.

Before the fixes, the source-safety test failed because the corrupt extractor
received the original manifest, and the reordered-manifest test failed with a
native 7-Zip exit code of 2. Both now pass. Fixtures and logs are retained at:
`C:/Users/user/AppData/Local/Temp/mi-smoke-helper-d44208381f4743a79ce62a3ca194d1f2`.

### Diagnostics Callback And Packaged EXE

Extraction callbacks now publish memory-only progress. The sampler copies the
snapshot under its lock, releases the lock, then logs it. Tests deliberately
block both a log mutex and the sampler's sink while the callback publishes;
publishing/cancellation still complete before log delivery is released. Final
partial/completed progress is retained. The pre-fix callback failed the blocked
sink test. Another pre-fix integration test showed final delivery extending the
measured stage; finish time and process totals are now frozen before draining.

Packaged EXE:
`E:/Test builds/Modlist-Diagnostics-20261106-review-fixes/modlist-diagnostics.exe`.
SHA-256: `77AB8E6EEA24978FAA1AC88F77723459633D0BC2D3D15E74A2DCD5220FA4C197`.
Three CLI smoke runs used E: source and D: output, 64 MiB/515 files, four decoder
threads and both extraction orders. Each then ran the quiet pipeline against
the exact same archive/manifest. All final counts and compressed validation
totals agreed. An independent external SHA-256 comparison checked the copy,
four diagnostic extraction trees and quiet output against the source in each
run (18 trees total), including file/directory sets and empty directories.

| Run | Diagnostic pipeline 1/2 (s) | Native 1/2 (s) | Quiet pipeline (s) |
| --- | ---: | ---: | ---: |
| 1 | 0.604 / 0.602 | 0.824 / 0.664 | 0.570 |
| 2 | 0.625 / 0.562 | 0.726 / 0.612 | 0.606 |
| 3 | 0.590 / 0.574 | 0.593 / 0.609 | 0.592 |

These are short warm-cache functional checks, not a reliable speedup estimate.
Other small integration tests were running on the host during part of the pass.
Each EXE report folder contains `results.json`, `quiet-metrics.txt` and
`independent-content-verification.json`. Logs are below the packaged EXE's
`logs` folder. Generated data remain below
`E:/Test builds/ReviewFix-Packaged-20261106` and
`D:/MI-ReviewFix-Packaged-20261106`.

The all-target xmake Release build and core, scheduler, UI, diagnostics unit,
diagnostics integration and all 20 packer tests passed. Integration covers native
and pipeline API cancellation, delayed final progress, report-write failure and
invalid paths. The external SHA verifier self-test also passed. Cancellation in
the packaged desktop GUI was not exercised. No full 10 GiB workload or affected
tester-PC performance result is claimed. The distributed installer remains
unchanged, with SHA-256
`A69D2C5399CB6E078F5C6F240D8702C10DAFA2CB19ACC0455E4FAC0B1DCF8474`.
No commit, push or release was made for these tooling fixes.
Independent review found two additional small defects (whitespace decoding and
early readiness publication), both fixed before the final verification. The
follow-up review found no remaining actionable issue. Description tests also
round-trip a legacy explicit first part containing whitespace, a JSON control
escape and UTF-8. Mid-copy termination, cancellation after nonzero diagnostic
output and a blocked sampler inside the real Stage finish path were not directly
injected; callback/snapshot isolation is covered by the blocked-sink unit tests.
