# Runtime Layout

The distributable folder contains only the installer, manifest, and archives.

```text
YourPack/
  modlist-installer.exe
  manifest.json
  YourPack.7z.001
  YourPack.7z.002
```

The theme and text are embedded in the executable. On startup, `logs` is created
beside the executable with `modlist-installer.log`. Open Log opens that log.
Tools are cached under `%LOCALAPPDATA%\ModlistInstaller\<package key>`.
Older `data` layouts remain supported.
