# Sign Dock.exe + Hoverdock-Setup-*.exe with a local code-signing cert (MouseLabDev preferred).
param(
  [string]$BinDir = '',
  [string]$Thumbprint = ''
)
$ErrorActionPreference = 'Stop'
if (-not $BinDir) { $BinDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'out\bin\Release' }
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
  Where-Object FullName -match '\\x64\\signtool.exe$' | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw 'signtool.exe not found' }
if (-not $Thumbprint) {
  $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object Subject -match 'MouseLab' | Select-Object -First 1
  if (-not $cert) {
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Select-Object -First 1
  }
  if (-not $cert) { throw 'No code-signing certificate in CurrentUser\My' }
  $Thumbprint = $cert.Thumbprint
}
$targets = @()
$targets += Get-Item (Join-Path $BinDir 'Dock.exe')
$targets += Get-ChildItem (Join-Path (Split-Path $BinDir -Parent) '..\Hoverdock-Setup-*.exe') -EA SilentlyContinue
$targets += Get-ChildItem (Join-Path (Split-Path $BinDir -Parent) 'Hoverdock-Setup-*.exe') -EA SilentlyContinue
$root = Split-Path $PSScriptRoot -Parent
$targets += Get-ChildItem (Join-Path $root 'out\Hoverdock-Setup-*.exe') -EA SilentlyContinue
$targets = $targets | Where-Object { $_ } | Select-Object -Unique FullName
foreach ($t in $targets) {
  & $signtool.FullName sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com /sha1 $Thumbprint $t.FullName
  if ($LASTEXITCODE -ne 0) { throw "sign failed: $($t.FullName)" }
  Write-Host "Signed $($t.FullName)"
}
