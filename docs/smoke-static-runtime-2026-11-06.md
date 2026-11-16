# Static-Runtime Build Smoke Test

Date: 2026-11-06. Tested the uncommitted build containing F01-F05 fixes.
No production code, version, commit, release or tracked distribution EXE changed
during this smoke run.

Artifacts: `E:/Test builds/Modlist-Smoke-20261106`.
Disposable installed outputs: `D:/MI-Smoke-CRT-20261106`.

## Fixture And Product Binaries

Generated 1,114,587,910 bytes (1.04 GiB), 10,020 files and 168 directories:
16 unique random 64 MiB bulk files, 10,000 small files, an empty file, nested
empty directories, Cyrillic names and two legitimate ownership-like filenames.
No existing user archive or game installation was used as test input.

The copied portable product EXEs match the preceding static-runtime build:

- Installer SHA-256: `814B648E02246B4CCE0DE67688EDAFE1062A30F5D21A264F91BB615F328632A7`.
- Packer SHA-256: `11079A73E9FCEAE5784BB492F07B2B59D860C7AD8DD4E5D7A360251D492A82FE`.

The product EXEs and embedded 7-Zip source pair passed the runtime-import gate
again. The actual product packer was launched and operated through its UI with
the generated source, default compression settings and a fresh release folder.
It compressed, tested and published `release/SMOKE-CRT`, containing only the
archive volume, manifest and byte-identical current installer. The manifest's
payload byte count matches the generated source.

## Functional Results

- PASS: real production-worker direct and staged installation of the full fixture.
- PASS: both full output trees match independent .NET SHA-256 checks and exact
  directory sets, including empty directories. Successful staging was removed.
- PASS: five further full trees from two native 7-Zip runs, two pipeline runs and
  a fresh run after cancellation matched the same source independently.
- PASS: pipeline cancellation at 35% returned cancelled, not successful; the
  subsequent fresh install succeeded.
- PASS: one changed byte in a copied first-block corruption fixture was rejected.
- PASS: another isolated copy changed byte 134,230,073 in the third 64 MiB block.
  Both actual worker routes reported SHA-256 failure, validation failure and no
  installation success. The original archive's full SHA-256 was unchanged.
- PASS: fresh packer-generated ZIP and Cyrillic packages installed through direct
  and staged workers, with independent full content/directory checks.
- PASS: core, scheduler, 31 cleanup, native UI and diagnostics unit suites.
- PASS: all 43 packer cases, using the real current sibling installer where supported.
- PASS: diagnostics integration, including both extraction orders, cancellation,
  invalid paths and truthful report-write failure.
- PASS: the complete smoke-helper suite, including forced termination, copy failure,
  reordered/leading-space descriptions and original-input preservation.

## Live Installer UI

The packer-generated actual installer opened with its manifest detected, visible
bottom controls and a `logs/modlist-installer.log` beside the EXE. Both game
Documents folders already existed before the run. After explicit approval, the
UI installed into `D:/MI-Smoke-CRT-20261106/ui-success/SMOKE-CRT`.

Its real path inspection counted 10,188 entries, maximum final length 78 against
the 240-unit limit, and selected direct extraction. Progress reached 100%, the
themed "Установка завершена!" message appeared with centered OK, OK dismissed it,
and the main "Закрыть" button closed the installer. No test application windows
remained after closing.

PASS: the GUI-created tree also matched all 10,020 source files byte-for-byte
through independent SHA-256 checks, and all 168 directories matched. The result
is recorded in `reports/ui-content-verification.json` and `reports/ui-result.json`.
The actual production log is retained as `reports/ui-modlist-installer.log`.

## Evidence And Limits

See `reports/fixture.json`, `reports/package-check.json`, `worker-large/`,
`worker-zip/`, `worker-unicode/`, `pipeline-smoke/`, `reports/corrupt-middle.json`
and the raw suite logs under `reports/`.

These are functional smoke tests on this PC, not clean-machine qualification.
No clean Windows VM, real UNC share or affected tester PC was used. Native/pipeline
timings have warm caches, no cache flush and different validation work; they are
not a general speed comparison. Retained test data has not been recursively cleaned.
The existing nonblocking depth warning was logged for the nested test destination,
even though the adaptive planner found every complete path within its budget;
installation and content verification still passed. This warning was not changed.
