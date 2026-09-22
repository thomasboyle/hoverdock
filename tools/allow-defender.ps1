#Requires -Version 5.1
# Adds Windows Defender exclusions for Hoverdock so ML false positives stop deleting Dock.exe.
# Must run elevated (UAC). Safe to re-run.
$ErrorActionPreference = "Stop"
$install = Join-Path $env:LOCALAPPDATA "Programs\Hoverdock"
$exe = Join-Path $install "Dock.exe"
if (-not (Test-Path $install)) { New-Item -ItemType Directory -Force -Path $install | Out-Null }
Add-MpPreference -ExclusionPath $install
if (Test-Path $exe) { Add-MpPreference -ExclusionProcess $exe }
# Also cover the build output tree if present
$repoOut = "D:\C++\hoverdock\out"
if (Test-Path $repoOut) { Add-MpPreference -ExclusionPath $repoOut }
Write-Host "OK: Defender exclusions set for:"
Write-Host "  $install"
if (Test-Path $exe) { Write-Host "  process: $exe" }
if (Test-Path $repoOut) { Write-Host "  $repoOut" }
Remove-MpThreat -ErrorAction SilentlyContinue | Out-Null
