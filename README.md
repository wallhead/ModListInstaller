# ModListInstaller

Native Windows modlist installer with torrent support.

The ready-to-run build is included at:

```text
InstallerApp/dist/modlist-installer.exe
```

Put exactly one `.torrent` file in `InstallerApp/dist/package`, then run the exe. The app auto-detects that torrent, lets the user choose download and install folders, validates the torrent payload, and unpacks the downloaded archive with bundled 7-Zip.

## Current Features

- Native Windows GUI.
- Three-step wizard with Previous/Next navigation.
- Classic installer-inspired color scheme with blue side rail and grey navigation footer.
- Welcome page with placeholder greeting text.
- Folder selection page.
- Download and validation page.
- Auto-detects one `.torrent` from the `package` folder.
- Torrent download through libtorrent-rasterbar.
- DHT, PEX, and local service discovery enabled.
- Download progress, speed, seeds, peers, and ETA.
- Pause, resume, and stop controls.
- Reuses existing downloaded files when the same folder is selected again.
- Force-rechecks torrent data after download before unpacking.
- Releases torrent file handles before extraction.
- Unpack-only button for already downloaded `.7z.001` archives.
- Live unpack progress percentage.
- Supports split `.7z.001` packages that contain an inner `.7z` archive.
- Keeps full 7-Zip diagnostics in `dist/logs`, while the GUI log stays concise.

## Quick Use

```text
MyPack/
  modlist-installer.exe
  package/
    MyPack.torrent
  tools/
    7zip/
      7z.exe
```

Run `modlist-installer.exe`, choose a download folder and a short install folder such as `D:\Sky`, then press `Start`.

Use `Unpack` when the archive is already downloaded and you want to skip torrent validation/download.

## Build Setup

Requirements:

- Windows
- Visual Studio C++ build tools
- xmake
- vcpkg at `%USERPROFILE%\vcpkg` or `VCPKG_ROOT`
- `libtorrent:x64-windows-static` installed in vcpkg

Install libtorrent:

```powershell
cd $env:USERPROFILE\vcpkg
.\vcpkg install libtorrent:x64-windows-static
```

Build and refresh `InstallerApp/dist/modlist-installer.exe`:

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

Manual build:

```powershell
cd InstallerApp
xmake f --use_libtorrent=y -m release
xmake
xmake run installer_core_tests
Copy-Item .\build\windows\x64\release\modlist-installer-gui.exe .\dist\modlist-installer.exe -Force
```

## Repository Layout

```text
InstallerApp/
  dist/       ready exe and bundled 7-Zip
  resources/  Windows icon resources
  docs/       architecture notes
  examples/   example manifest
  scripts/    build helper
  src/        C++ source
  tests/      core tests
  xmake.lua   primary build
```

The app can still build without libtorrent, but the real torrent backend requires `xmake f --use_libtorrent=y`.
