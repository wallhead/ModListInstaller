param(
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$OutputParent,
    [Parameter(Mandatory)][string]$Reports,
    [Parameter(Mandatory)][string]$Runner,
    [Parameter(Mandatory)][string]$SevenZip
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputParent) { throw 'Refusing to overwrite a smoke output folder.' }
[IO.Directory]::CreateDirectory($OutputParent) | Out-Null
[IO.Directory]::CreateDirectory($Reports) | Out-Null
$summaryPath = Join-Path $Reports 'extraction-results.json'
if (Test-Path -LiteralPath $summaryPath) { throw 'Refusing to overwrite smoke results.' }
$package = Split-Path -Parent $Manifest
$model = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$archive = Join-Path $package $model.files[0].path
$summary = [ordered]@{
    status = 'running'; started = [DateTimeOffset]::Now.ToString('o')
    manifest = $Manifest; output_parent = $OutputParent
    payload_bytes = $model.unpacked_size; threads = 4
    native_version = (Get-Item -LiteralPath $SevenZip).VersionInfo.FileVersion
    native_sha256 = (Get-FileHash -LiteralPath $SevenZip -Algorithm SHA256).Hash
    caveat = 'Warm caches; no cache flush. Native excludes SHA-256. Hash comparisons are outside timed extraction.'
    stages = @()
}
function Save-Results {
    [IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}
function Run-Case([string]$Name, [bool]$Native, [int]$ExpectedExit = 0, [int]$CancelPercent = -1) {
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
        $arguments = '"' + $Manifest + '" "' + $output + '"'
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
        average_output_mib_s = if ($ExpectedExit -eq 0) { $model.unpacked_size / 1MB / $seconds } else { $null }
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

    $part = $model.files[-1]
    $path = Join-Path $package $part.path
    $offset = [long][math]::Min(1048699, $part.size - 1)
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $stream.Position = $offset
        $original = $stream.ReadByte()
        $journal = [ordered]@{path = $path; offset = $offset; original_byte = $original}
        [IO.File]::WriteAllText((Join-Path $Reports 'mutation-journal.json'), ($journal | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
        $stream.Position = $offset
        $stream.WriteByte($original -bxor 0x5a)
    } finally { $stream.Dispose() }
    try {
        Run-Case 'pipeline-corrupt' $false 1
        $failure = Get-Content -LiteralPath (Join-Path $Reports 'pipeline-corrupt-progress.txt') -Raw
        if (-not $failure.Contains('SHA256 mismatch')) { throw 'Corruption failed for an unexpected reason.' }
    } finally {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Position = $offset; $stream.WriteByte($original) } finally { $stream.Dispose() }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $part.sha256) {
            throw 'Restored archive checksum does not match the manifest.'
        }
        $summary.archive_restored = $true
        Save-Results
    }
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
