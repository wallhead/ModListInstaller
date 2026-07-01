# Windows Installer With Torrent Support - Architecture

This project follows the prompt in `Windows Installer with Torrent Support.pdf`.

## Goals

- Download modlist archives through libtorrent-rasterbar.
- Load additional public trackers from `https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_all.txt`.
- Enable DHT, peer exchange, and local service discovery where supported.
- Ask the user for a download folder and a short final install folder such as `D:\Sky`.
- Verify every downloaded file against manifest SHA256 hashes before extraction.
- Extract multi-volume 7-Zip archives directly into the selected install folder.
- Keep extraction temp work on the same drive as the install folder.
- Show progress, cancellation, retry, and clear errors.

## Project Layout

```text
InstallerApp/
  src/
    app/          CLI harness now; future Windows UI host
    downloader/   Torrent downloader interface and libtorrent backend
    extractor/    7-Zip process wrapper and same-disk temp rules
    logging/      File logging
    manifest/     JSON parser and manifest validation
    paths/        Folder validation and Skyrim path safety rules
    tracker/      Tracker list download/parsing interface
    verifier/     SHA256 and manifest file verification
  tests/          Dependency-free C++ tests
  third_party/
    7zip/         Place bundled 7z.exe, 7za.exe, or 7zz.exe here
    libtorrent/   Place vendored/runtime notes or binaries here if needed
  docs/
```

## Runtime Flow

1. Load and validate the JSON manifest.
2. Ask for download and install folders.
3. Validate write access, free space, drive root, path length, and user-profile nesting.
4. Load public trackers. If this fails and the manifest allows it, continue.
5. Start libtorrent with the selected download folder, DHT, PEX, LSD, and tracker list.
6. Report progress, speeds, peer count, DHT status, tracker count, and errors.
7. Recheck/complete the torrent, then verify file size and SHA256 from the manifest.
8. Extract the first archive volume, such as `modpack.7z.001`, with 7-Zip:

```bat
7z.exe x "D:\Downloads\modpack.7z.001" -o"D:\Sky" -y -w"D:\Sky\.install_temp"
```

9. Show success or a precise error.

## Module Boundaries

- UI owns user interaction and progress presentation only.
- Torrent logic stays behind `ITorrentDownloader`.
- Tracker loading stays behind `ITrackerProvider`.
- Path validation stays behind `IPathValidator`.
- Verification never trusts torrent completion alone.
- Extraction never runs before manifest verification passes.

## Libtorrent

The repo builds without libtorrent by default so the manifest, paths, trackers, verifier, and extractor can be tested immediately. To enable the real backend:

```powershell
cmake -S InstallerApp -B InstallerApp/build -DMODLIST_USE_LIBTORRENT=ON
```

The CMake package name expected is `LibtorrentRasterbar::torrent-rasterbar`, which is the target exposed by common vcpkg installs.

## Windows UI Plan

The initial code includes a CLI harness. The next UI layer should be a native Windows app or installer front end that calls the same core modules. Required controls:

- Download folder picker.
- Install folder picker with a visible short-path warning.
- Stage label: loading manifest, loading trackers, downloading, checking files, extracting, completed.
- Progress bar.
- Download speed, upload speed, peers, ETA, DHT status, and tracker count.
- Cancel, retry, and open log actions.
