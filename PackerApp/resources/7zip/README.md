# Embedded 7-Zip Runtime

Copied byte-for-byte from the repository's existing `InstallerApp/resources`
26.02 runtime pair and license on 2026-11-06. No download was used. Both PE
files report file version 26.02. `app.rc` embeds these files, not the legacy
ignored `resources/7z.exe` console.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| 7z.exe | 576000 | 83967f1b02b43c4efeda302795722c809e0e81b8307de73558d10484d5676a7d |
| 7z.dll | 1906688 | 69fd4df057985c40e510e2fac182881c7f85e90aa13ec703f763a8fdb2ce61f8 |
| 7zip-License.txt | 6031 | 519ac0a4bded9c18ea02e0afb71f663d8c47373bd9facd3ac96a79f51d77765d |

The original license is embedded and deployed alongside both binary components.
Keep this pair and license matched when updating. The console loads archive
codecs from its sibling DLL; shipping only the standard console is insufficient.

Upstream project and source distribution: <https://www.7-zip.org/>.
