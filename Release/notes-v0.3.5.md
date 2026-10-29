# ModListInstaller v0.3.5

Self-contained flat packages, Unicode fixes, and installer reliability improvements.

## Ready-To-Use Setup

Download `ModlistInstaller-v0.3.5.zip`, extract it, and run `modlist-packer.exe`.
Keep the included matching `modlist-installer.exe` beside the packer. The ZIP also
includes the README and 7-Zip license. No WebView2, Edge, .NET, external CSS/JSON,
or browser runtime is needed.

The packer creates `<selected release parent>\<archive name>` containing only:

```text
modlist-installer.exe
manifest.json
<archive name>.7z.001
<archive name>.7z.002
...
```

The installer creates `logs\modlist-installer.log` beside its executable on startup.
UI text and theme are embedded; edit `InstallerApp/ui/strings.json` or `style.css`
in the source and rebuild to customize them. Older `data` packages remain supported.

## Fixes

- Manifest archive paths consistently use UTF-8, including Cyrillic and other Unicode names.
- Full paths and `-spf` custom switches are rejected before packing output is written.
- Cached 7-Zip executable/DLL bytes are checked against embedded copies; same-size corruption is repaired.
- Progress messages are rate-limited, failed posts release their owned strings, and log limits are raised.
- Unicode case comparisons and filename handling no longer depend on the Windows ANSI code page.
- A long destination's access probe no longer reports false permission errors when its own filenames exceed the legacy path limit.
- Package/install overlap messages explain how to choose another destination.
- Startup no longer performs a throwing, redundant cache-folder creation inside the window callback.
- Unrelated torrent files no longer prevent the GUI's local package discovery.
- The command-line extraction fallback no longer treats 7-Zip warning exit codes as installation success.
- Packer test fixtures are cleaned by default; explicit retention remains available for diagnostics.

## Verification

Release builds and both test suites pass, including 20 packer tests with real compression,
extraction, independent manifest hashes, Unicode roundtrips, and cache-tampering regressions.

The release code was also smoke-tested through the native GUIs using exactly 30 GiB
of real data in 200,000 files and 16,250 directories, including empty files/folders and
Cyrillic, Chinese, and emoji filenames. Direct extraction and a 307-character staged
destination both passed full output SHA-256 comparison. All four archive volumes and
241 SHA-256 chunks were independently checked. A deliberate byte change was rejected,
then restored and verified against the original full-volume hash.
