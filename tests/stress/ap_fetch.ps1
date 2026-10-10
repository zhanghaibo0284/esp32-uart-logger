# Quick AP fetch: connect to recorder SoftAP, download all files in the
# manifest JSON ([{url, out}]), then reconnect to the home SSID. Minimises the
# window without internet (no model calls should run while this executes).
#
# Usage: powershell -ExecutionPolicy Bypass -File ap_fetch.ps1 `
#        -Manifest fetch.json -HomeSsid qdmetro
param(
    [Parameter(Mandatory = $true)] [string] $Manifest,
    [string] $HomeSsid = "qdmetro",
    [string] $ApSsid = "UART-LOG"
)

$ErrorActionPreference = "Stop"
$entries = Get-Content -Raw -Encoding UTF8 $Manifest | ConvertFrom-Json

Write-Host "[1/4] connecting AP $ApSsid ..."
netsh wlan connect name=$ApSsid ssid=$ApSsid | Out-Null

$ok = $false
for ($i = 0; $i -lt 24; $i++) {
    Start-Sleep -Milliseconds 800
    $out = ping -n 1 -w 600 192.168.4.1
    if ($out -match "TTL=64") { $ok = $true; break }
}
if (-not $ok) {
    Write-Warning "AP link not confirmed; reconnecting home SSID"
    netsh wlan connect name=$HomeSsid ssid=$HomeSsid | Out-Null
    exit 2
}
Write-Host "[2/4] AP link ready (TTL=64)"

Write-Host "[3/4] downloading $($entries.Count) file(s) ..."
foreach ($e in $entries) {
    $dir = Split-Path $e.out -Parent
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    # curl.exe: URL kept quoted to protect & and ? from PowerShell parsing.
    & curl.exe -sS --fail --max-time 60 --output $e.out $e.url
    if ($LASTEXITCODE -ne 0) { Write-Warning "download failed: $($e.url)" }
    else { Write-Host "  got $($e.out) ($((Get-Item $e.out).Length) bytes)" }
}

Write-Host "[4/4] reconnecting home SSID $HomeSsid ..."
netsh wlan connect name=$HomeSsid ssid=$HomeSsid | Out-Null
Start-Sleep -Seconds 3
Write-Host "done; home network reconnecting."
