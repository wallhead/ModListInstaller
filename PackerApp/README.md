# Modlist Packer

Native Windows helper for building modlist release folders.

It provides a focused 7-Zip-style archive settings window, runs embedded 7-Zip, tests the archive when requested, then writes a chunked SHA-256 manifest into `data\package\manifest.json`.

`modlist-packer.exe` is portable. Keep `modlist-installer.exe` beside it, and keep installer UI files under `data\ui` beside the packer. The packer copies those files into each release folder.

## Release Layout

```text
ReleaseFolder\
  modlist-installer.exe
  data\
    package\
      manifest.json
    downloads\
      MyPack.7z.001
      MyPack.7z.002
    ui\
      index.html
      style.css
      app.js
      assets\
    logs\
    tools\
      7zip\
```

## Manifest

The manifest stores:

- archive file path
- archive file size
- full-file SHA-256
- per-chunk SHA-256
- chunk size
- total unpacked payload size

The default chunk size is 64 MiB.

7-Zip is embedded inside `modlist-packer.exe` and extracted to a per-user cache when archive creation starts. The packer does not ask for an external `7z.exe`.

The default compression profile matches the recommended release settings:

- level `5`
- method `LZMA2`
- dictionary `32m`
- word size `32`
- solid block `8g`
- threads `20`
- split volumes `4092m`

The RAM limit defaults to 80% of physical memory and is enforced on the 7-Zip process with a Windows Job Object. The packing status reports 7-Zip's own percentage together with live process read throughput, ETA, elapsed time, and RAM use.

`Build Package` writes archive parts into `data\downloads`, writes the manifest into `data\package`, and refreshes the installer exe plus `data\ui`.

Before a build, the packer removes older archive parts, interrupted `.tmp` files for the selected archive name, and the previous manifest. Source and release folders cannot be the same folder or contain one another, preventing the release output from being archived recursively.

The status line reports source scan file count, accumulated size, and elapsed time. During 7-Zip packing and testing, live process metrics continue updating every second even when the integer archive percentage has not changed.

`Manifest Only` scans `data\downloads` for archive outputs only. It includes the exact archive file or numeric split volumes such as `MyPack.7z.001`, ignores side files like `.tmp` and logs, and writes `archive_name` from the real detected archive name. If more than one archive set is present, enter the wanted archive name first.

Manifest hashing auto-selects sequential HDD reads or parallel SSD reads, with a measured 4 MiB per-worker buffer.

## Build

```powershell
cd PackerApp
.\scripts\build-release.ps1
```

The ready exe is copied to:

```text
PackerApp\dist\
  modlist-packer.exe
  modlist-installer.exe
  data\
    ui\
```
