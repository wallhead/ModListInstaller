param([string]$OutputFolder = "")

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
  xmake f --use_libtorrent=n -m release
  if ($LASTEXITCODE -ne 0) { throw "xmake configuration failed" }
  foreach ($target in @("diagnostics_tests", "diagnostics_integration_tests", "installer_core_tests", "modlist-diagnostics")) {
    xmake build $target
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $target" }
  }
  foreach ($target in @("diagnostics_tests", "installer_core_tests")) {
    xmake run $target
    if ($LASTEXITCODE -ne 0) { throw "Tests failed: $target" }
  }
  if (!$OutputFolder) { $OutputFolder = Join-Path $ProjectRoot "dist\diagnostics" }
  New-Item -ItemType Directory -Path $OutputFolder -Force | Out-Null
  Copy-Item -LiteralPath ".\build\windows\x64\release\modlist-diagnostics.exe" -Destination $OutputFolder -Force
  Copy-Item -LiteralPath ".\resources\7zip-License.txt" -Destination $OutputFolder -Force
  Copy-Item -LiteralPath ".\src\diagnostics\README.md" -Destination (Join-Path $OutputFolder "README.md") -Force
  Get-Item -LiteralPath (Join-Path $OutputFolder "modlist-diagnostics.exe")
} finally {
  Pop-Location
}
