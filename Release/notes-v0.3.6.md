# ModListInstaller v0.3.6

Fixes for blank completion dialogs and stale installation progress.

## Fixes

- Successful installation now sets progress and status to 100% before opening the completion popup.
- Delayed progress and status messages can no longer overwrite the finished state.
- The main window repaints synchronously before modal messages open.
- Themed popup backgrounds and text use GDI rather than allocating a GPU render target for every paint, removing a silent blank-dialog failure path.
- Partial Direct2D frames retain their contents. New, resized, or recreated render targets receive a full redraw, and drawing failures retry with a software target.
- Failed or cancelled operations still retain their failure/stopped state and do not display successful completion.

## Ready-To-Use Setup

Download `ModlistInstaller-v0.3.6.zip` and extract it. Keep the included matching
`modlist-installer.exe` beside `modlist-packer.exe`.

The ZIP contains the two portable executables, README, and 7-Zip license.
Generated modlist packages still contain only the installer, manifest, and archives.
Existing manifests and archive volumes remain compatible; no repacking is required
to use the new installer. No external CSS/JSON or browser runtime is needed.

## Verification

Clean release builds and installer core tests pass. Four native UI regressions cover
the state before a completion dialog opens, late progress messages, failed workers,
and actual popup background/text pixels without initializing a graphics device.
The release script now runs these UI regressions alongside the core tests.

The fix was also checked through a native packing/install smoke test: output hashes
and empty entries matched, the completed window showed 100%, the themed popup was
readable with its centered OK button, and Close exited the installer.
