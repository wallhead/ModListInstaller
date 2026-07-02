# Modlist Packer

Native Windows helper for building modlist release folders.

It provides a focused 7-Zip-style archive settings window, runs embedded 7-Zip, tests the archive when requested, then writes a chunked SHA-256 manifest into `data\package\manifest.json`.

## Release Layout

```text
ReleaseFolder\
  modlist-installer.exe
  MyPack.7z.001
  MyPack.7z.002
  data\
    package\
      manifest.json
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

The default chunk size is 64 MiB.

7-Zip is embedded inside `modlist-packer.exe` and extracted to a per-user cache when archive creation starts. The packer does not ask for an external `7z.exe`.

`Manifest Only` scans the release folder for archive outputs only. It includes the exact archive file or numeric split volumes such as `MyPack.7z.001`, ignores side files like `.tmp` and logs, and writes `archive_name` from the real detected archive name. If more than one archive set is present, enter the wanted archive name first.

Manifest hashing auto-selects sequential HDD reads or parallel SSD reads, with a measured 4 MiB per-worker buffer.

## Build

```powershell
cd PackerApp
.\scripts\build-release.ps1
```

The ready exe is copied to:

```text
PackerApp\dist\modlist-packer.exe
```
