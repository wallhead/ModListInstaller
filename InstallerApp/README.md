# Modlist Installer

Native Windows installer for local modlist release folders produced by `PackerApp`.

## What Is Included

- CMake C++ project structure.
- Manifest JSON parser and validator.
- Skyrim-safe path validation.
- SHA256 verifier for legacy manifests and chunked packer manifests.
- Asynchronous verified-block pipeline into the 7-Zip SDK, with the command-line extractor retained as fallback.
- Dependency-free tests.
- Example manifest.

## Build With xmake

```powershell
cd InstallerApp
xmake
xmake run installer_core_tests
xmake run pipeline_scheduler_tests
```

The GUI executable is built as:

```text
InstallerApp\build\windows\x64\release\modlist-installer-gui.exe
```

Place it beside `manifest.json` and the archive parts in an archive-named folder. Theme, translations, 7-Zip, and its license are embedded in the executable. The GUI does not use torrent files.

Windows builds statically link the Microsoft C/C++ runtime; end users do not need
the VC++ Redistributable for the installer, packer or embedded 7-Zip tools. xmake
uses `MT` consistently, including the optional release-built static dependencies;
CMake uses the static runtime matching its Release/Debug configuration. Release
scripts check the actual EXE and embedded-tool DLL imports before distribution.
The check can also be run directly with
`scripts/test-runtime-dependencies.ps1 -Binary <exe-or-dll>` (requires Visual C++
build tools on the build machine, not the end user's PC).

The verified SDK path supports 7z and ZIP, split or unsplit, using the matching
format handler. Hash metadata alone does not establish format support. Archive
open and extraction must return `S_OK`; correctly hashed nonarchives and failed
handler opens cannot be reported as successful empty installations. Genuinely
opened empty archives remain valid.

The `Install` button runs the install pipeline on a background thread:

- load `manifest.json` beside the executable (or the legacy `data\package\manifest.json` layout)
- inspect archive entry paths before extraction and choose the extraction folder
- validate the chosen extraction and install folders
- resolve the Windows Documents known folder and create missing `My Games\Skyrim Special Edition` and `My Games\Fallout4` folders
- check the manifest's unpacked payload size plus extraction overhead against free space
- for chunked packer manifests, read each archive block once, verify it asynchronously, and expose it to the 7-Zip SDK only after SHA256 succeeds
- advance four-block read-ahead on cached reads too, prioritise demand after seeks, and keep cached, borrowed and loading data within six chunk buffers; seeks may require rereading an evicted block
- show simultaneous validation and extraction percentages, validation speed, ETA, and elapsed time
- retain separate sequential HDD or parallel SSD validation followed by command-line extraction for legacy manifests or unavailable SDK components
- look for the archive named by the manifest beside the executable
- check unpack free space again before extraction; same-volume installs do not reserve a second full copy
- show live unpack percentage, speed, and ETA in the progress bar and status line
- refresh unpack elapsed time every second, count ETA down between percentage changes, and map split archives across the full unpack progress range
- show live install percentage, speed, ETA, and elapsed time in the status line
- install from the unpack folder into the final install folder, using same-drive move/cut semantics instead of copying when both folders are on the same drive
- extract embedded 7-Zip tools and license into the package-specific Local AppData cache when needed
- stream full 7-Zip diagnostics to `logs` beside the executable while keeping only a small in-memory tail for the GUI
- run 7-Zip inside a memory-limited child process so oversized archives fail cleanly instead of exhausting system RAM
- extract with bundled 7-Zip into the selected install folder

The GUI is a single native Direct2D/DirectWrite installer screen with the install root, final install folder, progress, and log output.

The app reads archive entry names before extraction. If every complete destination path is at most 240 UTF-16 code units (including the selected root, archive folder name, separators, internal folders, and filename), it extracts directly into `<selected root>\<archive_name>` and skips the final move. Otherwise it stages at `<drive>:\Unpacking` and moves the files into the final folder. If archive metadata cannot be inspected, it also uses staging. Inspection reads archive metadata without unpacking the payload, and the UI remains responsive during this check.

For example, choosing `D:\Folder1\folder2\folder3` always installs into `D:\Folder1\folder2\folder3\<archive_name>`; staging on `D:` is used only when needed. Direct installs do not create or clean the unused `D:\Unpacking` folder. Failed or cancelled direct installs leave files in the final folder, and retrying asks before cleaning it. The path budget is a conservative compatibility policy, not a guarantee that every game or external tool supports the final paths.

The chosen extraction and install folders must be empty before installation begins. On different volumes, the installer also requires enough destination space for the full unpacked payload. Manifests without `unpacked_size` remain supported and use the archive size as an approximate compatibility fallback.

Staging ownership is recorded in a locked `staging-owner\owner` file in the
stage-specific Local AppData runtime cache, not inside or beside the payload.
This avoids drive-root file permissions and collisions with valid archive names.
Claims are persisted as pending before the final empty/identity check, then committed.
Pending or truncated records never authorize cleanup; nonempty pending claims need
manual inspection. Installation and cleanup refuse overlap with the runtime cache.
Only leftovers with matching package/directory ownership are eligible for staging
cleanup; arbitrary existing `Unpacking` contents are never adopted as installer
files. A selected final folder can still be overwritten after explicit confirmation.
Cleanup refuses redirected roots/ancestors, volume roots, nested links and changed
folder identities. An active staging session cannot be cleaned by another session.
Successful staged installation removes its owned empty staging folder and ownership metadata.
If a move leaves behind only locked empty directories, installation succeeds with
a log warning and keeps the ownership record for later cleanup. Leftover files
or an unsafe staging tree still fail finalization.
Old unmarked staging contents require manual inspection rather than automatic deletion.

## Ready Binary

The ready exe is placed at:

```text
InstallerApp\dist\modlist-installer.exe
```

The Windows installer statically links the Microsoft C/C++ runtime. It
does not require a separately installed VC++ Redistributable or WebView2.

The runtime layout is:

```text
YourPack\
  modlist-installer.exe
  manifest.json
  YourPack.7z.001
  YourPack.7z.002
```

On startup, the installer creates `logs\modlist-installer.log` beside its executable. Extraction diagnostics and `last-7z-output.log` are also saved in `logs`. The current log path is shown in the installer and accessible using Open Log. Extracted tools remain in `%LOCALAPPDATA%\ModlistInstaller\<package key>`, where the key identifies the package's absolute folder. No external UI files are needed. Do not commit generated logs.

## Release Build Script

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The script configures xmake for release, rebuilds embedded assets, runs tests, and copies only the GUI executable into `dist`. No browser runtime is required.

## Performance Diagnostics

Build the separate portable diagnostic utility with `scripts\build-diagnostics.ps1`.
It generates 10/20/30 GiB, measures cross-disk bulk/small-file copying, and compares
the real verified extraction pipeline against ordinary 7-Zip using the same
archive and thread count. Output checks use file/directory names, types, counts,
and sizes only, with no per-file SHA-256 pass. The pipeline's archive-block
SHA-256 validation remains enabled.
See [diagnostic instructions](src/diagnostics/README.md) for disk-space requirements,
cache/timing caveats, retained data, and the logs to collect. It is not part of a
normal installer release.

For a small packer-to-installer regression, build `installer_workflow_tests` and
run `scripts\test-install-worker.ps1` with a flat package, its source payload and
fresh work/output parents. It runs the actual install worker in direct/staged
modes without desktop automation, then compares complete payload hashes and empty
directories outside the worker. It does not touch real Documents/game folders.

For the installer finalization regressions, run `pwsh -File .\tests\test_install_finalize.ps1`
after building `installer_workflow_tests`. The suite generates and removes its
own small packages. It covers legitimate `.install_temp` entries on both
extraction routes, locked empty staging directories, and refusal when files
remain in staging.

For an existing chunk-validated package, build `pipeline_smoke` and use
`scripts\run-existing-smoke.ps1`. It resolves the first archive part through
`ManifestLoader`, not `files[]` order. Legacy sequential manifests are rejected
before timing. Input package files remain read-only: the corruption test copies
the whole package into a fresh folder below `Reports` and changes only that copy.
Allow additional free space equal to the archive size for this retained fixture;
fixture-copy failure stops the test and never falls back to changing the input.
Keep outputs/reports outside the package, use a new output parent, and retain
separate output space for all extraction attempts.

Build `pipeline_smoke` and `smoke_script_driver`, then run
`tests\test_smoke_helpers.ps1` for small source-safety/archive-selection regression
tests, including copy failure and forced process termination.

## Change Theme And Text

Edit the source stylesheet:

```text
InstallerApp\ui\style.css
```

Edit `ui\strings.json` to change labels, buttons, dialogs, and primary messages. Preserve placeholders such as `{path}`. Rebuild after changing CSS or JSON:

```powershell
.\scripts\build-release.ps1
```

The renderer reads supported `:root` variables from its embedded stylesheet: the palette variables plus `--font-family`, `--font-size`, `--header-height`, `--footer-height`, `--control-height`, `--corner-radius`, `--content-padding`, and `--label-width`. The source CSS and JSON remain editable in the repository but are not distributed or read from external files at runtime.

## Build With CMake

```powershell
cmake -S InstallerApp -B InstallerApp/build
cmake --build InstallerApp/build
ctest --test-dir InstallerApp/build --output-on-failure
```

## Package Layout

The GUI package layout is:

```text
MyPack/
  modlist-installer.exe
  manifest.json
  MyPack.7z.001
  MyPack.7z.002
```

Run from that folder:

```powershell
modlist-installer.exe
```

The GUI requires `manifest.json`, uses archive files beside the executable, and uses the manifest's `unpacked_size` for space checks. If no flat manifest exists, it also supports older `data\package\manifest.json` and `data\downloads` packages. Chunked packer manifests use a bounded verified-block cache: validation runs ahead of extraction, and unverified bytes never reach 7-Zip. Legacy manifests use the original separate verification pass. If the manifest is missing, invalid, or any hash fails, installation stops and shows a validation failure message.

The interface uses Windows Direct2D and DirectWrite. It does not require WebView2, Edge, .NET, or an internet connection.

The SDK component is from [7-Zip](https://www.7-zip.org/). Its original license is in `resources\7zip-License.txt`, embedded in the executable, and written into the Local AppData tools cache.
