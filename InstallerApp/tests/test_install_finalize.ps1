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

function Write-LegacyManifest($Fixture, [long]$UnpackedBytes, $Files) {
    $legacy = [ordered]@{
        version='1'; archive_name='Fixture'; unpacked_size=$UnpackedBytes
        torrent=@{type='torrent';source='unused-local-fixture.torrent'}
        extract=@{first_archive_part=$Files[0].path;use_same_disk_temp=$true}
        files=@($Files | Select-Object path,size,sha256)
    }
    [IO.File]::WriteAllText($Fixture.Manifest,($legacy | ConvertTo-Json -Depth 6))
}

function Build-Package($Fixture, [string]$VolumeSize='1m') {
    $archive = Join-Path $Fixture.Package 'Fixture.7z'
    $options = @('-t7z','-mx=0','-y','-bsp0','-bb0')
    if ($VolumeSize) { $options += "-v$VolumeSize" }
    Push-Location $Fixture.Source
    try {
        $lines = & $sevenZip a $archive '*' @options 2>&1
        if ($LASTEXITCODE -ne 0) { throw ($lines -join [Environment]::NewLine) }
    } finally { Pop-Location }
    $bytes = (Get-ChildItem -LiteralPath $Fixture.Source -Recurse -Force -File | Measure-Object Length -Sum).Sum
    if ($VolumeSize) {
        & (Join-Path $scripts 'new-smoke-manifest.ps1') -Package $Fixture.Package -ArchiveName Fixture -UnpackedBytes $bytes | Out-Null
    } else {
        Write-LegacyManifest $Fixture $bytes @([pscustomobject]@{
            path='Fixture.7z'; size=(Get-Item -LiteralPath $archive).Length
            sha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }
    [IO.File]::Copy($Runner,(Join-Path $Fixture.Package 'installer_workflow_tests.exe'))
}

function Use-LegacyManifest($Fixture) {
    $chunked = Get-Content -LiteralPath $Fixture.Manifest -Raw | ConvertFrom-Json
    Write-LegacyManifest $Fixture $chunked.unpacked_size $chunked.files
}

function Add-ListedSidecar($Fixture) {
    $sidecar = Join-Path $Fixture.Package 'notes.txt'
    [IO.File]::WriteAllText($sidecar,'Listed in the manifest, not part of the archive.')
    $sha = (Get-FileHash -LiteralPath $sidecar -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = Get-Content -LiteralPath $Fixture.Manifest -Raw | ConvertFrom-Json
    $manifest.files += [pscustomobject]@{path='notes.txt';size=(Get-Item -LiteralPath $sidecar).Length;sha256=$sha;chunks=@($sha)}
    [IO.File]::WriteAllText($Fixture.Manifest,($manifest | ConvertTo-Json -Depth 8))
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
        $archiveParts = @(Get-ChildItem -LiteralPath $Fixture.Package -File -Filter 'Fixture.7z*')
        $selectedBytes = ($archiveParts | Measure-Object -Property Length -Sum).Sum
        $sidecar = Join-Path $Fixture.Package 'notes.txt'
        $manifestBytes = $selectedBytes
        if (Test-Path -LiteralPath $sidecar) { $manifestBytes += (Get-Item -LiteralPath $sidecar).Length }
        $packageId = (Get-FileHash -LiteralPath $Fixture.Manifest -Algorithm SHA256).Hash.ToLowerInvariant()
        $isChunked = (Get-Content -LiteralPath $Fixture.Manifest -Raw | ConvertFrom-Json).schema -eq 'modlist-manifest-chunks-v1'
        $config = Select-String -LiteralPath (Join-Path $Fixture.Package 'logs\modlist-installer.log') -Pattern 'INSTALL_PERF .* event=config ' |
            Select-Object -Last 1 -ExpandProperty Line
        $expectedBytes = if ($isChunked) { $selectedBytes } else { 'unavailable' }
        $expectedVolumes = if ($isChunked) { $archiveParts.Count } else { 'unavailable' }
        foreach ($field in @("archive_bytes=$manifestBytes","manifest_bytes=$manifestBytes",
                "selected_archive_bytes=$expectedBytes","selected_volumes=$expectedVolumes","package_id=$packageId")) {
            if ($config -notmatch (' ' + $field + '( |$)')) { throw "Wrong or missing config field: $field" }
        }
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

function Add-LongPathPayload($Fixture, [string]$Case, [string]$RelativeDirectory='') {
    $staging = Join-Path $WorkRoot "$Case\Unpacking"
    $source = $Fixture.Source
    if ($RelativeDirectory) {
        $staging = Join-Path $staging $RelativeDirectory
        $source = Join-Path $source $RelativeDirectory
        [IO.Directory]::CreateDirectory($source) | Out-Null
    }
    # The staged path is below MAX_PATH; the installed path is longer by 8 units.
    $stemLength = 258 - $staging.Length - 1 - '.txt'.Length
    if ($stemLength -lt 1 -or $stemLength -gt 251) { throw 'Use a shorter work root for the long-path fixture.' }
    [IO.File]::WriteAllText((Join-Path $source (('x' * $stemLength) + '.txt')), 'Known long-path payload')
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
            if ($kind -eq 'file' -and $case -eq 'lower') { Add-ListedSidecar $fixture }
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

    $long = New-Package 'long-path'
    Add-LongPathPayload $long 'long-final-placement'
    Build-Package $long
    Test-Install 'long-final-placement' $long 'staged'

    $fallback = New-Package 'long-locked'
    Add-LongPathPayload $fallback 'long-directory-fallback' 'locked\nested'
    Build-Package $fallback
    Test-Install 'long-directory-fallback' $fallback 'staged' '--lock-staged-tree'

    $cli = New-Package 'cli-2147483648%'
    # Keep the CLI running long enough for its banner to reach live progress parsing.
    for ($i = 0; $i -lt 6000; $i++) {
        [IO.File]::WriteAllText((Join-Path $cli.Source ('payload-{0:D5}.txt' -f $i)), 'CLI progress payload')
    }
    Build-Package $cli ''
    Test-Install 'cli-percent-in-path' $cli 'direct'

    $split = New-Package 'split-archive'
    $bytes = [byte[]]::new(1000000)
    [Random]::new(610).NextBytes($bytes)
    [IO.File]::WriteAllBytes((Join-Path $split.Source 'payload.bin'), $bytes)
    Build-Package $split '1000b'
    if (@(Get-ChildItem -LiteralPath $split.Package -File -Filter 'Fixture.7z.*').Count -ne 1001) {
        throw 'Expected a 1,001-volume archive fixture.'
    }
    Test-Install 'split-volumes-past-999' $split 'direct'

    # Change only a byte of the generated volume, after manifest creation.
    $latePart = [IO.File]::Open((Join-Path $split.Package 'Fixture.7z.1000'), 'Open', 'ReadWrite', 'None')
    try {
        $firstByte = $latePart.ReadByte()
        $latePart.Position = 0
        $latePart.WriteByte($firstByte -bxor 1)
    } finally { $latePart.Dispose() }
    $corruption = & (Join-Path $split.Package 'installer_workflow_tests.exe') (Join-Path $WorkRoot 'late-split-corruption') direct failure 2>&1
    if ($LASTEXITCODE -ne 0 -or ($corruption -join "`n") -notmatch 'SHA256 mismatch in Fixture\.7z\.1000') {
        $failures.Add('late-split-corruption')
        Write-Host ('FAIL late-split-corruption: ' + ($corruption -join "`n"))
    } else {
        Write-Host 'PASS late-split-corruption'
    }
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
Write-Host 'All 23 unpacking and finalization cases passed; generated payloads removed.'
