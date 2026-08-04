ModListInstaller

Run modlist-packer.exe to create a complete modlist release folder.

The generated installer expects:
- data\package\manifest.json
- archive parts in data\downloads
- native theme in data\ui
- bundled support files in data\tools

The native Direct2D interface reads its editable theme from data\ui\style.css
and its editable visible text from data\ui\strings.json.
It does not require WebView2, Edge, .NET, or internet access.

The installer extracts to the selected drive's short Unpacked path, then moves
the files into <selected install root>\<archive_name>. After a successful
installation, the empty Unpacked folder is removed.

If the final <selected install root>\<archive_name> folder is not empty, the
installer asks for confirmation before permanently deleting everything inside
that folder and starting installation again.

When installation starts, the installer finds the Windows Documents known
folder and creates these folders if they are missing:
- My Games\Skyrim Special Edition
- My Games\Fallout4
