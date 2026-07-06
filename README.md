# ModListInstaller

Native Windows tools for building and installing local Skyrim modlist release packages.

## Ready Binaries

```text
InstallerApp/dist/modlist-installer.exe
PackerApp/dist/modlist-packer.exe
```

`modlist-installer.exe` is the distributable launcher. Ship it beside the archive parts and keep installer support files under `data`.

## Installer Runtime Layout

```text
MyPack/
  modlist-installer.exe
  MyPack.7z.001
  MyPack.7z.002
  data/
    package/
      manifest.json
    downloads/
    logs/
    tools/
      7zip/
  ui/
    index.html
    style.css
    app.js
    assets/
```

The installer looks for `data\package\manifest.json` beside the exe. Archive parts stay beside the exe root. There is no package-folder picker in the UI.

## Current Installer Features

- Native C++20 Win32 executable.
- WebView2-powered local HTML/CSS/JS interface.
- No Electron, no dev server, no remote UI assets.
- Single-screen installer UI with unpack drive, final install folder, progress, status, and log output.
- Manifest SHA256 verification before extraction.
- Archive discovery beside the exe, using the archive filename from `data\package\manifest.json`.
- Unpack drive selection automatically resolves to `<drive>:\Sky`.
- Live validation, extraction, and install progress with status text.
- Same-drive installs use move/cut semantics automatically when possible.
- Embedded 7-Zip extraction to `data\tools\7zip`.
- Full diagnostics under `data\logs`.

## Quick Use

1. Put `modlist-installer.exe` beside the archive parts.
2. Put the manifest at `data\package\manifest.json`.
3. Run `modlist-installer.exe`.
4. Select an unpack drive and final install folder.
5. Press `Install`.

## Build

Requirements:

- Windows
- Visual Studio C++ build tools
- xmake
- WebView2 Runtime installed on the target machine

Build and refresh `InstallerApp/dist/modlist-installer.exe`:

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The release script restores the WebView2 SDK package locally under ignored `third_party/webview2`, builds with xmake, runs tests, copies the local UI files, and refreshes the ready exe.

## Change Installer CSS

Edit the source stylesheet:

```text
InstallerApp/ui/style.css
```

For a quick local test, copy the edited UI files into the ready installer folder:

```powershell
Copy-Item -Recurse -Force InstallerApp\ui InstallerApp\dist\
```

Then restart `InstallerApp\dist\modlist-installer.exe`.

For a release build, run:

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The release script copies `InstallerApp\ui` into `InstallerApp\dist\ui`, so changes made only inside `dist\ui` are temporary and can be overwritten by the next build.

## Repository Layout

```text
InstallerApp/
  dist/       ready installer exe, ui files, and data folder
  resources/ Windows icon and embedded 7-Zip resource
  scripts/   build and dependency restore helpers
  src/       C++ installer source
  tests/     core tests
  ui/        local WebView2 HTML/CSS/JS UI
  xmake.lua  primary build
PackerApp/
  dist/      ready modlist packer exe
  scripts/   build helper
  src/       native packer GUI and manifest writer
  xmake.lua  packer build
```
