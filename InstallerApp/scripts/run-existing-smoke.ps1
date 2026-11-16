<#
.SYNOPSIS
Compare a chunk-validated package with native 7-Zip without modifying its input.
.DESCRIPTION
Runner must support --describe-package. OutputParent must not exist. Keep outputs
and Reports separate from the input package. The corruption case copies the full
package into a fresh folder below Reports, requiring an additional archive-sized
allocation there. All fixtures, partial outputs and reports are retained.
#>
param(
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$OutputParent,
    [Parameter(Mandatory)][string]$Reports,
    [Parameter(Mandatory)][string]$Runner,
    [Parameter(Mandatory)][string]$SevenZip
)

$ErrorActionPreference = 'Stop'
$Manifest = [IO.Path]::GetFullPath($Manifest)
$OutputParent = [IO.Path]::GetFullPath($OutputParent)
$Reports = [IO.Path]::GetFullPath($Reports)
$package = Split-Path -Parent $Manifest
foreach ($artifacts in @($OutputParent, $Reports)) {
    if ($artifacts -eq $package -or $artifacts.StartsWith($package.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $package.StartsWith($artifacts.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Keep smoke outputs and reports separate from the input package.'
    }
}
$descriptionText = & $Runner --describe-package $Manifest
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the package through ManifestLoader.' }
$description = ($descriptionText -join "`n") | ConvertFrom-Json
if (-not $description.pipeline_supported) { throw 'This helper requires a chunk-validated package; legacy sequential manifests are not supported.' }
if ([long]$description.payload_bytes -le 0) { throw 'This benchmark requires a declared positive unpacked size.' }
$model = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$archive = [IO.Path]::GetFullPath((Join-Path $package $description.first_archive_part))
$listedPaths = @($model.files | ForEach-Object { [IO.Path]::GetFullPath((Join-Path $package $_.path)) })
if ($archive -notin $listedPaths) { throw 'Resolved first archive part is not listed in the manifest.' }
foreach ($file in $listedPaths) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw ('Missing package file: ' + $file) }
}
if (Test-Path -LiteralPath $OutputParent) { throw 'Refusing to overwrite a smoke output folder.' }
[IO.Directory]::CreateDirectory($OutputParent) | Out-Null
[IO.Directory]::CreateDirectory($Reports) | Out-Null
$summaryPath = Join-Path $Reports 'extraction-results.json'
if (Test-Path -LiteralPath $summaryPath) { throw 'Refusing to overwrite smoke results.' }
$summary = [ordered]@{
    status = 'running'; started = [DateTimeOffset]::Now.ToString('o')
    manifest = $Manifest; output_parent = $OutputParent
    payload_bytes = [long]$description.payload_bytes; threads = 4
    first_archive_part = $description.first_archive_part
    native_version = (Get-Item -LiteralPath $SevenZip).VersionInfo.FileVersion
    native_sha256 = (Get-FileHash -LiteralPath $SevenZip -Algorithm SHA256).Hash
    caveat = 'Warm caches; no cache flush. Native excludes SHA-256. Hash comparisons are outside timed extraction.'
    stages = @()
}
function Save-Results {
    [IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}
function Run-Case([string]$Name, [bool]$Native, [int]$ExpectedExit = 0, [int]$CancelPercent = -1, [string]$CaseManifest = $Manifest) {
    $output = Join-Path $OutputParent $Name
    $stdout = Join-Path $Reports ($Name + '.txt')
    $stderr = Join-Path $Reports ($Name + '-progress.txt')
    if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $stdout) -or (Test-Path -LiteralPath $stderr)) {
        throw ('Existing smoke artifact: ' + $Name)
    }
    if ($Native) {
        $exe = $SevenZip
        $arguments = 'x "' + $archive + '" -o"' + $output + '" -y -mmt=4 -bb0 -bsp0'
    } else {
        $exe = $Runner
        $arguments = '"' + $CaseManifest + '" "' + $output + '"'
        if ($CancelPercent -gt 0) { $arguments += ' ' + $CancelPercent }
    }
    Write-Output ('START ' + $Name)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    Write-Output ('PID ' + $process.Id + ' ' + $Name)
    $process.WaitForExit()
    $wall = $timer.Elapsed.TotalSeconds
    $exit = $process.ExitCode
    $metrics = if ($Native) { $null } else { Get-Content -LiteralPath $stdout -Raw | ConvertFrom-StringData }
    $seconds = if ($Native) { $wall } else { [double]::Parse($metrics.seconds, [Globalization.CultureInfo]::InvariantCulture) }
    $summary.stages += [ordered]@{
        name = $Name; exit_code = $exit; expected_exit_code = $ExpectedExit
        wall_seconds = $wall; extraction_seconds = $seconds
        average_output_mib_s = if ($ExpectedExit -eq 0) { [long]$description.payload_bytes / 1MB / $seconds } else { $null }
        output = $output; metrics = $metrics
    }
    Save-Results
    if ($exit -ne $ExpectedExit) { throw ('Unexpected exit code for ' + $Name + ': ' + $exit) }
    if (-not $Native -and $ExpectedExit -eq 0 -and ($metrics.ok -ne '1' -or $metrics.validated_bytes -ne $metrics.validation_total_bytes)) {
        throw ('Incomplete pipeline success: ' + $Name)
    }
    if ($CancelPercent -gt 0 -and ($metrics.cancelled -ne '1' -or $metrics.ok -ne '0')) {
        throw 'Cancellation was ignored.'
    }
    Write-Output ('PASS ' + $Name + ': ' + [math]::Round($seconds, 3) + ' s')
}

