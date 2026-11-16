# ModListInstaller v0.3.8

## Ready-To-Use Setup

Extract `ModlistInstaller-v0.3.8.zip` and run `modlist-packer.exe`. Keep
`modlist-installer.exe` beside it so the packer can copy it into new packages.
For an existing package, put the installer beside its manifest and archives.
The ZIP includes both Windows x64 executables, the README, and the 7-Zip license.
No separate VC++ Redistributable, installed 7-Zip, or browser runtime is needed.

## Changes

- The installer preserves legitimate archive entries named `.install_temp`
  during both direct and staged installation.
- A staged install succeeds with a log warning when only locked empty folders
  remain. Leftover files or unsafe staging contents still prevent success.
- The packer distinguishes 7-Zip progress from filenames and log text. It cleans
  up the child process and its handles if packing fails unexpectedly.
- The packer uses shorter temporary paths and rejects release paths that won't
  fit before compression or manifest hashing begins.

## Verification

Both release builds passed the runtime-import checks. All 60 packer tests,
34 folder-cleanup tests, 18 installer finalization cases, and 10 end-to-end
smoke cases passed. The smoke tests covered 7z and ZIP, direct and staged
installation, cancellation and retry, and rejection of a damaged third volume.
The larger fixture had 10,017 files (1.02 GiB); installed file hashes and
directory lists matched the source. No clean-VM test was performed.
