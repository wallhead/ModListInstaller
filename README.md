# ModListInstaller

Native Windows tools for building and installing local Skyrim modlist release packages.

## Ready Binaries

```text
InstallerApp/dist/modlist-installer.exe
PackerApp/dist/modlist-packer.exe
```

`modlist-installer.exe` is the distributable launcher. Ship it at the release root and keep installer support files, UI, manifest, and archive parts under `data`.

`modlist-packer.exe` is portable from `PackerApp/dist`: keep `modlist-installer.exe` beside it and keep `data/ui` plus `data/tools` beside both. The packer copies that local installer bundle into each release folder.

## Release Package

Ready-to-use versioned release archives are written to:

```text
Release/ModlistInstaller-v0.2.8.zip
```

Extract the zip and run `modlist-packer.exe`. The archive contains the portable packer, the installer exe beside it, and `data/ui` ready for release-folder creation.

## Installer Runtime Layout

```text
MyPack/
  modlist-installer.exe
  data/
    package/
      manifest.json
    downloads/
      MyPack.7z.001
      MyPack.7z.002
    logs/
    tools/
      7zip/
      webview2/
        MicrosoftEdgeWebview2Setup.exe
    ui/
      index.html
      style.css
      app.js
      assets/
```

The installer looks for `data\package\manifest.json` beside the exe and archive parts in `data\downloads`. There is no package-folder picker in the UI.

## Current Installer Features

- Native C++20 Win32 executable.
- WebView2-powered local HTML/CSS/JS interface.
- No Electron, no dev server, no remote UI assets.
- Single-screen installer UI with unpack drive, install root, final archive-named path, progress, status, and log output.
- One-read asynchronous SHA256 validation/extraction for chunked packer manifests: a background reader verifies blocks before an embedded 7-Zip SDK decoder can consume them.
- Legacy manifests retain the separate full SHA256 pass and command-line 7-Zip extraction path.
- Archive discovery in `data\downloads`, using the archive filename from `data\package\manifest.json`.
- Unpack drive selection automatically resolves to `<drive>:\Unpacked`.
- Unpacked payload size recorded in new manifests for accurate free-space checks.
- Same-volume installs reserve extraction space once; cross-volume installs also check the destination for the full payload.
- Existing files in the unpack or final install folder require explicit confirmation before their contents are permanently removed and installation restarts.
- Before installation, the installer uses the Windows Documents known folder and ensures `My Games\Skyrim Special Edition` and `My Games\Fallout4` exist.
- Live validation, extraction, and install progress with status text.
- Same-drive installs use move/cut semantics automatically when possible.
- Successful installs remove the empty `<drive>:\Unpacked` staging folder.
- Embedded 7-Zip command-line and SDK extraction components under `data\tools\7zip`.
- Bundled Microsoft WebView2 Evergreen Bootstrapper under `data\tools\webview2`; if the runtime is missing, the installer explains the requirement, installs it silently, verifies it, and then loads the themed UI.
- Full diagnostics under `data\logs`.

## Quick Use

1. Put `modlist-installer.exe` at the release root.
2. Put archive parts in `data\downloads`.
3. Put the manifest at `data\package\manifest.json`.
4. Put WebView UI files in `data\ui` and the bundled WebView2 bootstrapper in `data\tools\webview2`.
5. Run `modlist-installer.exe`.
6. Select an unpack drive and install root. The installer creates `<install root>\<archive_name>`.
7. Press `Install`.

If `<drive>:\Unpacked` or the final `<install root>\<archive_name>` folder contains files, the installer offers to permanently clear the affected folders before installing. The install root itself may contain other folders. The packer removes older outputs for the selected archive name before building, and rejects source and release folders that overlap.

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
Remove-Item -Recurse -Force InstallerApp\dist\data\ui -ErrorAction SilentlyContinue
Copy-Item -Recurse -Force InstallerApp\ui InstallerApp\dist\data\ui
```

Then restart `InstallerApp\dist\modlist-installer.exe`.

For a release build, run:

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The release script copies `InstallerApp\ui` into `InstallerApp\dist\data\ui`, so changes made only inside `dist\data\ui` are temporary and can be overwritten by the next build.

## 7-Zip

This project uses 7-Zip components under the GNU LGPL and BSD 3-clause licenses. The bundled license is written to `data\tools\7zip\License.txt` at runtime. Source and project information are available from [7-zip.org](https://www.7-zip.org/).

## Repository Layout

```text
InstallerApp/
  dist/       ready installer exe and data folder
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
