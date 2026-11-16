# ModListInstaller v0.3.7

## Ready-To-Use Setup

Extract `ModlistInstaller-v0.3.7.zip` and keep `modlist-packer.exe` and
`modlist-installer.exe` together. Run the packer to build a release folder, or
put the installer beside the manifest and archives of an existing package.
The ZIP also includes the README and the 7-Zip license.

Both Windows x64 executables include their C/C++ runtime and archive tools.
No separate VC++ Redistributable, installed 7-Zip, or browser runtime is needed.

## Changes

- Verified extraction now uses the matching 7z or ZIP handler. An archive that
  failed to open can no longer be reported as a successful empty installation.
- Validation maintains read-ahead during extraction while checking each block
  before passing it to the decoder.
- The packer embeds and verifies a matching 7-Zip console and codec DLL.
- The packer builds replacements before publishing them and preserves previous
  output on build errors or cancellation. Rebuilds need room for the new
  archive before the old one is replaced.
- Installer cleanup checks folder identity, refuses reparse points, and only
  clears staging leftovers with a matching ownership record.
- Diagnostics and smoke helpers have more reliable progress reporting and
  isolate intentionally damaged copies from the original input.

## Verification

Release builds, runtime-import checks and ten smoke cases passed on Windows,
including real 7z/ZIP roundtrips, direct and staged installation, cancellation
and retry, and rejection of a damaged third volume. The larger fixture had
10,017 files (1.02 GiB); the installed bytes and directory lists matched.
No clean-VM compatibility test was performed.
