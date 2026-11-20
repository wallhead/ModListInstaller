param(
    [string]$Runner = (Join-Path $PSScriptRoot '..\build\windows\x64\release\patch_installer_tests.exe'),
    [string]$WorkRoot = (Join-Path ([IO.Path]::GetTempPath()) ('mi-patch-smoke-' + [guid]::NewGuid().ToString('N'))),
    [string]$DemoPackage
)

$ErrorActionPreference = 'Stop'
$Runner = (Resolve-Path -LiteralPath $Runner).ProviderPath
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$parent = Get-Item -LiteralPath (Split-Path -Parent $WorkRoot) -Force
if (-not $parent.PSIsContainer -or ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
    (Test-Path -LiteralPath $WorkRoot)) {
    throw 'Choose a fresh smoke-test folder under an existing ordinary parent folder.'
}
if ($DemoPackage) {
    $DemoPackage = [IO.Path]::GetFullPath($DemoPackage)
    if ((Test-Path -LiteralPath $DemoPackage) -or $DemoPackage.StartsWith($WorkRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The retained demo package must be new and outside the smoke-test folder.'
    }
}

$sevenZip = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\resources\7z.exe')).ProviderPath
$encoding = [Text.UTF8Encoding]::new($false)
$chunkSize = 32768
$null = New-Item -ItemType Directory -Path $WorkRoot
$oldCache = $env:LOCALAPPDATA
$env:LOCALAPPDATA = Join-Path $WorkRoot 'cache'

function Write-FixtureText([string]$Path, [string]$Text) {
    $null = [IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    [IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Finish-Package([string]$Package, [long]$PayloadSize, [switch]$Legacy) {
    $archiveFiles = @(Get-ChildItem -LiteralPath $Package -File | Sort-Object Name)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $rows = foreach ($part in $archiveFiles) {
            $raw = [IO.File]::ReadAllBytes($part.FullName)
            $chunks = @(
                for ($offset = 0; $offset -lt $raw.Length; $offset += $count) {
                    $count = [Math]::Min($chunkSize, $raw.Length - $offset)
                    ([BitConverter]::ToString($sha.ComputeHash($raw, $offset, $count))).Replace('-', '').ToLowerInvariant()
                }
            )
            [ordered]@{
                path = $part.Name; size = $part.Length; chunks = $chunks
                sha256 = ([BitConverter]::ToString($sha.ComputeHash($raw))).Replace('-', '').ToLowerInvariant()
            }
        }
    } finally { $sha.Dispose() }
    $manifest = [ordered]@{
        schema = 'modlist-manifest-chunks-v1'; archive_name = 'PatchSample'; unpacked_size = $PayloadSize
        hash = [ordered]@{ algorithm = 'sha256'; chunk_size = $chunkSize }; files = @($rows)
    }
    if (-not $Legacy) {
        $manifest.delete_paths = @('mods/Retired Mod', 'Stock Game/Data/obsolete.ini', 'not-present.txt')
    }
    Write-FixtureText (Join-Path $Package 'manifest.json') ($manifest | ConvertTo-Json -Depth 8)
    if ($Legacy) {
        $deletes = @('"\mods\Retired Mod"', '"\Stock Game\Data\obsolete.ini"', '\not-present.txt') -join "`r`n"
        Write-FixtureText (Join-Path $Package 'for delete.txt') ([string][char]0xfeff + $deletes)
        & (Join-Path $PSScriptRoot 'prepare-patch-package.ps1') -Package $Package
    }
}

function Invoke-PatchCase([string]$Package, [string]$Expected = 'success') {
    $logPath = Join-Path $Package 'logs\modlist-patch-installer.log'
    $previous = if (Test-Path -LiteralPath $logPath) { [IO.File]::ReadAllText($logPath) } else { '' }
    $arguments = @('--archive', $Package)
    if ($Expected -eq 'keep-unlisted') { $arguments += 'keep-unlisted' }
    elseif ($Expected -ne 'success') { $arguments += 'failure' }
    $lines = & $Runner @arguments
    if ($LASTEXITCODE -ne 0) { throw "Patch workflow smoke failed: $Package" }
    $log = $lines -join "`n"
    if ($Expected -notin @('success', 'keep-unlisted') -and $log -notmatch $Expected) {
        throw "Fixture failed for the wrong reason: $log"
    }
    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) { throw "Patch did not save a log in $Package" }
    $saved = [IO.File]::ReadAllText($logPath)
    if (-not $saved.StartsWith($previous, [StringComparison]::Ordinal)) { throw 'Patch replaced its previous log' }
    $saved = $saved.Substring($previous.Length)
    $result = if ($Expected -in @('success', 'keep-unlisted')) { 'success' } else { 'failed' }
    if ($saved -notmatch '(?m)^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} PATCH_RUN start ' -or
        $saved -notmatch 'build_id=' -or $saved -notmatch 'Selected modlist:' -or
        $saved -notmatch "PATCH_RUN end status=$result elapsed_ms=\d+") {
        throw "Saved log is missing its timestamp, build, target, or result: $saved"
    }
    if ($Expected -like '*SHA256 mismatch*' -and $saved -notmatch '(?s)Original files restored.*SHA256 mismatch') {
        throw 'Saved log did not record the SHA-256 failure and completed rollback'
    }
    if ($Expected -in @('success', 'keep-unlisted')) {
        if ($saved -notmatch 'manifest_sha256=[0-9a-f]{64}' -or
            $saved -notmatch 'open_ms=\d+ .*extract_ms=\d+ .*final_verify_ms=\d+' -or
            $saved -notmatch 'Archive verified 1/' -or $saved -notmatch 'Patch applied') {
            throw "Saved log is missing package identity, extraction timings, or activity: $saved"
        }
        foreach ($event in @('Manifest loaded:', 'Archive:', 'Delete list:', 'Free space:',
                'Archive paths checked:', 'Removing listed paths', 'Verifying and unpacking the patch',
                'Verification and extraction completed:', 'Checking the destination before replacing files',
                'Replacing files with the unpacked patch',
                'Cleaning up the patch workspace', 'Patch applied')) {
            if ($log -notmatch "(?m)^LOG $([regex]::Escape($event))") {
                throw "Patch log omitted '$event' in $Package"
            }
        }
        $volumes = @(Get-ChildItem -LiteralPath $Package -File |
            Where-Object { $_.Name -match '\.(7z|zip)(\.\d+)?$' } | Sort-Object Name)
        $verified = [regex]::Matches($log, '(?m)^LOG Archive verified \d+/\d+: (.+)$')
        if ($verified.Count -ne $volumes.Count) { throw "Missing or duplicate verification records in $Package" }
        for ($i = 0; $i -lt $volumes.Count; $i++) {
            if ($verified[$i].Groups[1].Value -cne $volumes[$i].Name) {
                throw "Verification log is out of volume order in $Package"
            }
            $part = "$($i + 1)/$($volumes.Count): $($volumes[$i].Name)"
            $verification = $log.IndexOf("LOG Verifying archive $part", [StringComparison]::Ordinal)
            $unpacking = $log.IndexOf("LOG Unpacking archive $part", [StringComparison]::Ordinal)
            if ($verification -lt 0 -or ($unpacking -ge 0 -and $unpacking -lt $verification)) {
                throw "The log did not show verification first for $part in $Package"
            }
        }
        if ($log -notmatch '(?m)^LOG Unpacking archive 1/' -or $log -match '(?m)^LOG .*?(payload\.bin|patch\.esp)$') {
            throw "Extraction log must report archive volumes, not individual files: $Package"
        }
    }
    Write-Output $lines
}

try {
    $source = Join-Path $WorkRoot 'fixture-payload'
    Write-FixtureText (Join-Path $source 'enblocal.ini') 'packed settings'
    Write-FixtureText (Join-Path $source 'Stock Game\Data\patch.esp') 'packed plugin'
    Write-FixtureText (Join-Path $source 'mods\Retired Mod\new.txt') 'packed mod'
    $bytes = [byte[]]::new(160KB)
    [Random]::new(42).NextBytes($bytes)
    [IO.File]::WriteAllBytes((Join-Path $source 'Stock Game\Data\payload.bin'), $bytes)
    $payloadSize = (Get-ChildItem -LiteralPath $source -Recurse -File | Measure-Object -Property Length -Sum).Sum

    foreach ($format in @('7z', 'zip')) {
        foreach ($mode in @('single', 'split')) {
            $package = Join-Path $WorkRoot "$format-$mode"
            $null = New-Item -ItemType Directory -Path $package
            $arguments = @('a', (Join-Path $package "PatchSample.$format"), '*', "-t$format", '-mx=0', '-y', '-bsp0', '-bb0')
            if ($mode -eq 'split') { $arguments += '-v40k' }
            Push-Location $source
            try {
                & $sevenZip @arguments | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "Could not make $format $mode fixture." }
            } finally { Pop-Location }
            if (($mode -eq 'split' -and -not (Test-Path -LiteralPath (Join-Path $package "PatchSample.$format.002"))) -or
                ($mode -eq 'single' -and -not (Test-Path -LiteralPath (Join-Path $package "PatchSample.$format")))) {
                throw 'The generated archive does not have the requested format.'
            }
            Finish-Package $package $payloadSize
            Invoke-PatchCase $package
        }
    }

    $broken = Join-Path $WorkRoot 'broken-copy'
    Copy-Item -LiteralPath (Join-Path $WorkRoot '7z-split') -Destination $broken -Recurse
    $stream = [IO.File]::Open((Join-Path $broken 'PatchSample.7z.002'), [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $location = [long]($stream.Length / 2)
        $stream.Position = $location
        $value = $stream.ReadByte()
        $stream.Position = $location
        $stream.WriteByte([byte]($value -bxor 1))
    } finally { $stream.Dispose() }
    Invoke-PatchCase $broken '(?s)LOG Removing listed paths.+LOG Restoring the original files.+LOG Original files restored.+SHA256 mismatch'

    $legacy = Join-Path $WorkRoot 'legacy'
    $null = New-Item -ItemType Directory -Path $legacy
    Copy-Item -LiteralPath (Join-Path $WorkRoot '7z-single\PatchSample.7z') -Destination $legacy
    Finish-Package $legacy $payloadSize -Legacy
    Invoke-PatchCase $legacy
    [IO.File]::AppendAllText((Join-Path $legacy 'for delete.txt'), "`r`n\enblocal.ini", $encoding)
    Invoke-PatchCase $legacy 'for delete\.txt does not match'

    $inline = Join-Path $WorkRoot 'inline-cases'
    Copy-Item -LiteralPath (Join-Path $WorkRoot '7z-single') -Destination $inline -Recurse
    $model = Get-Content -LiteralPath (Join-Path $inline 'manifest.json') -Raw | ConvertFrom-Json
    Write-FixtureText (Join-Path $inline 'for delete.txt') '\profiles'
    Invoke-PatchCase $inline
    $model.delete_paths = @()
    Write-FixtureText (Join-Path $inline 'manifest.json') ($model | ConvertTo-Json -Depth 8)
    Invoke-PatchCase $inline 'keep-unlisted'
    $legacyModel = Get-Content -LiteralPath (Join-Path $legacy 'manifest.json') -Raw | ConvertFrom-Json
    $inlineFiles = $model.files
    $model.files += @($legacyModel.files | Where-Object path -eq 'for delete.txt')
    Write-FixtureText (Join-Path $inline 'manifest.json') ($model | ConvertTo-Json -Depth 8)
    Invoke-PatchCase $inline 'both.*delete_paths'
    $model.files = $inlineFiles
    foreach ($bad in @('not-an-array', @(123), @('..\outside'), @("mods/one`nprofiles"))) {
        $model.delete_paths = $bad
        Write-FixtureText (Join-Path $inline 'manifest.json') ($model | ConvertTo-Json -Depth 8)
        Invoke-PatchCase $inline 'delete_paths'
    }

    Add-Type -AssemblyName System.IO.Compression
    $collision = Join-Path $WorkRoot 'case-collision'
    $null = New-Item -ItemType Directory -Path $collision
    $file = [IO.File]::Create((Join-Path $collision 'PatchSample.zip'))
    $archive = [IO.Compression.ZipArchive]::new($file, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in @('enblocal.ini', 'ENBLOCAL.INI')) {
            $entry = $archive.CreateEntry($name).Open()
            try { $entry.WriteByte(65) } finally { $entry.Dispose() }
        }
    } finally { $archive.Dispose() }
    Finish-Package $collision 2
    Invoke-PatchCase $collision '(?i)archive output|ambiguous archive'

    if ($DemoPackage) {
        Copy-Item -LiteralPath (Join-Path $WorkRoot '7z-single') -Destination $DemoPackage -Recurse
        Write-Output "Retained ready-to-use demo inputs: $DemoPackage"
    }
} finally {
    $env:LOCALAPPDATA = $oldCache
    $resolved = (Resolve-Path -LiteralPath $WorkRoot).ProviderPath
    $item = Get-Item -LiteralPath $resolved -Force
    if ($resolved -cne $WorkRoot -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        @(Get-ChildItem -LiteralPath $resolved -Recurse -Force -Attributes ReparsePoint).Count) {
        throw "Unsafe path for smoke-test cleanup: $resolved"
    }
    [IO.Directory]::Delete($resolved, $true)
}
