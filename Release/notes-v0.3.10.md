# ModListInstaller v0.3.10

## Ready-To-Use Setup

Extract `ModlistInstaller-v0.3.10.zip` and run `modlist-packer.exe` with
`modlist-installer.exe` beside it. The ZIP includes both portable Windows x64
executables, the README, and the 7-Zip license. No separate VC++ Redistributable,
installed 7-Zip, or browser runtime is needed.

To update an existing package, replace the installer beside its `manifest.json`
and archive volumes. No repacking is needed.

## Changes

- Installation logs identify the source revision used to build the installer
  and the SHA-256 of the loaded manifest, so reports can be matched to the
  exact build and package.
- The log distinguishes total bytes listed in the manifest from the bytes and
  volume count selected for verified extraction. The selected totals are marked
  unavailable on the legacy route.
- Verified extraction logs separate SDK open, extraction, and final verification
  wait timings. Minute and final samples also include existing input, output,
  cache, and file operation counters. The counters are cumulative and some
  timings overlap.

This is a diagnostics update. The interface, package format, and verification
policy are unchanged. For a slow installation, share `logs\modlist-installer.log`.

## Verification

The installer embeds clean source revision `868608cf68c0c3a2e954f02cdb6da412e2a91501`.
Both release builds passed the runtime-import checks. All 60 packer tests,
18 installer finalization cases, and 5 CMake tests passed. Pack-to-install smoke
tests verified payload contents on direct and staged routes for split 7z, ZIP,
and Cyrillic paths. A damaged volume was rejected, and the installer logged
a minute sample while the progress callback was paused. No clean-VM test was
performed.
