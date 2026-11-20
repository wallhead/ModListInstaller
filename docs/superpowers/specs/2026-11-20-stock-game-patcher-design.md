# Stock Game Patch Installer

## Scope

Build a separate portable Windows executable, `modlist-patch-installer.exe`,
using the existing installer dialogs and verified 7-Zip extractor. Its target
is the newer-layout modlist root: the immediate parent of `Stock Game`, beside
`mods` and `ModOrganizer.exe`. The root may have any name. The patcher does not
inventory installed files, identify game versions, compare existing file
contents, or create an extra archive-named folder. The regular installer and
the packer's default Full package mode keep their current behavior. Do not
translate legacy `MO2` paths.

## Approaches

- Patch only `Stock Game`: cannot update the new layout's sibling `mods` and
  root-level files, so it is not the target.
- Delete and extract directly into the root: simple, but a damaged archive,
  cancellation, or sharing error can leave deleted or partially replaced files.
- Remove the listed paths, unpack into private staging, then merge into
  the modlist root: preserves the requested order and can restore affected
  files on failure. This is the approved approach. It needs room for the
  unpacked patch while the originals are still on disk.

## Target and Package

Read `Installed Path` from the local-machine `SOFTWARE\Bethesda Softworks\Skyrim
Special Edition` key, checking the 32-bit view first and then the 64-bit view.
Use it only if it resolves to an existing, non-redirected `Stock Game` folder.
Check that its parent is a modlist root using the newer layout's markers. Show
`Найдена копия сборки по пути «{path}». Это верный путь?` with `Да` and `Нет`,
using the root, not the child, as `{path}`. `Нет`, a missing key, or an unsuitable
value opens a folder picker. Accept the root or its `Stock Game` child and
confirm the resulting root. The full installer EXE is not a required marker.
Cancel writes nothing to the destination. No registry writes or disk search.

In the packer's Patch mode, select the patch contents, enter the deletion paths,
and build the archive. Ship the patch EXE, the generated `manifest.json`, and
matching archive together outside the target. Current single-file and split
7z and ZIP packages use the existing verified extractor. Pack contents relative
to the modlist root, without a wrapper folder. The packer writes `delete_paths`
into the manifest. The author does not edit JSON.

The deletion field takes one exact path per line, with optional straight outer
quotes and blank lines allowed. A single leading backslash is a modlist-root
marker, not a drive root. Backslashes do not need escaping. Missing items are
skipped. Existing directories are removed with their contents. An empty list
is valid. Absolute paths from Explorer must have their authoring root removed.

```text
"\mods\Some Mod"
\somefile.exe
"\Stock Game\Data\old.ini"
```

## Apply Order and Failure Handling

1. Before changing game files, read the deletion paths from the manifest,
   check the archive is available, validate paths and space,
   and create a new, owned work directory on the destination volume. Preflight
   does not unpack the patch.
2. Remove the listed files and folder trees from the modlist root by moving them
   into backup storage in the work directory. Finish all listed removals
   before calling the extractor. Keep recovery records for these moves.
3. Verify and unpack the archive into a fresh staging folder in the work
   directory. Merge the staged files into the modlist root with replacement,
   backing up original files before installing replacements. Merge existing
   directories; do not remove their other contents unless explicitly listed.
4. After the merge succeeds, commit the result, then remove only the patcher's
   own temporary data and backups. On error or cancellation before commit,
   undo changes made by this run and restore originals. If restore is blocked,
   report the failing path and keep the backup. Persist enough information for
   manual recovery, including whether the merge committed before cleanup. On
   restart, report the unresolved workspace and block a new patch rather than
   attempting automatic recovery across process exits. Preserve the record
   until all temporary payload and saved originals are gone. If the final
   commit record cannot be saved, keep originals and report the uncertain
   state instead of treating the patch as completed.

Use the existing Windows path rules and no-follow folder handling. Reject
absolute paths, traversal, Windows device/stream names, redirected paths,
case aliases or overlapping delete entries that would make recovery ambiguous,
and conflicts with the app's work directory. Refuse overlap with the patch
package. Recheck path identity when moving or replacing entries; never truncate
an old file in place. Use `<modlist root>\.modlist-patch` as a reserved work directory
created exclusively for the run. An unowned existing directory at that path
must not be adopted or cleared. Only the explicit delete paths, archive output
paths, and their necessary ancestors are examined. This is not an inventory of
installed files.

Close Skyrim, MO2, and other tools that write to the modlist while applying a
patch. The patcher should prevent simultaneous patch runs and report locked or
changed paths; it does not terminate other apps or coordinate their writes.

## Small Test Fixture

Use dummy files in a fresh test folder patterned after `D:\TRUEAE_V54_EXT`:
placeholder executables, `Stock Game\Data`, nested `mods`, one obsolete tree,
an unrelated dummy profile save, and a sentinel outside the modlist. Repeat
selection with a differently named root. Do not copy game binaries, edit the
live registry, or apply a test patch to the real game.

Cover deletion before extraction, replacement and addition, a deleted folder
recreated by the archive, already missing entries, a mismatched manifest hash,
invalid paths and redirects, manual selection and cancel, damaged archive
rollback, and a locked destination. Check that a pending recovery is preserved
and distinguish it from a committed patch with unfinished cleanup. Check
fixture contents after both success and failure. No full modlist copy is needed.
