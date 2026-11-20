# ModListInstaller v0.3.11

## Ready-To-Use Setup

Extract `ModlistInstaller-v0.3.11.zip`, keep all three Windows x64 EXEs together,
and run `modlist-packer.exe`. The packer uses `modlist-installer.exe` for full
packages and `modlist-patch-installer.exe` for patch packages. The ZIP also
includes the README and 7-Zip license. No separate VC++ Redistributable, 7-Zip,
or browser runtime is needed.

For an existing full package, replace `modlist-installer.exe` beside the
manifest and archive volumes. No repacking is needed.

## Changes

- Staged installs can now move files into final paths beyond the legacy
  Windows path limit.
- The 7-Zip command-line progress parser no longer mistakes percent signs in
  paths for progress or throws on oversized numbers.
- Split archives are read in numeric order, including `.1000` and later.
  SHA-256 verification still covers every selected volume.
- The patch installer is bundled for the first time. It supports the newer
  `Stock Game` modlist layout and saves a separate patch log. In the resizable
  packer, select Patch mode to package replacement files and a list of paths
  to delete.

## Verification

Both installers embed clean source revision
`f577988ae66157d6de1b30a4564fe206116d9db5`. Seven Windows test targets, 23
unpacking and finalization smoke cases, and the patch-package smoke tests
passed. All 63 packer tests passed using the installers extracted from the ZIP,
including applying a generated patch to an isolated modlist. All three shipped
EXEs passed the static-runtime import check.
