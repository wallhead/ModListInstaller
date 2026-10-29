# ModListInstaller

Native Windows tools for building and installing local Skyrim modlist release packages.

## Ready Binaries

```text
InstallerApp/dist/modlist-installer.exe
PackerApp/dist/modlist-packer.exe
```

`modlist-installer.exe` is self-contained. Ship it beside `manifest.json` and the archive parts, inside a folder named after the archive.

`modlist-packer.exe` is portable from `PackerApp/dist`: keep only `modlist-installer.exe` beside it. The packer creates `<selected release parent>/<archive name>` and copies the installer there.

## Release Package

Ready-to-use versioned release archives are written to:

```text
Release/ModlistInstaller-v0.3.4.zip
```

Extract the zip and run `modlist-packer.exe`. Published older ZIPs retain their original layout; the current source build uses the flat layout below.

## Installer Runtime Layout

```text
MyPack/
  modlist-installer.exe
  manifest.json
  MyPack.7z.001
  MyPack.7z.002
```

The installer reads `manifest.json` and archive parts beside its executable. UI text, theme, 7-Zip components, and the license are embedded. On startup it creates `logs\modlist-installer.log` beside the executable. Extraction diagnostics are also saved in `logs`; Open Log opens the main log. Extracted tools remain in `%LOCALAPPDATA%\ModlistInstaller\<package key>`. Older `data\package\manifest.json` and `data\downloads` packages remain supported when no flat manifest exists.

## Current Installer Features

- Native C++20 Win32 executable.
- Native Direct2D/DirectWrite interface with crisp hardware-accelerated rendering.
- No WebView2, Edge, Electron, .NET, dev server, or remote UI assets.
- Embedded CSS theme variables control colors, typography, spacing, and core dimensions.
- Embedded JSON strings control the visible labels, buttons, dialogs, and primary messages.
- Single-screen installer UI with install root, final archive-named path, progress, status, and log output.
- One-read asynchronous SHA256 validation/extraction for chunked packer manifests: a background reader verifies blocks before an embedded 7-Zip SDK decoder can consume them.
- Legacy manifests retain the separate full SHA256 pass and command-line 7-Zip extraction path.
- Archive discovery beside the executable, using the archive filename from `manifest.json`.
- Before extraction, archive entry names are checked against the final install path. If every complete path fits within 240 UTF-16 code units, extraction goes directly into `<install root>\<archive_name>` and skips the final move.
- Longer paths or unavailable archive metadata use the short `<drive>:\Unpacking` staging folder on the install volume.
- Unpacked payload size recorded in new manifests for accurate free-space checks.
- Same-volume installs reserve extraction space once; cross-volume installs also check the destination for the full payload.
- Existing files in the unpack or final install folder require explicit confirmation before their contents are permanently removed and installation restarts.
- Before installation, the installer uses the Windows Documents known folder and ensures `My Games\Skyrim Special Edition` and `My Games\Fallout4` exist.
- Live validation, extraction, and install progress with status text.
- Same-drive installs use move/cut semantics automatically when possible.
- Successful staged installs remove the empty `<drive>:\Unpacking` folder; direct installs leave that unused folder untouched.
- Embedded 7-Zip command-line and SDK components are cached in Local AppData when needed.
- A `logs` folder is created beside the executable on startup for the main log and extraction diagnostics.

## Quick Use

1. Keep the packer and installer executables together.
2. In the packer, select the source folder, release parent, and archive name.
3. Build the package and distribute the generated `<archive name>` folder.
4. Run its `modlist-installer.exe` and select an install root.
5. Press `Install`. The final folder is `<install root>\<archive_name>`; the installer chooses direct extraction or `<drive>:\Unpacking` after checking the archive paths.

If the chosen extraction folder or the final `<install root>\<archive_name>` folder contains files, the installer offers to permanently clear the affected folders before installing. Direct installs do not check or clear the unused `<drive>:\Unpacking` folder. The install root itself may contain other folders. The packer removes older outputs for the selected archive name before building, and rejects source and release folders that overlap.

## Build

Requirements:

- Windows
- Visual Studio C++ build tools
- xmake

Build and refresh `InstallerApp/dist/modlist-installer.exe`:

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The release script builds with xmake, embeds the theme and text catalogue, runs tests, and copies the self-contained executable into `dist`. No browser runtime is downloaded or bundled.

## Change Installer Theme And Text

Edit the source stylesheet:

```text
InstallerApp/ui/style.css
```

To change visible text, edit `InstallerApp\ui\strings.json`. Keep placeholder names such as `{path}` unchanged. Both source files are embedded when building the installer; external CSS and JSON files are no longer read or distributed.

For a release build, run:

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The native renderer reads supported `:root` variables from its embedded stylesheet. It supports the palette variables plus `--font-family`, `--font-size`, `--header-height`, `--footer-height`, `--control-height`, `--corner-radius`, `--content-padding`, and `--label-width`. Arbitrary browser selectors and animations are intentionally ignored. Rebuild after editing either source asset.

## 7-Zip

This project uses 7-Zip components under the GNU LGPL and BSD 3-clause licenses. The embedded license is written to the Local AppData tools cache at runtime. Source and project information are available from [7-zip.org](https://www.7-zip.org/).

## Repository Layout

```text
InstallerApp/
  dist/       ready self-contained installer exe
  resources/ Windows icon and embedded 7-Zip resource
  scripts/   build and dependency restore helpers
  src/       C++ installer source
  tests/     core tests
  ui/        editable CSS variables for the native Direct2D theme
  xmake.lua  primary build
PackerApp/
  dist/      ready modlist packer exe
  scripts/   build helper
  src/       native packer GUI and manifest writer
  xmake.lua  packer build
```
