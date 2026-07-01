# Modlist Packer

Native Windows helper for building modlist release folders.

It provides a 7-Zip-style archive settings window, runs `7z.exe`, then writes a chunked SHA-256 manifest into `package\manifest.json`.

## Release Layout

```text
ReleaseFolder\
  modlist-installer.exe
  MyPack.7z.001
  MyPack.7z.002
  package\
    manifest.json
```

## Manifest

The manifest stores:

- archive file path
- archive file size
- full-file SHA-256
- per-chunk SHA-256
- chunk size

The default chunk size is 64 MiB.

## Build

```powershell
cd PackerApp
.\scripts\build-release.ps1
```

The ready exe is copied to:

```text
PackerApp\dist\modlist-packer.exe
```
