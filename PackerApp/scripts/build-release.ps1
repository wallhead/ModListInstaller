param(
  [string]$InstallerFolder = (Join-Path $PSScriptRoot '..\..\InstallerApp\dist'),
  [string]$OutputFolder = (Join-Path $PSScriptRoot '..\dist')
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$RepoRoot = Split-Path -Parent $ProjectRoot
$InstallerFolder = (Resolve-Path -LiteralPath $InstallerFolder).ProviderPath
$OutputFolder = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputFolder)
$InstallerExe = Join-Path $InstallerFolder "modlist-installer.exe"
$PatchExe = Join-Path $InstallerFolder "modlist-patch-installer.exe"
if (-not (Test-Path -LiteralPath $InstallerExe -PathType Leaf)) {
  throw "Installer exe not found: $InstallerExe. Run InstallerApp\scripts\build-release.ps1 first."
}
if (-not (Test-Path -LiteralPath $PatchExe -PathType Leaf)) {
  throw "Patch installer exe not found: $PatchExe. Run InstallerApp\scripts\build-patch-installer.ps1 first."
}

Push-Location $ProjectRoot
try {
  xmake f -m release
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  xmake
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  $TestArgs = @('--installer-source', $InstallerFolder)
  $PatchRunner = Join-Path $InstallerFolder 'patch_installer_tests.exe'
  if (Test-Path -LiteralPath $PatchRunner -PathType Leaf) {
    $TestArgs += @('--patch-runner', $PatchRunner)
  }
  xmake run packer_tests @TestArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  $PackerExe = ".\build\windows\x64\release\modlist-packer.exe"
  & (Join-Path $RepoRoot "InstallerApp\scripts\test-runtime-dependencies.ps1") -Binary @(
    $PackerExe, $InstallerExe, $PatchExe,
    ".\resources\7zip\7z.exe", ".\resources\7zip\7z.dll")

  New-Item -ItemType Directory -Path $OutputFolder -Force | Out-Null
  Copy-Item -LiteralPath $PackerExe, $InstallerExe, $PatchExe -Destination $OutputFolder -Force
  Get-Item -LiteralPath @('modlist-packer.exe', 'modlist-installer.exe', 'modlist-patch-installer.exe' |
    ForEach-Object { Join-Path $OutputFolder $_ })
} finally {
  Pop-Location
}
