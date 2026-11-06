param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][long]$UnpackedBytes,
    [string]$ArchiveName = 'Smoke10G'
)

$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $Package 'manifest.json'
if (Test-Path -LiteralPath $manifestPath) { throw 'Refusing to overwrite a manifest.' }
$parts = @(Get-ChildItem -LiteralPath $Package -File | Where-Object { $_.Name -match '\.7z\.\d{3,}$' } | Sort-Object Name)
if ($parts.Count -eq 0) { throw 'No archive volumes found.' }
$buffer = [byte[]]::new(64 * 1024 * 1024)
$files = @()
foreach ($part in $parts) {
    $input = [IO.File]::Open($part.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $full = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    $sha = [Security.Cryptography.SHA256]::Create()
    $chunks = [Collections.Generic.List[string]]::new()
    try {
        while ($true) {
            $count = 0
            while ($count -lt $buffer.Length) {
                $read = $input.Read($buffer, $count, $buffer.Length - $count)
                if ($read -eq 0) { break }
                $count += $read
            }
            if ($count -eq 0) { break }
            $full.AppendData($buffer, 0, $count)
            $chunks.Add([Convert]::ToHexString($sha.ComputeHash($buffer, 0, $count)).ToLowerInvariant())
        }
        $files += [ordered]@{
            path = $part.Name; size = $input.Length
            sha256 = [Convert]::ToHexString($full.GetHashAndReset()).ToLowerInvariant()
            chunks = $chunks.ToArray()
        }
    } finally { $input.Dispose(); $full.Dispose(); $sha.Dispose() }
    Write-Output ('Hashed ' + $part.Name + ': ' + $chunks.Count + ' chunks')
}
$manifest = [ordered]@{
    schema = 'modlist-manifest-chunks-v1'; archive_name = $ArchiveName; unpacked_size = $UnpackedBytes
    hash = [ordered]@{algorithm = 'sha256'; chunk_size = $buffer.Length}; files = $files
}
$bytes = [Text.Encoding]::UTF8.GetBytes(($manifest | ConvertTo-Json -Depth 8))
$output = [IO.File]::Open($manifestPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try { $output.Write($bytes, 0, $bytes.Length) } finally { $output.Dispose() }
Write-Output ('Manifest: ' + $manifestPath)
