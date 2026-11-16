# Modlist Packer

Native Windows helper for building modlist release folders.

It provides a focused 7-Zip-style archive settings window, runs embedded 7-Zip, tests the archive when requested, then writes a chunked SHA-256 manifest beside the archive parts.

`modlist-packer.exe` is portable. Keep `modlist-installer.exe` beside it. Both
executables embed their 7-Zip tools and do not require a separately installed
VC++ Redistributable.

## Release Layout

```text
SelectedReleaseParent\
  MyPack\
    modlist-installer.exe
    manifest.json
    MyPack.7z.001
    MyPack.7z.002
```

The child folder is named from the archive name field, without an added archive extension. A new package contains only the installer, manifest, and archive file(s), with no `data`, `ui`, `tools`, or log directories. Manifest archive paths are UTF-8 plain filenames relative to this child folder, including non-ASCII archive names.

## Manifest

The manifest stores:

- archive file path
- archive file size
- full-file SHA-256
- per-chunk SHA-256
- chunk size
- total unpacked payload size

The default chunk size is 64 MiB.

The matched 7-Zip 26.02 `7z.exe`, `7z.dll`, and `7zip-License.txt` are embedded inside `modlist-packer.exe` and extracted together to a per-user cache when archive creation starts. Every component is compared byte-for-byte with its embedded copy before reuse; a missing file or mismatch, including same-size corruption, causes regeneration. A repair/write failure aborts the build. The packer does not require an installed 7-Zip or registry-discovered codec DLL. Resource provenance and checksums are in `resources/7zip/README.md`.

Only `Relative paths` is supported. Full paths (`-spf`, including `-spf2`) embed absolute source paths that the installer rejects. Full/unknown path modes and full-path switches in additional parameters are rejected before package output is created.

The default compression profile matches the recommended release settings:

- level `5`
- method `LZMA2`
- dictionary `32m`
- word size `32`
- solid block `8g`
- threads `20`
- split volumes `4092m`

The RAM limit defaults to 80% of physical memory and is enforced on the 7-Zip process with a Windows Job Object. The packing status reports 7-Zip's own percentage together with live process read throughput, ETA, elapsed time, and RAM use.

`Build Package` preflights configuration, source, embedded tools, sibling installer, and replacement free space. It builds the archive parts, installer, and manifest in a fresh owned `.packer-<process>-<tick>-<sequence>` sibling workspace on the selected release volume. No separate installer assets or browser runtime are copied.

The previous package is left intact while the replacement is compressed, optionally integrity-tested, and hashed. The new manifest is explicitly closed and checked before publication. Only then are selected-name `.7z`/`.zip` archives, numeric volumes, their interrupted `.tmp` files, the manifest, and an installer being replaced moved into the owned workspace's `previous` backup. New files are published with the manifest last. Discovery and promotion both accept case-only archive-name/extension differences using Windows case-insensitive path comparisons. Failed moves or cancellation during publication trigger rollback; unexpected destination files are never overwritten. Other releases, parent files, unrelated archives/files, and existing directories (including legacy `data`) stay in place. The old package directory is never recursively deleted. Source and release folders cannot be the same folder or contain one another.

Replacement requires additional free space while the previous archive still exists: the source-size estimate, the installer size when copied, and the greater of 16 MiB or 6.25% of source size. If this reservation is unavailable, the build fails without removing the previous package. Compression or write errors after that check also preserve the previous package; the check is a conservative estimate, not a guarantee against other disk users consuming space.

Successful publication or complete rollback removes only that run's owned workspace. If rollback cannot restore a file, the log prints `RECOVERY REQUIRED` and the exact retained path. Its `promotion-journal.txt` identifies live/new/previous paths, `previous` contains old files not yet restored, and `replacement` contains new files not yet published or rolled back. Preserve that workspace and inspect both live and backup files before manual recovery. Redirected or inaccessible workspace entries prevent recursive cleanup. Publication is a recoverable sequence of file moves, not a crash-atomic directory swap or a guarantee against concurrent external writers; do not rebuild/install the same release concurrently.

The status line reports source scan file count, accumulated size, and elapsed time. During 7-Zip packing and testing, live process metrics continue updating every second even when the integer archive percentage has not changed. Worker failures finish with `Package build failed` and reset progress to zero after rollback/cleanup; cancellations finish with `Stopped` and zero progress. A completed staged manifest alone is not a ready package.

