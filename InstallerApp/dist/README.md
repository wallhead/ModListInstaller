# Runtime Layout

Keep only the launcher at the root of this folder.

```text
dist/
  modlist-installer.exe
  package/
    YourPack.torrent
  tools/
    7zip/
      7z.exe
  downloads/
  logs/
```

The app creates missing `package`, `downloads`, `logs`, and `tools\7zip` folders on startup.
