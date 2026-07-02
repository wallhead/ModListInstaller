# Runtime Layout

Keep only the launcher at the root of this folder.

```text
dist/
  modlist-installer.exe
  YourPack.7z.001
  YourPack.7z.002
  data/
    package/
      manifest.json
    downloads/
    logs/
    tools/
      7zip/
```

The app creates missing `data\package`, `data\downloads`, `data\logs`, and `data\tools\7zip` folders on startup.
