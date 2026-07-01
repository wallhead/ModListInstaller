param(
  [switch]$NoLibtorrent
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
  if ($NoLibtorrent) {
    xmake f -m release
  } else {
    xmake f --use_libtorrent=y -m release
  }

  xmake
  xmake run installer_core_tests

  New-Item -ItemType Directory -Path ".\dist" -Force | Out-Null
  Copy-Item ".\build\windows\x64\release\modlist-installer-gui.exe" ".\dist\modlist-installer.exe" -Force

  $BundledSevenZip = ".\third_party\7zip\7z.exe"
  if (Test-Path $BundledSevenZip) {
    Copy-Item $BundledSevenZip ".\dist\7z.exe" -Force
  } elseif (-not (Test-Path ".\dist\7z.exe")) {
    Write-Warning "7z.exe is not present. Put 7z.exe in dist or third_party\7zip before packaging."
  }

  Get-Item ".\dist\modlist-installer.exe"
} finally {
  Pop-Location
}
