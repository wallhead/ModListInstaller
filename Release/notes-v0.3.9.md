# ModListInstaller v0.3.9

## Ready-To-Use Setup

Extract `ModlistInstaller-v0.3.9.zip` and run `modlist-packer.exe` with
`modlist-installer.exe` beside it. The ZIP includes both portable Windows x64
executables, the README, and the 7-Zip license. No separate VC++ Redistributable,
installed 7-Zip, or browser runtime is needed.

For an existing package, replace only the installer next to the manifest and
archive volumes. No repacking is needed.

## Changes

- `logs\modlist-installer.log` now groups timing records by install attempt.
  The records identify the build, drives, direct or staged route, verification
  mode, and time spent in each phase. Waiting for cleanup confirmation is
  logged separately from cleanup itself.
- Verified extraction reports input, validation, output, and wait counters
  once a minute, even without progress callbacks, and again when it ends.
  These counters are cumulative and some timings overlap.
- Diagnostic records stay in the log file. The UI and package format have not
  changed, and there is no new per-file logging.

## Verification

Both release builds passed the runtime-import checks. All 60 packer tests,
18 installer workflow cases, and 10 end-to-end smoke cases passed. The smoke
tests covered 7z and ZIP, direct and staged installation, cancellation and
retry, and rejection of a damaged volume. A deliberate pause produced a
one-minute timing sample without progress callbacks, then the installed
payload passed SHA-256 verification. No clean-VM test was performed.
