$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Version = "1.0.4022.49"
$SdkRoot = Join-Path $ProjectRoot "third_party\webview2"
$PackageRoot = Join-Path $SdkRoot "Microsoft.Web.WebView2"
$Marker = Join-Path $PackageRoot "build\native\include\WebView2.h"

if (Test-Path $Marker) {
  Write-Host "WebView2 SDK already restored at $PackageRoot"
  return
}

New-Item -ItemType Directory -Path $SdkRoot -Force | Out-Null
$Package = Join-Path $SdkRoot "Microsoft.Web.WebView2.$Version.nupkg"
$Url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$Version"

Write-Host "Downloading Microsoft.Web.WebView2 $Version..."
Invoke-WebRequest -Uri $Url -OutFile $Package

$ExtractRoot = Join-Path $SdkRoot "package"
if (Test-Path $ExtractRoot) {
  Remove-Item $ExtractRoot -Recurse -Force
}
Expand-Archive -LiteralPath $Package -DestinationPath $ExtractRoot -Force

if (Test-Path $PackageRoot) {
  Remove-Item $PackageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $PackageRoot -Force | Out-Null
Move-Item (Join-Path $ExtractRoot "build") (Join-Path $PackageRoot "build")

Remove-Item $ExtractRoot -Recurse -Force

if (-not (Test-Path $Marker)) {
  throw "WebView2 SDK restore failed; WebView2.h was not found."
}

Write-Host "Restored WebView2 SDK to $PackageRoot"
