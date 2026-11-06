param(
    [Parameter(Mandatory)][string]$SeedArchive,
    [Parameter(Mandatory)][string]$Installer,
    [Parameter(Mandatory)][string]$Root
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Root) { throw 'Refusing to overwrite an existing UI smoke fixture.' }
[IO.Directory]::CreateDirectory($Root) | Out-Null
$valid = Join-Path $Root 'valid'
[IO.Directory]::CreateDirectory($valid) | Out-Null
$input = [IO.File]::OpenRead($SeedArchive)
$buffer = [byte[]]::new(8 * 1024 * 1024)
$files = @()
$index = 0
try {
    while ($true) {
        $count = 0
        while ($count -lt $buffer.Length) {
            $read = $input.Read($buffer, $count, $buffer.Length - $count)
            if ($read -eq 0) { break }
            $count += $read
        }
        if ($count -eq 0) { break }
        $index++
        $name = 'Smoke.7z.{0:D3}' -f $index
        $path = Join-Path $valid $name
        $output = [IO.File]::Open($path, [IO.FileMode]::CreateNew)
        try { $output.Write($buffer, 0, $count) } finally { $output.Dispose() }
        $sha = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $files += [ordered]@{path = $name; size = $count; sha256 = $sha; chunks = @($sha)}
    }
} finally { $input.Dispose() }
if ($files.Count -lt 3) { throw 'UI fixture must contain at least three archive volumes.' }
$manifest = [ordered]@{
    schema = 'modlist-manifest-chunks-v1'
    archive_name = 'UI-SMOKE'
    unpacked_size = 64 * 1024 * 1024
    hash = [ordered]@{algorithm = 'sha256'; chunk_size = 64 * 1024 * 1024}
    files = $files
}
[IO.File]::WriteAllText((Join-Path $valid 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath $Installer -Destination (Join-Path $valid 'modlist-installer.exe')
foreach ($variant in @('cancel', 'corrupt')) {
    $folder = Join-Path $Root $variant
    [IO.Directory]::CreateDirectory($folder) | Out-Null
    Get-ChildItem -LiteralPath $valid -File | Copy-Item -Destination $folder
}
$corruptedVolume = Join-Path $Root 'corrupt\Smoke.7z.003'
$stream = [IO.File]::Open($corruptedVolume, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $stream.Position = 1048699
    $original = $stream.ReadByte()
    $stream.Position = 1048699
    $stream.WriteByte($original -bxor 0x5a)
} finally { $stream.Dispose() }
Write-Output ('Prepared UI smoke fixture: ' + $Root)
Write-Output ('Volumes: ' + $files.Count + '; corruption isolated to the copied third volume.')
