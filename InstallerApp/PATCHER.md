# Modlist Patch Installer

`modlist-patch-installer.exe` is a separate portable Windows x64 app. Use it
for the newer layout, where `Stock Game`, `mods`, and `ModOrganizer.exe` share
the modlist root. It does not support the old MO2-inside-game layout.

## Make a Patch Package

1. Keep `modlist-patch-installer.exe` beside `modlist-packer.exe`. In the packer,
   select **Package mode > Patch** and choose the **Patch files folder**. Put
   new or replacement files directly in their modlist-relative locations, such
   as `mods\Example Mod\file.dds` or `Stock Game\Data\Patch.esp`, with no wrapper
   folder. A single archive or split volumes both work.
2. Under **Deletion paths**, paste one path per line. Keep quotes from Explorer
   if present, but remove the drive and old modlist folder prefix. For example:

   ```text
   "\mods\Some Retired Mod"
   "\Stock Game\Data\obsolete.ini"
   \oldfile.exe
   ```

   A listed directory includes its contents. Missing items are skipped.
   Leave the field blank if the patch only adds or replaces files.
3. Choose a release folder and archive name, then click **Build Patch**. Ship the
   resulting archive-named folder with `modlist-patch-installer.exe`,
   `manifest.json`, and the archive file(s). The manifest carries the deletion
   paths in `delete_paths`; no separate text file or packaging script is needed.

## Apply

Close Skyrim, MO2, and other apps that write to the modlist. Keep the patch
package outside the modlist. The patcher finds `Stock Game` through the Skyrim
registry, confirms its parent, and can also accept a manual selection.

Listed items are removed from the modlist first. Then the archive is verified
and unpacked into a private staging folder, and the files are moved into place
with replacement. Unlisted files are not removed unless they are inside an
explicitly listed directory. Leave enough space for the unpacked patch while
the moved originals are retained.

The log pane shows the manifest, archive volumes, free-space check, deletion
counts, and verification, extraction, replacement, and rollback stages.
After the modlist is confirmed, the installer appends a timestamped run to
`logs\modlist-patch-installer.log` next to the patch EXE. The saved log adds
the build and manifest IDs, 7-Zip timings and counters, the final result,
and any recovery path. **Открыть лог** opens the file. If the package
folder cannot be written to safely, the log goes to the user's installer
cache instead; the log pane shows its actual path.

After an ordinary failure, the patcher attempts to restore the originals. On
interruption or a blocked restore, it keeps `.modlist-patch\recovery.txt` and any
remaining originals. Check the recovery path shown by the app before another
run. Do not restore backups over a completed patch whose record ends
with `COMMITTED`. The patcher does not automatically adopt or clear a leftover
workspace.

## Build and Test

From `InstallerApp`, run:

```powershell
.\scripts\build-patch-installer.ps1
.\scripts\test-patch-package.ps1
```

The patch EXE is built into `dist` without replacing the full installer.
The smoke script uses small generated directories and archives, including a
damaged copy. It never writes to the actual Skyrim installation or registry.
