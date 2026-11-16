param(
    [Parameter(Mandatory = $true)][string[]]$Binary,
    [string]$Dumpbin,
    [string]$Report
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Runtime dependency inspection requires Windows.' }

if (-not $Dumpbin) {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) {
        $Dumpbin = $command.Source
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Cannot find dumpbin or vswhere.' }
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Cannot find the Visual C++ build tools.' }
        $toolset = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Directory |
            Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
        if (-not $toolset) { throw 'Cannot find the Visual C++ toolset.' }
        $Dumpbin = Join-Path $toolset.FullName 'bin\Hostx64\x64\dumpbin.exe'
    }
}
if (-not (Test-Path -LiteralPath $Dumpbin -PathType Leaf)) { throw "dumpbin not found: $Dumpbin" }

$results = @()
$failures = @()
foreach ($file in $Binary) {
    $path = (Get-Item -LiteralPath $file -ErrorAction Stop).FullName
    $output = & $Dumpbin /nologo /dependents $path 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect $path`: $($output -join [Environment]::NewLine)" }
    $dependencies = @($output | ForEach-Object {
        if ($_ -match '^\s+([^\s]+\.dll)\s*$') { $Matches[1] }
    } | Sort-Object -Unique)
    if (-not $dependencies.Count) { throw "No DLL imports parsed for $path; inspection is inconclusive." }
    $runtime = @($dependencies | Where-Object {
        $_ -match '^(msvcp\d.*|msvcr\d.*|vcruntime\d.*|concrt\d.*|vcomp\d.*|ucrtbased?|api-ms-win-crt-.*)\.dll$'
    })
    $results += [pscustomobject]@{
        binary = $path
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        dependencies = $dependencies
        runtime_imports = $runtime
    }
    if ($runtime.Count) {
        $failures += "$path imports $($runtime -join ', ')"
    } else {
        Write-Host "PASS static runtime: $path"
    }
}
if ($Report) {
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Report -Encoding UTF8
}
if ($failures.Count) { throw "Portable runtime check failed:`n$($failures -join [Environment]::NewLine)" }
