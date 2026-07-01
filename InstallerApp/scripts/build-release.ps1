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
  New-Item -ItemType Directory -Path ".\dist\package" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\downloads" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\logs" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\tools\7zip" -Force | Out-Null
  Copy-Item ".\build\windows\x64\release\modlist-installer-gui.exe" ".\dist\modlist-installer.exe" -Force

  $BundledSevenZip = ".\third_party\7zip\7z.exe"
  if (Test-Path $BundledSevenZip) {
    Copy-Item $BundledSevenZip ".\dist\tools\7zip\7z.exe" -Force
  } elseif (Test-Path ".\dist\7z.exe") {
    Move-Item ".\dist\7z.exe" ".\dist\tools\7zip\7z.exe" -Force
  } elseif (-not (Test-Path ".\dist\tools\7zip\7z.exe")) {
    Write-Warning "7z.exe is not present. Put 7z.exe in dist\tools\7zip or third_party\7zip before packaging."
  }

  Get-Item ".\dist\modlist-installer.exe"
} finally {
  Pop-Location
}
