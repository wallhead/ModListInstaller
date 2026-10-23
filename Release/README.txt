ModListInstaller v0.3.4

Run modlist-packer.exe to create a complete modlist release folder.

The generated installer expects:
- data\package\manifest.json
- archive parts in data\downloads
- native theme in data\ui
- bundled support files in data\tools

The native Direct2D interface reads its editable theme from data\ui\style.css
and its editable visible text from data\ui\strings.json.
It does not require WebView2, Edge, .NET, or internet access.

The final folder is <selected install root>\<archive_name>. Before extraction,
the installer checks all archive entry paths. If every full destination path
fits within 240 UTF-16 code units, it extracts directly into the final folder.
Otherwise, or when archive metadata cannot be inspected, it uses the install
drive's short Unpacking path and then moves the files into the final folder.
Successful staged installs remove the empty Unpacking folder. Direct installs
do not create or clean the unused Unpacking folder.

If the final <selected install root>\<archive_name> folder is not empty, the
installer asks for confirmation before permanently deleting everything inside
that folder and starting installation again.

When installation starts, the installer finds the Windows Documents known
folder and creates these folders if they are missing:
- My Games\Skyrim Special Edition
- My Games\Fallout4
