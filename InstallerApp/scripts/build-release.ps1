$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
  xmake f --use_libtorrent=n -m release
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  xmake
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  xmake run installer_core_tests
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  New-Item -ItemType Directory -Path ".\dist" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\data\package" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\data\downloads" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\data\logs" -Force | Out-Null
  New-Item -ItemType Directory -Path ".\dist\data\tools\7zip" -Force | Out-Null
  Copy-Item ".\resources\7zip-License.txt" ".\dist\data\tools\7zip\License.txt" -Force
  if (Test-Path ".\dist\data\tools\webview2") {
    Remove-Item ".\dist\data\tools\webview2" -Recurse -Force
  }
  if (Test-Path ".\dist\data\ui") {
    Remove-Item ".\dist\data\ui" -Recurse -Force
  }
  if (Test-Path ".\dist\ui") {
    Remove-Item ".\dist\ui" -Recurse -Force
  }
  New-Item -ItemType Directory -Path ".\dist\data\ui" -Force | Out-Null
  Copy-Item ".\ui\style.css" ".\dist\data\ui\style.css" -Force
  Copy-Item ".\ui\strings.json" ".\dist\data\ui\strings.json" -Force
  Copy-Item ".\build\windows\x64\release\modlist-installer-gui.exe" ".\dist\modlist-installer.exe" -Force

  if (Test-Path ".\dist\tools") {
    Remove-Item ".\dist\tools" -Recurse -Force
  }
  if (Test-Path ".\dist\logs") {
    Remove-Item ".\dist\logs" -Recurse -Force
  }
  if (Test-Path ".\dist\downloads") {
    Remove-Item ".\dist\downloads" -Recurse -Force
  }
  if (Test-Path ".\dist\package") {
    Remove-Item ".\dist\package" -Recurse -Force
  }
  if (Test-Path ".\dist\last-7z-output.log") {
    Remove-Item ".\dist\last-7z-output.log" -Force
  }

  Get-Item ".\dist\modlist-installer.exe"
} finally {
  Pop-Location
}
