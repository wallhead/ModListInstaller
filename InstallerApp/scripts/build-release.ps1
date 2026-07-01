$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
  xmake f --use_libtorrent=n -m release

  xmake
  xmake run installer_core_tests

  New-Item -ItemType Directory -Path ".\dist" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\package" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\downloads" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\logs" -Force | Out-Null
  Copy-Item ".\build\windows\x64\release\modlist-installer-gui.exe" ".\dist\modlist-installer.exe" -Force

  if (Test-Path ".\dist\tools\7zip\7z.exe") {
    Remove-Item ".\dist\tools\7zip\7z.exe" -Force
  }

  Get-Item ".\dist\modlist-installer.exe"
} finally {
  Pop-Location
}
