param()

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
  xmake f -m release
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  xmake
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  xmake run packer_tests
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  New-Item -ItemType Directory -Path ".\dist" -Force | Out-Null
  Copy-Item ".\build\windows\x64\release\modlist-packer.exe" ".\dist\modlist-packer.exe" -Force

  $RepoRoot = Split-Path -Parent $ProjectRoot
  $InstallerDist = Join-Path $RepoRoot "InstallerApp\dist"
  $InstallerExe = Join-Path $InstallerDist "modlist-installer.exe"
  if (-not (Test-Path -LiteralPath $InstallerExe -PathType Leaf)) {
    throw "Installer exe not found: $InstallerExe. Run InstallerApp\scripts\build-release.ps1 first."
  }

  Copy-Item -LiteralPath $InstallerExe -Destination ".\dist\modlist-installer.exe" -Force

  Get-Item ".\dist\modlist-packer.exe", ".\dist\modlist-installer.exe"
} finally {
  Pop-Location
}
