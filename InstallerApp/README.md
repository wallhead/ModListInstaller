# Modlist Installer

Initial implementation from `Windows Installer with Torrent Support.pdf`.

## What Is Included

- CMake C++ project structure.
- Manifest JSON parser and validator.
- Skyrim-safe path validation.
- Tracker list parser with protocol filtering and deduplication.
- SHA256 verifier for manifest files.
- 7-Zip wrapper that extracts directly to the install folder and uses same-disk temp.
- Libtorrent downloader interface with an optional libtorrent-rasterbar backend.
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

Copy it into `dist`, then put exactly one `.torrent` file in `dist\package`. The GUI does not ask for a torrent path; it automatically scans the package folder and requires exactly one `.torrent` file there.

The `Start` button runs the install pipeline on a background thread:

- detect the torrent in the package folder
- validate download, unpack, and install folders
- check known archive size against free space when local archive parts are present
- read total payload size directly from the `.torrent` file before downloading
- check remaining torrent size against download free space once torrent metadata is available
- start the torrent downloader
- write torrent payloads to disk through bounded libtorrent queues instead of keeping full payloads in RAM
- show progress, download speed, upload speed, seeds, peers, and ETA
- pause/resume or stop the active download
- force-check existing files when starting, so closing the exe mid-download and choosing the same download folder resumes after validation
- force-recheck the completed torrent payload before extraction
- release torrent file handles before 7-Zip starts
- unpack directly from an existing `.7z.001` with the `Unpack` button, skipping torrent validation/download
- look for a `.7z.001` archive after download
- check unpack free space again before extraction
- show live unpack percentage in the progress bar and status line
- stream full 7-Zip diagnostics to `dist\logs` while keeping only a small in-memory tail for the GUI
- automatically run a second extraction pass when a split `.7z.001` contains one inner `.7z`
- extract with bundled 7-Zip into the selected install folder

The GUI is organized as a three-step wizard:

- welcome page
- folder selection page with download folder, unpack drive, and final install folder
- download and validation page

The window uses a dark Nordic-inspired color scheme with charcoal panels, cool grey text, icy blue accents, and owner-drawn buttons with hover/pressed/focus states. The welcome text is intentionally placeholder copy for now.

The unpack drive selector asks only for a drive letter. The app derives the unpack target as `<drive>:\Sky`, for example `X:\Sky`.

The default build reports a clear error until libtorrent-rasterbar is linked with `xmake f --use_libtorrent=y`.

## Ready Binary

The ready exe is placed at:

```text
InstallerApp\dist\modlist-installer.exe
```

The runtime layout is:

```text
InstallerApp\dist\
  modlist-installer.exe
  package\
    YourPack.torrent
  tools\
    7zip\
      7z.exe
  downloads\
  logs\
```

Do not commit personal `.torrent` files or generated logs.

## Release Build Script

```powershell
cd InstallerApp
.\scripts\build-release.ps1
```

The script configures xmake for release, enables libtorrent, runs tests, and copies the GUI exe into `dist\modlist-installer.exe`.

## Build With CMake

```powershell
cmake -S InstallerApp -B InstallerApp/build
cmake --build InstallerApp/build
ctest --test-dir InstallerApp/build --output-on-failure
```

## Enable Libtorrent

Install libtorrent-rasterbar so CMake can find `LibtorrentRasterbar::torrent-rasterbar`, then configure with:

```powershell
cmake -S InstallerApp -B InstallerApp/build -DMODLIST_USE_LIBTORRENT=ON
```

For xmake, configure with:

```powershell
cd InstallerApp
xmake f --use_libtorrent=y
xmake
```

## Bundle 7-Zip

Place one of these in `InstallerApp/third_party/7zip/`:

- `7z.exe`
- `7za.exe`
- `7zz.exe`

## Try Manifest Validation

```powershell
InstallerApp/build/src/Debug/modlist-installer.exe InstallerApp/examples/example-manifest.json D:\Downloads D:\Sky
```

Use a short install path near the drive root. Avoid paths under `C:\Users\...` for Skyrim modlists.

## Simple Torrent Package Mode

You can skip the manifest for a basic GUI package layout:

```text
MyPack/
  modlist-installer.exe
  package/
    MyPack.torrent
  tools/
    7zip/
      7z.exe
```

Run from that folder:

```powershell
modlist-installer.exe
```

The GUI scans `package` for exactly one `.torrent` file. If a `.7z.001` file is already next to the torrent, it detects that too.

The console harness can still pass a torrent explicitly for testing:

```powershell
modlist-installer.exe MyPack.torrent D:\Downloads D:\Sky
```

The tradeoff: without a manifest, the installer does not know expected SHA256 hashes, exact archive names, cleanup rules, or version/update metadata. Torrent piece checks still protect torrent content during download, but the stronger post-download SHA256 verification needs a manifest or a separate hash file.