Save-Results
try {
    Run-Case 'pipeline-1' $false
    Run-Case 'native-1' $true
    Run-Case 'native-2' $true
    Run-Case 'pipeline-2' $false
    Run-Case 'pipeline-cancel' $false 2 35
    Run-Case 'pipeline-after-cancel' $false

    $fixture = Join-Path $Reports ('corruption-fixture-' + [guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $fixture) { throw 'Corruption fixture already exists.' }
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    $fixtureManifest = Join-Path $fixture 'manifest.json'
    $mutation = $null
    try {
        $copies = @([pscustomobject]@{Source=$Manifest; Target=$fixtureManifest})
        foreach ($entry in $model.files) {
            $source = [IO.Path]::GetFullPath((Join-Path $package $entry.path))
            if ($source -eq $Manifest) { continue }
            $target = [IO.Path]::GetFullPath((Join-Path $fixture $entry.path))
            if (-not $target.StartsWith($fixture + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe corruption fixture path.' }
            $copies += [pscustomobject]@{Source=$source; Target=$target}
        }
        foreach ($copy in $copies) {
            Write-Output ('Copying corruption fixture: ' + [IO.Path]::GetFileName($copy.Source))
            [IO.Directory]::CreateDirectory((Split-Path -Parent $copy.Target)) | Out-Null
            $input = [IO.File]::Open($copy.Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $output = $null
            try {
                $output = [IO.File]::Open($copy.Target, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
                $input.CopyTo($output)
                if ($copy.Source -eq $archive) { $mutation = $output; $output = $null }
            } finally { $input.Dispose(); if ($null -ne $output) { $output.Dispose() } }
        }
        if ($null -eq $mutation -or $mutation.Length -eq 0) { throw 'No copied volume is available for corruption.' }
        $offset = [long][math]::Min(1048699, $mutation.Length - 1)
        $mutation.Position = $offset
        $original = $mutation.ReadByte()
        $mutation.Position = $offset
        $mutation.WriteByte($original -bxor 0x5a)
    } finally {
        if ($null -ne $mutation) { $mutation.Dispose() }
    }
    $summary.corruption_fixture = $fixture
    $summary.source_modified = $false
    Save-Results
    Run-Case 'pipeline-corrupt' $false 1 -1 $fixtureManifest
    $failure = Get-Content -LiteralPath (Join-Path $Reports 'pipeline-corrupt-progress.txt') -Raw
    if (-not $failure.Contains('SHA256 mismatch')) { throw 'Corruption failed for an unexpected reason.' }
    $summary.status = 'passed'
} catch {
    $summary.status = 'failed'
    $summary.error = $_.Exception.ToString()
    throw
} finally {
    $summary.finished = [DateTimeOffset]::Now.ToString('o')
    Save-Results
}
Write-Output ('Extraction report: ' + $summaryPath)
