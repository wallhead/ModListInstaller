# ModListInstaller

Native Windows modlist installer with torrent support.

The ready-to-run build is included at:

```text
InstallerApp/dist/modlist-installer.exe
```

Put exactly one `.torrent` file in `package\` and the complete multi-volume archive beside `modlist-installer.exe`, then run the exe. The app validates the local archive files against the torrent, then unpacks them with bundled 7-Zip.

## Current Features

- Native Windows GUI.
- Three-step wizard with Previous/Next navigation.
- Dark Nordic-inspired color scheme with charcoal panels, cool grey text, and icy blue accents.
- Owner-drawn installer-style buttons with hover, pressed, focus, and disabled states.
- Welcome page with placeholder greeting text.
- Folder selection page.
- Separate choices for unpack drive and final install folder.
- Unpack drive selection automatically resolves to `X:\Sky`.
- Validation and install page.
- Auto-detects one `.torrent` from the `package` folder.
- Validates local archive files through libtorrent-rasterbar without starting a network download.
- Shows local validation progress before unpacking.
- Stop control for active validation.
- Force-checks torrent data before unpacking.
- Releases torrent file handles before extraction.
- Live unpack progress percentage.
- Supports split `.7z.001` packages that contain an inner `.7z` archive.
- Keeps full 7-Zip diagnostics in `dist/logs`, while the GUI log stays concise.
- Streams 7-Zip diagnostics to disk and keeps only a small in-memory tail for error display.
- Runs 7-Zip in a memory-limited child process so oversized archives fail cleanly instead of exhausting system RAM.

## Quick Use

```text
MyPack/
  modlist-installer.exe
  MyPack.7z.001
  MyPack.7z.002
  package/
    MyPack.torrent
  tools/
    7zip/
      7z.exe
```

Run `modlist-installer.exe`, choose the drive to unpack to, choose the final install folder, then press `Install`. The unpack target is always generated as `<drive>:\Sky`.

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
