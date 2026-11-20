param(
    [Parameter(Mandatory)][string]$Package
)

$ErrorActionPreference = 'Stop'
$folder = Get-Item -LiteralPath $Package -Force
if (-not $folder.PSIsContainer -or ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Choose the ordinary package folder made by the packer.'
}
$manifest = Get-Item -LiteralPath (Join-Path $folder.FullName 'manifest.json') -Force
$deletions = Get-Item -LiteralPath (Join-Path $folder.FullName 'for delete.txt') -Force
foreach ($file in @($manifest, $deletions)) {
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Expected an ordinary package file: $($file.Name)"
    }
}
if ($deletions.Length -gt 4MB) { throw 'for delete.txt exceeds 4 MiB.' }
$data = [IO.File]::ReadAllBytes($deletions.FullName)
if ($data.Length -gt 4MB) { throw 'for delete.txt exceeds 4 MiB.' }
$encoding = [Text.UTF8Encoding]::new($false, $true)
$null = $encoding.GetString($data)
$model = Get-Content -LiteralPath $manifest.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
if ($model.PSObject.Properties['delete_paths']) {
    throw 'This package already embeds delete_paths. Edit the list in the packer and rebuild it.'
}
$chunkSize = [long]$model.hash.chunk_size
if ($model.schema -ne 'modlist-manifest-chunks-v1' -or
    $model.hash.algorithm -ne 'sha256' -or $chunkSize -le 0 -or
    -not @($model.files | Where-Object { $_.path -match '\.(7z|zip)(\.\d{3})?$' }).Count) {
    throw 'This is not a current packer archive manifest.'
}
$chunks = [Collections.Generic.List[string]]::new()
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $whole = [BitConverter]::ToString($sha.ComputeHash($data)).Replace('-', '').ToLowerInvariant()
    for ($offset = 0; $offset -lt $data.Length; $offset += $count) {
        $count = [int][Math]::Min($chunkSize, $data.Length - $offset)
        $hash = [BitConverter]::ToString($sha.ComputeHash($data, $offset, $count))
        $chunks.Add($hash.Replace('-', '').ToLowerInvariant())
    }
} finally {
    $sha.Dispose()
}
$model.files = @($model.files | Where-Object { $_.path -ine 'for delete.txt' }) + @([ordered]@{
    path = 'for delete.txt'
    size = $data.Length
    sha256 = $whole
    chunks = $chunks.ToArray()
})
$temp = Join-Path $folder.FullName ('.patch-manifest-' + [guid]::NewGuid().ToString('N') + '.tmp')
$stream = [IO.File]::Open($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    try {
        $json = $encoding.GetBytes(($model | ConvertTo-Json -Depth 12))
        $stream.Write($json, 0, $json.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    [IO.File]::Replace($temp, $manifest.FullName, [NullString]::Value)
} finally {
    if (Test-Path -LiteralPath $temp -PathType Leaf) { [IO.File]::Delete($temp) }
}
Write-Output "Bound for delete.txt to $($manifest.FullName)"
