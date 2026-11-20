# Stock Game Patch Installer

## Scope

Build a separate portable Windows executable, `modlist-patch-installer.exe`,
using the existing installer dialogs and verified 7-Zip extractor. Its only
installation target is an existing `Stock Game` folder. It does not inspect
the parent modlist, identify game versions, compare existing file contents, or
create an extra archive-named folder. The regular installer and packer keep
their current behavior.

## Approaches

- Patch the modlist parent: supports broader updates, but contradicts the chosen
  `Stock Game` scope. The parent folder can have any name and is not a target.
- Delete and extract directly into `Stock Game`: simple, but a damaged archive,
  cancellation, or sharing error can leave deleted or partially replaced files.
- Remove the listed paths, unpack into private staging, then merge into
  `Stock Game`: preserves the requested order and can restore affected files
  on failure. This is the proposed implementation. It needs room for the
  unpacked patch while the originals are still on disk.

## Target and Package

Read `Installed Path` from the local-machine `SOFTWARE\Bethesda Softworks\Skyrim
Special Edition` key, checking the 32-bit view first and then the 64-bit view.
Use it only if it resolves to an existing, non-redirected `Stock Game` folder.
Show `Найдена копия сборки по пути «{path}». Это верный путь?` with `Да` and `Нет`.
`Нет`, a missing key, or an unsuitable value opens a folder picker for `Stock
Game`. A manual selection must meet the same folder checks. Cancel writes
nothing to the destination. No registry writes or disk search are needed.

Ship the patch EXE, `for delete.json`, a matching current packer `manifest.json`,
and the archive together outside the target. Current single-file and split 7z
and ZIP packages use the same archive verification as the installer. Pack the
contents relative to `Stock Game`, not the wrapper folder itself. For example,
`Data/Patch.esp` becomes `Stock Game\Data\Patch.esp`.

`for delete.json` is a required UTF-8 object with the SHA-256 of the exact
`manifest.json` and two arrays of relative paths. This catches accidentally
mixed packages without changing the packer or checking the installed game.
Replace the example digest with the output of
`(Get-FileHash -LiteralPath .\manifest.json -Algorithm SHA256).Hash`.
Either array may be empty. Missing listed items are harmless; an existing file
listed as a folder or vice versa is an error.

```json
{
  "manifest_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "files": ["Data/obsolete.ini"],
  "folders": ["Data/Old Mod"]
}
```

## Apply Order and Failure Handling

1. Before changing game files, read the delete list and manifest, match the
   manifest digest, check the archive is available, validate paths and space,
   and create a new, owned work directory on the destination volume. Preflight
   does not unpack the patch.
2. Remove the listed files and folder trees from `Stock Game` by moving them
   into backup storage in the work directory. Finish all listed removals
   before calling the extractor. Keep recovery records for these moves.
3. Verify and unpack the archive into a fresh staging folder in the work
   directory. Merge the staged files into `Stock Game` with replacement,
   backing up original files before installing replacements. Merge existing
   directories; do not remove their other contents unless explicitly listed.
4. After the merge succeeds, commit the result, then remove only the patcher's
   own temporary data and backups. On error or cancellation before commit,
   undo changes made by this run and restore originals. If restore is blocked,
   report the failing path and keep the backup. Persist enough information for
   manual recovery, including whether the merge committed before cleanup. On
   restart, report the unresolved workspace and block a new patch rather than
   attempting automatic recovery across process exits. Never treat it as
   disposable leftover data or roll back a committed patch.

Use the existing Windows path rules and no-follow folder handling. Reject
absolute paths, traversal, Windows device/stream names, redirected paths,
case aliases or overlapping delete entries that would make recovery ambiguous,
and conflicts with the app's work directory. Refuse overlap with the patch
package. Recheck path identity when moving or replacing entries; never truncate
an old file in place. Use `Stock Game\.modlist-patch` as a reserved work directory
created exclusively for the run. An unowned existing directory at that path
must not be adopted or cleared. Only the explicit delete paths, archive output
paths, and their necessary ancestors are examined. This is not an inventory of
installed files.

Close Skyrim, MO2, and other tools that write to `Stock Game` while applying a
patch. The patcher should prevent simultaneous patch runs and report locked or
changed paths; it does not terminate other apps or coordinate their writes.

## Small Test Fixture

Use dummy files in a fresh test folder, patterned after the real
`D:\TRUEAE_V54_EXT\Stock Game`: a placeholder `SkyrimSE.exe`, a few `Data` files,
one obsolete folder tree, and one unrelated file. Add a dummy `profiles` save
outside `Stock Game` to prove the parent is untouched. Repeat target selection
with a different parent folder name using injected registry values. Do not copy
game binaries, edit the live registry, or apply a test patch to the real game.

Cover deletion before extraction, replacement and addition, a deleted folder
recreated by the archive, already missing entries, a mismatched manifest hash,
invalid paths and redirects, manual selection and cancel, damaged archive
rollback, and a locked destination. Check that a pending recovery is preserved
and distinguish it from a committed patch with unfinished cleanup. Check
fixture contents after both success and failure. No full modlist copy is needed.
