$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
  xmake f --use_libtorrent=n -m release
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  # Recompile resources so CSS and translation edits are embedded in this build.
  xmake -r
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  xmake run installer_core_tests
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  xmake run installer_ui_tests
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  New-Item -ItemType Directory -Path ".\dist" -Force | Out-Null
  Copy-Item ".\build\windows\x64\release\modlist-installer-gui.exe" ".\dist\modlist-installer.exe" -Force

  Get-Item ".\dist\modlist-installer.exe"
} finally {
  Pop-Location
}
