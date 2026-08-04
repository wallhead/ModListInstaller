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
```

The GUI executable is built as:

```text
InstallerApp\build\windows\x64\release\modlist-installer-gui.exe
```

Copy it into `dist`, put `manifest.json` in `dist\data\package`, keep archive parts in `dist\data\downloads`, and keep UI files in `dist\data\ui`. The GUI does not use torrent files.

The `Install` button runs the install pipeline on a background thread:

- load `data\package\manifest.json`
- validate unpack and install folders
- resolve the Windows Documents known folder and create missing `My Games\Skyrim Special Edition` and `My Games\Fallout4` folders
- check the manifest's unpacked payload size plus extraction overhead against free space
- for chunked packer manifests, read each archive block once, verify it asynchronously, and expose it to the 7-Zip SDK only after SHA256 succeeds
- show simultaneous validation and extraction percentages, validation speed, ETA, and elapsed time
- retain separate sequential HDD or parallel SSD validation followed by command-line extraction for legacy manifests or unavailable SDK components
- look for the archive named by the manifest in `data\downloads`
- check unpack free space again before extraction; same-volume installs do not reserve a second full copy
- show live unpack percentage, speed, and ETA in the progress bar and status line
- refresh unpack elapsed time every second, count ETA down between percentage changes, and map split archives across the full unpack progress range
- show live install percentage, speed, ETA, and elapsed time in the status line
- install from the unpack folder into the final install folder, using same-drive move/cut semantics instead of copying when both folders are on the same drive
- embed the 7-Zip command-line executable, SDK library, and license inside the installer exe and extract them to `data\tools\7zip` when needed
- stream full 7-Zip diagnostics to `dist\data\logs` while keeping only a small in-memory tail for the GUI
- run 7-Zip inside a memory-limited child process so oversized archives fail cleanly instead of exhausting system RAM
- extract with bundled 7-Zip into the selected install folder

The GUI is organized as a single WebView2-powered installer screen with unpack drive, final install folder, progress, and log output.

The unpack drive selector asks only for a drive letter. The app derives the unpack target as `<drive>:\Unpacked`, for example `X:\Unpacked`.

The unpack and install folders must be empty before installation begins. On different volumes, the installer also requires enough destination space for the full unpacked payload. Manifests without `unpacked_size` remain supported and use the archive size as an approximate compatibility fallback.

## Ready Binary

The ready exe is placed at:

```text
InstallerApp\dist\modlist-installer.exe
```

The runtime layout is:

```text
InstallerApp\dist\
  modlist-installer.exe
  data\
    package\
      manifest.json
    logs\
    downloads\
      YourPack.7z.001
      YourPack.7z.002
    tools\
      7zip\
      webview2\
        MicrosoftEdgeWebview2Setup.exe
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

The script restores the WebView2 SDK if needed, configures xmake for release, runs tests, and copies the GUI exe, local UI assets, and signed WebView2 Evergreen Bootstrapper into `dist`.

## Change CSS

Edit the source stylesheet:

```text
InstallerApp\ui\style.css
```

For a quick local test without rebuilding the exe, copy the UI folder into `dist\data\ui`:

```powershell
Remove-Item -Recurse -Force .\dist\data\ui -ErrorAction SilentlyContinue
Copy-Item -Recurse -Force .\ui .\dist\data\ui
```

Then restart `dist\modlist-installer.exe`.

For release builds, use:

```powershell
.\scripts\build-release.ps1
```

The release script copies `ui` into `dist\data\ui`. Treat edits in `dist\data\ui` as temporary because the next release build overwrites them.

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
  data/
    package/
      manifest.json
    downloads/
      MyPack.7z.001
      MyPack.7z.002
    tools/
      webview2/
        MicrosoftEdgeWebview2Setup.exe
    ui/
      index.html
      style.css
      app.js
      assets/
```

Run from that folder:

```powershell
modlist-installer.exe
```

The GUI requires `data\package\manifest.json`, loads UI from `data\ui`, uses archive files from `data\downloads`, and uses the manifest's `unpacked_size` for space checks. Chunked packer manifests use a bounded verified-block cache: validation runs ahead of extraction, and unverified bytes never reach 7-Zip. Legacy manifests use the original separate verification pass. If the manifest is missing, invalid, or any hash fails, installation stops and shows a validation failure message.

The themed UI requires Microsoft Edge WebView2 Runtime. If it is unavailable, the native launcher shows a warning and runs `data\tools\webview2\MicrosoftEdgeWebview2Setup.exe /silent /install`. The Evergreen Bootstrapper requires an internet connection; if installation fails, the standard native fallback remains available.

The SDK component is from [7-Zip](https://www.7-zip.org/) and is distributed with its original license in `resources\7zip-License.txt` and `data\tools\7zip\License.txt`.
