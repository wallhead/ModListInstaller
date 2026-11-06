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
