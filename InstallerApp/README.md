# Modlist Installer

Native Windows installer for local modlist release folders produced by `PackerApp`.

## What Is Included

- CMake C++ project structure.
- Manifest JSON parser and validator.
- Skyrim-safe path validation.
- SHA256 verifier for manifest files.
- 7-Zip wrapper that extracts directly to the install folder and uses same-disk temp.
- Dependency-free tests.
- Example manifest.

## Build With xmake

```powershell
cd InstallerApp
xmake
xmake run installer_core_tests
```

The GUI executable is built as:

```text
InstallerApp\build\windows\x64\release\modlist-installer-gui.exe
```

Copy it into `dist`, put `manifest.json` in `dist\data\package`, and keep all archive parts beside the exe. The GUI does not use torrent files.

The `Install` button runs the install pipeline on a background thread:

- load `data\package\manifest.json`
- validate unpack and install folders
- check manifest archive size against free space
- verify archive file sizes and SHA256 hashes from the manifest
- show validation progress, speed, ETA, and elapsed time
- auto-select sequential HDD validation or parallel SSD validation
- look for the archive named by the manifest beside the exe
- check unpack free space again before extraction
- show live unpack percentage, speed, and ETA in the progress bar and status line
- show live install percentage, speed, ETA, and elapsed time in the status line
- install from the unpack folder into the final install folder, using same-drive move/cut semantics instead of copying when both folders are on the same drive
- embed 7-Zip inside the installer exe and extract it to `data\tools\7zip` when needed
- stream full 7-Zip diagnostics to `dist\data\logs` while keeping only a small in-memory tail for the GUI
- run 7-Zip inside a memory-limited child process so oversized archives fail cleanly instead of exhausting system RAM
- extract with bundled 7-Zip into the selected install folder

The GUI is organized as a single WebView2-powered installer screen with unpack drive, final install folder, progress, and log output.

The unpack drive selector asks only for a drive letter. The app derives the unpack target as `<drive>:\Sky`, for example `X:\Sky`.

## Ready Binary

The ready exe is placed at:

```text
InstallerApp\dist\modlist-installer.exe
```

The runtime layout is:

```text
InstallerApp\dist\
  modlist-installer.exe
  YourPack.7z.001
  YourPack.7z.002
  data\
    package\
      manifest.json
    logs\
    downloads\
    tools\
      7zip\
  ui\
    index.html
    style.css
    app.js
    assets\
```

Do not commit generated logs.

## Release Build Script

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The script restores the WebView2 SDK if needed, configures xmake for release, runs tests, copies local UI assets, and copies the GUI exe into `dist\modlist-installer.exe`.

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
  MyPack.7z.001
  MyPack.7z.002
  data/
    package/
      manifest.json
```

Run from that folder:

```powershell
modlist-installer.exe
```

The GUI requires `data\package\manifest.json`, uses its archive filename, uses its file sizes for space checks, and verifies SHA256 before extraction. If the manifest is missing, invalid, or the hashes fail, install stops and shows a validation failure message.
