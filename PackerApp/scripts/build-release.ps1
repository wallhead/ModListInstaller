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
  Get-Item ".\dist\modlist-packer.exe"
} finally {
  Pop-Location
}
