param(
    [string]$Runner = (Join-Path $PSScriptRoot '..\build\windows\x64\release\installer_workflow_tests.exe'),
    [string]$WorkRoot = (Join-Path ([IO.Path]::GetTempPath()) ('mi-finalize-' + [guid]::NewGuid().ToString('N')))
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$scripts = Join-Path $project 'scripts'
$sevenZip = Join-Path $project 'resources\7z.exe'
$Runner = (Resolve-Path -LiteralPath $Runner).ProviderPath
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
function Assert-PlainDirectoryChain([string]$Path) {
    for ($dir = $Path; $dir; $dir = Split-Path -Parent $dir) {
        $item = Get-Item -LiteralPath $dir -Force
        if (-not $item.PSIsContainer -or $item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Finalization fixtures require plain directories: $dir"
        }
    }
}
Assert-PlainDirectoryChain (Split-Path -Parent $WorkRoot)
if (Test-Path -LiteralPath $WorkRoot) { throw 'Use a fresh finalization fixture folder.' }
New-Item -ItemType Directory -Path $WorkRoot | Out-Null
$previousCache = $env:LOCALAPPDATA
$env:LOCALAPPDATA = Join-Path $WorkRoot 'cache'
$failures = [Collections.Generic.List[string]]::new()

function New-Package([string]$Name) {
    $source = Join-Path $WorkRoot ($Name + '-source')
    $package = Join-Path $WorkRoot ($Name + '-package')
    New-Item -ItemType Directory -Path $source,$package | Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'control.txt'),'Control payload')
    [pscustomobject]@{Source=$source; Package=$package; Manifest=(Join-Path $package 'manifest.json')}
}

function Build-Package($Fixture) {
    Push-Location $Fixture.Source
    try {
        $lines = & $sevenZip a (Join-Path $Fixture.Package 'Fixture.7z') '*' -t7z -mx=0 -v1m -y -bsp0 -bb0 2>&1
        if ($LASTEXITCODE -ne 0) { throw ($lines -join [Environment]::NewLine) }
    } finally { Pop-Location }
    $bytes = (Get-ChildItem -LiteralPath $Fixture.Source -Recurse -Force -File | Measure-Object Length -Sum).Sum
    & (Join-Path $scripts 'new-smoke-manifest.ps1') -Package $Fixture.Package -ArchiveName Fixture -UnpackedBytes $bytes | Out-Null
    [IO.File]::Copy($Runner,(Join-Path $Fixture.Package 'installer_workflow_tests.exe'))
}

function Use-LegacyManifest($Fixture) {
    $chunked = Get-Content -LiteralPath $Fixture.Manifest -Raw | ConvertFrom-Json
    $legacy = [ordered]@{
        version='1'; archive_name=$chunked.archive_name; unpacked_size=$chunked.unpacked_size
        torrent=@{type='torrent';source='unused-local-fixture.torrent'}
        extract=@{first_archive_part=$chunked.files[0].path;use_same_disk_temp=$true}
        files=@($chunked.files | Select-Object path,size,sha256)
    }
    [IO.File]::WriteAllText($Fixture.Manifest,($legacy | ConvertTo-Json -Depth 6))
}

function Test-Install([string]$Name,$Fixture,[string]$Route,[string]$Option,[string]$Expectation='success') {
    try {
        $output = Join-Path $WorkRoot $Name
        $arguments = @($output,$Route,$Expectation)
        if ($Option) { $arguments += $Option }
        $log = & (Join-Path $Fixture.Package 'installer_workflow_tests.exe') @arguments 2>&1
        $code = $LASTEXITCODE
        $text = $log -join [Environment]::NewLine
        $expectedSuccess = [int]($Expectation -eq 'success')
        if ($code -ne 0 -or $text -notmatch "worker_success=$expectedSuccess") { throw $text }
        if ($Option -and $text -notmatch 'staging_retained=1') { throw 'Expected retained staging outcome was not observed.' }
        $verification = @{
            Source=$Fixture.Source
            Destinations=@((Join-Path $output 'installed\Fixture'))
            Report=(Join-Path $WorkRoot ($Name + '.json'))
        }
        & (Join-Path $scripts 'verify-smoke.ps1') @verification | Out-Null
        Write-Host "PASS $Name"
    } catch {
        $failures.Add($Name)
        Write-Host "FAIL ${Name}: $($_.Exception.Message)"
    }
}

try {
    [IO.Directory]::CreateDirectory($env:LOCALAPPDATA) | Out-Null
    foreach ($kind in @('file','directory')) {
        foreach ($case in @('lower','upper')) {
            $fixture = New-Package "$kind-$case"
            $name = if ($case -eq 'lower') { '.install_temp' } else { '.INSTALL_TEMP' }
            $entry = Join-Path $fixture.Source $name
            if ($kind -eq 'directory') {
                [IO.Directory]::CreateDirectory((Join-Path $entry 'empty')) | Out-Null
                $entry = Join-Path $entry 'payload.txt'
            }
            [IO.File]::WriteAllText($entry,'An ordinary archive entry, not a work folder')
            Build-Package $fixture
            foreach ($route in @('direct','staged')) { Test-Install "$kind-$case-sdk-$route" $fixture $route }
            Use-LegacyManifest $fixture
            foreach ($route in @('direct','staged')) { Test-Install "$kind-$case-cli-$route" $fixture $route }
        }
    }

    $locked = New-Package 'locked'
    $nested = Join-Path $locked.Source 'locked\nested'
    [IO.Directory]::CreateDirectory($nested) | Out-Null
    [IO.File]::WriteAllText((Join-Path $nested 'payload.txt'),'Move this despite the directory lock')
    Build-Package $locked
    Test-Install 'locked-empty-staging' $locked 'staged' '--lock-staged-tree'
    Test-Install 'locked-staging-with-leftover-file' $locked 'staged' '--lock-staged-tree' 'failure'
} finally {
    $env:LOCALAPPDATA = $previousCache
    Assert-PlainDirectoryChain $WorkRoot
    $resolved = (Resolve-Path -LiteralPath $WorkRoot).ProviderPath
    if ($resolved -cne $WorkRoot) { throw "Unexpected finalization fixture root: $resolved" }
    $items = @(Get-ChildItem -LiteralPath $WorkRoot -Recurse -Force)
    if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
        throw "Finalization fixture cleanup refused: $WorkRoot"
    }
    foreach ($item in @($items | Where-Object { -not $_.PSIsContainer })) { [IO.File]::Delete($item.FullName) }
    foreach ($item in @($items | Where-Object PSIsContainer | Sort-Object {$_.FullName.Length} -Descending)) {
        [IO.Directory]::Delete($item.FullName)
    }
    [IO.Directory]::Delete($WorkRoot)
}
if ($failures.Count) { throw "Finalization regressions failed: $($failures -join ', ')" }
Write-Host 'All 18 finalization cases passed; generated payloads removed.'
