param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$WorkRoot,
    [Parameter(Mandatory)][string]$DestinationParent,
    [string]$Runner = (Join-Path $PSScriptRoot '..\build\windows\x64\release\installer_workflow_tests.exe')
)

$ErrorActionPreference = 'Stop'
$Package = [IO.Path]::GetFullPath($Package)
$Source = [IO.Path]::GetFullPath($Source)
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$DestinationParent = [IO.Path]::GetFullPath($DestinationParent)
$Runner = [IO.Path]::GetFullPath($Runner)
foreach ($path in @($WorkRoot, $DestinationParent)) {
    if (Test-Path -LiteralPath $path) { throw ('Use a fresh test folder: ' + $path) }
    foreach ($inputFolder in @($Package, $Source)) {
        if ($path -eq $inputFolder -or $path.StartsWith($inputFolder.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $inputFolder.StartsWith($path.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Test artifacts must not overlap source data or the original package.'
        }
    }
}
if (-not (Test-Path -LiteralPath $Runner -PathType Leaf)) { throw 'Build installer_workflow_tests first.' }
$manifestPath = Join-Path $Package 'manifest.json'
$model = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($model.schema -ne 'modlist-manifest-chunks-v1' -or -not $model.archive_name) { throw 'Use a flat packer-generated chunk manifest.' }
$inputs = @($manifestPath) + @($model.files | ForEach-Object { Join-Path $Package $_.path })
$before = @{}
foreach ($inputPath in $inputs) { $before[$inputPath] = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash }
[IO.Directory]::CreateDirectory($WorkRoot) | Out-Null
[IO.Directory]::CreateDirectory($DestinationParent) | Out-Null
$targets = @()
foreach ($route in @('direct', 'staged')) {
    $copy = Join-Path $WorkRoot ('package-' + $route)
    [IO.Directory]::CreateDirectory($copy) | Out-Null
    [IO.File]::Copy($manifestPath, (Join-Path $copy 'manifest.json'), $false)
    foreach ($entry in $model.files) {
        $target = [IO.Path]::GetFullPath((Join-Path $copy $entry.path))
        if (-not $target.StartsWith($copy + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture path.' }
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
        [IO.File]::Copy((Join-Path $Package $entry.path), $target, $false)
    }
    $testExe = Join-Path $copy 'installer_workflow_tests.exe'
    [IO.File]::Copy($Runner, $testExe, $false)
    $output = Join-Path $DestinationParent $route
    $stdout = Join-Path $WorkRoot ($route + '.txt')
    $stderr = Join-Path $WorkRoot ($route + '-errors.txt')
    $process = Start-Process -FilePath $testExe -ArgumentList ('"' + $output + '" ' + $route + ' success') -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if ($process.ExitCode -ne 0) { throw ('Production worker failed: ' + $route + '; see ' + $stderr) }
    $remaining = @(Get-ChildItem -LiteralPath $output -Force)
    if ($remaining.Count -ne 1 -or $remaining[0].Name -ne 'installed') { throw ('Staging or metadata was left behind: ' + $output) }
    $targets += Join-Path $output ('installed\' + $model.archive_name)
    Write-Output ('PASS production worker: ' + $route)
}
& (Join-Path $PSScriptRoot 'verify-smoke.ps1') -Source $Source -Destinations $targets -Report (Join-Path $WorkRoot 'content-verification.json')
if (-not $?) { throw 'Installed payload content differs from the original source.' }
foreach ($inputPath in $inputs) {
    if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -ne $before[$inputPath]) { throw ('Original package input changed: ' + $inputPath) }
}
Write-Output ('PASS original package unchanged; reports: ' + $WorkRoot)
