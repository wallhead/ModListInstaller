ModListInstaller v0.3.4

Keep modlist-packer.exe and modlist-installer.exe together. Run the packer to
create <selected release parent>\<archive name>.

The generated installer expects:
- modlist-installer.exe
- manifest.json
- archive parts

These files are all placed directly in the archive-named folder. The theme,
text, 7-Zip tools, and license are embedded in the installer. On startup, a logs
folder with modlist-installer.log is created beside the executable. Extracted
tools are stored under %LOCALAPPDATA%\ModlistInstaller\<package key>.
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
