param()

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
  xmake f -m release
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  xmake
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  New-Item -ItemType Directory -Path ".\dist" -Force | Out-Null
  Copy-Item ".\build\windows\x64\release\modlist-packer.exe" ".\dist\modlist-packer.exe" -Force

  $RepoRoot = Split-Path -Parent $ProjectRoot
  $InstallerDist = Join-Path $RepoRoot "InstallerApp\dist"
  $InstallerExe = Join-Path $InstallerDist "modlist-installer.exe"
  $InstallerUi = Join-Path $InstallerDist "data\ui"

  if (-not (Test-Path -LiteralPath $InstallerExe)) {
    throw "Installer exe not found: $InstallerExe. Run InstallerApp\scripts\build-release.ps1 first."
  }
  if (-not (Test-Path -LiteralPath $InstallerUi)) {
    throw "Installer UI not found: $InstallerUi. Run InstallerApp\scripts\build-release.ps1 first."
  }

  New-Item -ItemType Directory -Path ".\dist\data" -Force | Out-Null
  if (Test-Path -LiteralPath ".\dist\data\ui") {
    Remove-Item -LiteralPath ".\dist\data\ui" -Recurse -Force
  }
  if (Test-Path -LiteralPath ".\dist\ui") {
    Remove-Item -LiteralPath ".\dist\ui" -Recurse -Force
  }

  Copy-Item -LiteralPath $InstallerExe -Destination ".\dist\modlist-installer.exe" -Force
  Copy-Item -LiteralPath $InstallerUi -Destination ".\dist\data\ui" -Recurse -Force

  Get-Item ".\dist\modlist-packer.exe", ".\dist\modlist-installer.exe"
} finally {
  Pop-Location
}
