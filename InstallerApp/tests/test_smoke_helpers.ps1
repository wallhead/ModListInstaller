param(
    [ValidateSet('All','Safety','Selection','LeadingSpace','Failure','CopyFailure','Termination','Description')][string]$Case = 'All'
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$helper = Join-Path $project 'scripts\run-existing-smoke.ps1'
$runner = Join-Path $project 'build\windows\x64\release\pipeline_smoke.exe'
$driver = Join-Path $project 'build\windows\x64\release\smoke_script_driver.exe'
$sevenZip = Join-Path $project 'resources\7z.exe'
$root = Join-Path $env:TEMP ('mi-smoke-helper-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$data = Join-Path $root 'data'
$seed = Join-Path $root 'seed'
[IO.Directory]::CreateDirectory((Join-Path $data 'empty\nested')) | Out-Null
[IO.Directory]::CreateDirectory($seed) | Out-Null
$bytes = [byte[]]::new(64 * 1024 * 1024)
[Random]::new(421).NextBytes($bytes)
[IO.File]::WriteAllBytes((Join-Path $data 'payload.bin'), $bytes)
[IO.File]::WriteAllBytes((Join-Path $data 'empty.txt'), [byte[]]::new(0))
$process = Start-Process -FilePath $sevenZip -ArgumentList ('a "' + (Join-Path $seed 'Fixture.7z') + '" * -t7z -mx=1 -v8m -mmt=2 -y -bb0 -bsp0') -WorkingDirectory $data -WindowStyle Hidden -RedirectStandardOutput (Join-Path $root 'create.txt') -Wait -PassThru
if ($process.ExitCode -ne 0) { throw 'Cannot prepare split archive.' }
& (Join-Path $project 'scripts\new-smoke-manifest.ps1') -Package $seed -UnpackedBytes $bytes.Length -ArchiveName 'Fixture'
if (-not $?) { throw 'Cannot prepare manifest.' }

function Source-Hashes([string]$Folder) {
    $hashes = [ordered]@{}
    foreach ($file in Get-ChildItem -LiteralPath $Folder -File) {
        $hashes[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return ($hashes | ConvertTo-Json -Compress)
}

function Invoke-HelperTest([string]$Name, [string]$Mode = 'pass', [bool]$Reorder = $false, [string]$Prefix = '') {
    $caseRoot = Join-Path $root $Name
    $source = Join-Path $caseRoot 'source'
    $reports = Join-Path $caseRoot 'reports'
    $control = Join-Path $caseRoot 'control'
    foreach ($folder in @($source,$reports,$control)) { [IO.Directory]::CreateDirectory($folder) | Out-Null }
    Get-ChildItem -LiteralPath $seed -File | Copy-Item -Destination $source
    $manifest = Join-Path $source 'manifest.json'
    if ($Reorder -or $Prefix) {
        $model = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        if ($Reorder) { $model.files = @($model.files | Sort-Object path -Descending) }
        if ($Prefix) {
            foreach ($entry in $model.files) {
                [IO.File]::Move((Join-Path $source $entry.path), (Join-Path $source ($Prefix + $entry.path)))
                $entry.path = $Prefix + $entry.path
            }
        }
        [IO.File]::WriteAllText($manifest, ($model | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    }
    $before = Source-Hashes $source
    $env:MODLIST_SCRIPT_TEST_RUNNER = $runner
    $env:MODLIST_SCRIPT_TEST_CONTROL = $control
    $env:MODLIST_SCRIPT_TEST_MODE = $Mode
    $gate = $null
    $lockedSource = $null
    if ($Mode -eq 'block-copy') {
        $env:MODLIST_SCRIPT_TEST_GATE = 'Local\mi-smoke-copy-' + [guid]::NewGuid().ToString('N')
        $gate = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $env:MODLIST_SCRIPT_TEST_GATE)
    }
    $output = Join-Path $caseRoot 'output'
    $arguments = '-NoProfile -File "' + $helper + '" -Manifest "' + $manifest + '" -OutputParent "' + $output + '" -Reports "' + $reports + '" -Runner "' + $driver + '" -SevenZip "' + $sevenZip + '"'
    $process = Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardOutput (Join-Path $caseRoot 'helper.txt') -RedirectStandardError (Join-Path $caseRoot 'helper-errors.txt') -PassThru
    try {
        if ($Mode -eq 'block-copy') {
            $marker = Join-Path $control 'after-cancel-finished.txt'
            $deadline = [DateTime]::UtcNow.AddSeconds(60)
            while (-not (Test-Path -LiteralPath $marker) -and -not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 25 }
            if (-not (Test-Path -LiteralPath $marker)) { throw 'Copy-failure test never reached its barrier.' }
            $lockedSource = [IO.File]::Open((Join-Path $source 'Fixture.7z.001'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
            $gate.Set() | Out-Null
        }
        if ($Mode -eq 'hold') {
            $marker = Join-Path $control 'corrupt-entered.txt'
            $deadline = [DateTime]::UtcNow.AddSeconds(60)
            while (-not (Test-Path -LiteralPath $marker) -and -not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 25 }
            if (-not (Test-Path -LiteralPath $marker)) { throw 'Corruption process never reached the termination barrier.' }
            $childId = [int](Get-Content -LiteralPath $marker -Raw)
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
            $child = Get-Process -Id $childId -ErrorAction SilentlyContinue
            if ($child -and $child.Path -eq $driver) { Stop-Process -Id $childId -Force }
        } else {
            if (-not $process.WaitForExit(60000)) { throw 'Smoke helper timed out.' }
            $expected = if ($Mode -in @('fail','block-copy')) { 1 } else { 0 }
            if ($process.ExitCode -ne $expected) {
                throw ('Unexpected helper result (' + $Name + '): ' + (Get-Content -LiteralPath (Join-Path $caseRoot 'helper-errors.txt') -Raw))
            }
        }
        if ($null -ne $lockedSource) { $lockedSource.Dispose(); $lockedSource = $null }
        if ((Source-Hashes $source) -ne $before) { throw ('Source package changed during ' + $Name) }
        if ($Mode -eq 'block-copy') {
            if (Test-Path -LiteralPath (Join-Path $control 'corrupt-entered.txt')) { throw 'Corruption ran after fixture copying failed.' }
            $summary = Get-Content -LiteralPath (Join-Path $reports 'extraction-results.json') -Raw | ConvertFrom-Json
            if ($summary.status -ne 'failed' -or -not $summary.error.Contains('Fixture.7z.001')) { throw 'Fixture-copy failure was not reported.' }
            Write-Output 'PASS fixture-copy-failure: original hashes unchanged; no corruption fallback.'
            return
        }
        $fixtureManifestPath = Get-Content -LiteralPath (Join-Path $control 'corrupt-manifest.txt') -Raw
        if ([IO.Path]::GetFullPath($fixtureManifestPath) -eq [IO.Path]::GetFullPath($manifest)) {
            throw 'Corruption extraction must use a copied fixture, never the supplied package.'
        }
        $fixture = Split-Path -Parent $fixtureManifestPath
        $model = Get-Content -LiteralPath (Join-Path $fixture 'manifest.json') -Raw | ConvertFrom-Json
        $changed = @($model.files | Where-Object { (Get-FileHash -LiteralPath (Join-Path $fixture $_.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $_.sha256 })
        if ($changed.Count -ne 1) { throw 'Deliberate corruption must exist only in one copied fixture volume.' }
        Write-Output ('PASS ' + $Name + ': original hashes unchanged; corruption confined to fixture.')
    } finally {
        if ($null -ne $lockedSource) { $lockedSource.Dispose() }
        if ($null -ne $gate) { $gate.Dispose() }
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        foreach ($name in @('corrupt-entered.txt','after-cancel-finished.txt')) {
            $marker = Join-Path $control $name
            if (Test-Path -LiteralPath $marker) {
                $child = Get-Process -Id ([int](Get-Content -LiteralPath $marker -Raw)) -ErrorAction SilentlyContinue
                if ($child -and $child.Path -eq $driver) { Stop-Process -Id $child.Id -Force }
            }
        }
        Remove-Item Env:\MODLIST_SCRIPT_TEST_RUNNER,Env:\MODLIST_SCRIPT_TEST_CONTROL,Env:\MODLIST_SCRIPT_TEST_MODE,Env:\MODLIST_SCRIPT_TEST_GATE -ErrorAction SilentlyContinue
    }
}

function Test-Description {
    $caseRoot = Join-Path $root 'description'
    [IO.Directory]::CreateDirectory($caseRoot) | Out-Null
    $seedModel = Get-Content -LiteralPath (Join-Path $seed 'manifest.json') -Raw | ConvertFrom-Json
    $legacy = [ordered]@{
        version = '1.0.0'; torrent = @{type='torrent'; source='fixture.torrent'}
        files = @($seedModel.files | Sort-Object path -Descending)
        unpacked_size = $bytes.Length
        extract = @{first_archive_part='Fixture.7z.001'; cleanup_after_success=$false}
    }
    $manifest = Join-Path $caseRoot 'legacy.json'
    [IO.File]::WriteAllText($manifest, ($legacy | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $description = & $runner --describe-package $manifest
    if ($LASTEXITCODE -ne 0) { throw 'Read-only legacy description failed.' }
    $resolved = ($description -join "`n") | ConvertFrom-Json
    if ($resolved.first_archive_part -ne 'Fixture.7z.001' -or $resolved.pipeline_supported -or [long]$resolved.payload_bytes -ne $bytes.Length) {
        throw 'Explicit first archive part was not honored.'
    }
    $output = Join-Path $root 'unsupported-output'
    $reports = Join-Path $root 'unsupported-reports'
    $caught = $false
    try { & $helper -Manifest $manifest -OutputParent $output -Reports $reports -Runner $runner -SevenZip $sevenZip }
    catch {
        if ($_.Exception.Message -notmatch 'legacy sequential manifests are not supported') { throw }
        $caught = $true
    }
    if (-not $caught -or (Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $reports)) { throw 'Unsupported input was not rejected before creating artifacts.' }
    $encodedName = " `t" + [char]0x0421 + 'fixture.7z.001'
    $legacy.archive_name = 'Fixture'
    $legacy.files[0].path = $encodedName
    $legacy.extract.first_archive_part = $encodedName
    [IO.File]::WriteAllText($manifest, ($legacy | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $encoded = & $runner --describe-package $manifest
    if ($LASTEXITCODE -ne 0 -or (($encoded -join "`n") | ConvertFrom-Json).first_archive_part -cne $encodedName) {
        throw 'Description did not preserve UTF-8, leading whitespace and JSON control escaping.'
    }
    $legacy.extract.first_archive_part = 'missing.7z.001'
    [IO.File]::WriteAllText($manifest, ($legacy | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $invalid = & $runner --describe-package $manifest 2>&1
    if ($LASTEXITCODE -eq 0 -or ($invalid -join "`n") -notmatch 'must match an entry') { throw 'Invalid explicit first part was accepted.' }
    Write-Output 'PASS description: explicit first part honored; unsupported/invalid manifests rejected.'
}

if ($Case -in @('All','Safety')) { Invoke-HelperTest 'normal' }
if ($Case -in @('All','Selection')) { Invoke-HelperTest 'reordered' 'pass' $true }
if ($Case -in @('All','LeadingSpace')) { Invoke-HelperTest 'leading-space' 'pass' $true ' ' }
if ($Case -in @('All','Failure')) { Invoke-HelperTest 'injected-failure' 'fail' }
if ($Case -in @('All','CopyFailure')) { Invoke-HelperTest 'fixture-copy-failure' 'block-copy' }
if ($Case -in @('All','Termination')) { Invoke-HelperTest 'forced-termination' 'hold' }
if ($Case -in @('All','Description')) { Test-Description }
Write-Output ('Fixtures retained: ' + $root)
