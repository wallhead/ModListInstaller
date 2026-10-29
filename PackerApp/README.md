# Modlist Packer

Native Windows helper for building modlist release folders.

It provides a focused 7-Zip-style archive settings window, runs embedded 7-Zip, tests the archive when requested, then writes a chunked SHA-256 manifest beside the archive parts.

`modlist-packer.exe` is portable. Keep the self-contained `modlist-installer.exe` beside it. These two executables are the only required files; the installer contains its own UI and extraction tools.

## Release Layout

```text
SelectedReleaseParent\
  MyPack\
    modlist-installer.exe
    manifest.json
    MyPack.7z.001
    MyPack.7z.002
```

The child folder is named from the archive name field, without an added archive extension. A new package contains only the installer, manifest, and archive file(s), with no `data`, `ui`, `tools`, or log directories. Manifest archive paths are plain filenames relative to this child folder.

## Manifest

The manifest stores:

- archive file path
- archive file size
- full-file SHA-256
- per-chunk SHA-256
- chunk size
- total unpacked payload size

The default chunk size is 64 MiB.

7-Zip is embedded inside `modlist-packer.exe` and extracted to a per-user cache when archive creation starts. The packer does not ask for an external `7z.exe`.

The default compression profile matches the recommended release settings:

- level `5`
- method `LZMA2`
- dictionary `32m`
- word size `32`
- solid block `8g`
- threads `20`
- split volumes `4092m`

The RAM limit defaults to 80% of physical memory and is enforced on the 7-Zip process with a Windows Job Object. The packing status reports 7-Zip's own percentage together with live process read throughput, ETA, elapsed time, and RAM use.

`Build Package` creates the archive-named child of the selected release parent and writes the archive parts, `manifest.json`, and sibling installer executable directly into it. No separate installer assets or browser runtime are copied.

Before a build, the packer removes only older `.7z`/`.zip` archive files and numeric split volumes for the selected archive name, their interrupted `.tmp` files, and the previous manifest within that child. Other releases, parent files, unrelated files, and existing directories (including legacy `data`) are preserved, not silently migrated or deleted. Source and release folders cannot be the same folder or contain one another, preventing the release output from being archived recursively.

The status line reports source scan file count, accumulated size, and elapsed time. During 7-Zip packing and testing, live process metrics continue updating every second even when the integer archive percentage has not changed.

`Manifest Only` uses the same selected release parent and archive-named child. It scans that child's root for an exact archive file or numeric split volumes such as `MyPack.7z.001`, ignores side files like `.tmp` and logs, and writes `archive_name` from the real detected archive name. It refreshes the installer and manifest without deleting archive parts. If multiple archive sets exist in that child, the archive name must identify one unambiguously.

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

The console tests exercise the production setup, worker, embedded compression, archive integrity testing, extraction, full/chunk hashing, Manifest Only, overlap validation, and scoped cleanup on small files. Fixtures are retained under the printed `build\windows\x64\release\test-fixtures\flat-layout-...` path. Tests without an installer source use an inert installer placeholder.

To recreate a launchable flat package with the current installer:

```powershell
xmake run packer_tests --installer-source dist
```

Open `compression\releases\My Pack\modlist-installer.exe` under the printed fixture root for native installer E2E. That folder contains the real installer, `manifest.json`, and three small split archive parts. The fixture's source and independently extracted payload are retained alongside `releases` for comparison. No UI automation or large benchmarks are run.
