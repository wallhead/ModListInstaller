$ErrorActionPreference = 'Stop'

$project = Split-Path -Parent $PSScriptRoot
Push-Location $project
try {
    xmake f --use_libtorrent=n -m release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    foreach ($test in @('patch_installer_tests', 'patch_registry_tests')) {
        xmake build $test
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        xmake run $test
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    xmake build modlist-patch-installer
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $exe = '.\build\windows\x64\release\modlist-patch-installer.exe'
    & "$PSScriptRoot\test-runtime-dependencies.ps1" -Binary @(
        $exe, '.\resources\7z.exe', '.\resources\7z.dll')
    New-Item -ItemType Directory -Path '.\dist' -Force | Out-Null
    Copy-Item -LiteralPath $exe -Destination '.\dist\modlist-patch-installer.exe' -Force
    Get-Item -LiteralPath '.\dist\modlist-patch-installer.exe'
} finally {
    Pop-Location
}