`Manifest Only` uses the same selected release parent and archive-named child. It scans that child's root for an exact archive file or numeric split volumes such as `MyPack.7z.001`, ignores side files like `.tmp` and logs, and writes `archive_name` from the real detected archive name. It hashes the live archives in place and stages only the new installer/manifest pair in an owned sibling workspace. No archive copies or hardlinks are made, and archive parts/temps never enter its backup or promotion set. The original installer and manifest remain intact on close failure, cancellation, or failed publication; copying-disabled mode leaves the existing installer untouched. Its free-space preflight reserves the copied installer size plus 16 MiB for manifest metadata, without reserving archive-copy space. Both modes write a uniquely reserved temporary manifest and explicitly close/check it before transactional publication. If multiple archive sets exist in that child, the archive name must identify one unambiguously.

Manifest hashing auto-selects sequential HDD reads or parallel SSD reads, with a measured 4 MiB per-worker buffer.

## Build

```powershell
cd PackerApp
.\scripts\build-release.ps1
```

The script builds and runs the packer tests, then copies the packer and the already-built `InstallerApp\dist\modlist-installer.exe` to:

```text
PackerApp\dist\
  modlist-packer.exe
  modlist-installer.exe
```

Build the installer separately first. The packer script does not build or modify `InstallerApp`, and does not remove old `dist` files or directories.

## Tests And Small Fixture

```powershell
cd PackerApp
xmake f -m release
xmake build packer_tests
xmake run packer_tests
```

The console tests exercise the production setup, Worker, embedded compression, archive integrity testing, extraction, full/chunk hashing, Manifest Only, overlap validation, and scoped cleanup on small files. Replacement regressions compare all seeded previous package bytes after prerequisite/cache/compressor/write failures, cancellation before/during compression and during publication, and publication/rollback failures. They include real Windows sharing violations, non-clobbering moves, sparse-file space rejection, case-only archive names/extensions, explicit recovery-artifact checks, and final status/progress assertions. Manifest Only tests lock live archive parts/temps against writes/deletes, inspect its staged pair to exclude archive copies/hardlinks, and verify installer/manifest preservation on close failures, cancellation, real manifest locks, and publication failures. Close and rename failure injection is per Worker invocation, with real compression/hash/publication orchestration, not global success switches. Other regressions cover real Cyrillic packing and UTF-8 manifest roundtrips, independent JSON/full/chunk hash verification, actual relative/`-spf` archive metadata, config/parameter rejection, same-size cached EXE/DLL/license tampering, and long log output. The JSON roundtrip uses Windows PowerShell's built-in JSON parser and .NET SHA-256, without extra modules.

Each run uses a unique marked tree under the printed `build\windows\x64\release\test-fixtures\flat-layout-...` path and a private tool cache. By default, that run's tree is removed after tests, including failed tests. Cleanup does not sweep older runs or the parent directory; unrelated files/directories are preserved. Redirected paths, reparse points, and missing/changed ownership markers cause cleanup to refuse deletion. Tests without an installer source use an inert installer placeholder for the ASCII package.

To recreate a launchable flat package with the current installer:

```powershell
xmake run packer_tests --keep-fixtures --installer-source dist
```

`--keep-fixtures` also works alone, and either option order is accepted. `--installer-source` alone still cleans up after testing.

Open `compression\releases\My Pack\modlist-installer.exe` under the printed fixture root for native installer E2E when an actual installer source was supplied. Without it, this EXE is an inert text placeholder and must not be treated as runnable. `compression\releases\My Pack` contains a real split 7z package; `zip-compression\releases\My Pack` contains a real unsplit ZIP package without an installer. Both contain an empty file and directory in addition to nested payload files. The Unicode E2E package is under `cyrillic-compression`, in its Cyrillic-named release parent and archive child; with `--installer-source`, it also contains the supplied installer. Its UTF-8 manifest points to the real Cyrillic archive filename, and the archive contains a Cyrillic nested payload with known SHA-256 hashes. Sources, independently extracted payloads, archive metadata listings, and the JSON verification output are retained for comparison. Controls are tested in a hidden native window; no visible UI automation or large benchmarks are run.
