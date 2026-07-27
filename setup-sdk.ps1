# Automatic Ultralight SDK 1.4.0 Setup
# Run this once before building any PrismaUI_F4 runtime target.

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $scriptDir "build"
$sdkDir = Join-Path $buildDir "ultralight-1.4.0"
$archiveFile = Join-Path $scriptDir "external\ultralight-sdk-1.4.0-win-x64.7z"
$sevenZip = "C:\Program Files\7-Zip\7z.exe"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Ultralight SDK 1.4.0 Setup" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check if 7-Zip is available
if (-not (Test-Path $sevenZip)) {
    Write-Host "ERROR: 7-Zip not found at: $sevenZip" -ForegroundColor Red
    Write-Host "Install 7-Zip from: https://www.7-zip.org/" -ForegroundColor Yellow
    exit 1
}

# Check if archive exists
if (-not (Test-Path $archiveFile)) {
    Write-Host "ERROR: SDK archive not found at: $archiveFile" -ForegroundColor Red
    exit 1
}

# Check if SDK already extracted
if ((Test-Path "$sdkDir\include") -and (Test-Path "$sdkDir\lib") -and (Test-Path "$sdkDir\bin")) {
    Write-Host "✓ Ultralight SDK 1.4.0 already extracted" -ForegroundColor Green
    Write-Host "  Location: $sdkDir" -ForegroundColor Green
    exit 0
}

Write-Host "Extracting Ultralight SDK 1.4.0..." -ForegroundColor Yellow
Write-Host "  Archive: $archiveFile" -ForegroundColor Gray
Write-Host "  Target: $sdkDir" -ForegroundColor Gray
Write-Host ""

# Create target directory
New-Item -ItemType Directory -Path $sdkDir -Force | Out-Null

# Extract
& $sevenZip x $archiveFile -o"$sdkDir" -y | Out-Null

# Verify
if ((Test-Path "$sdkDir\include") -and (Test-Path "$sdkDir\lib") -and (Test-Path "$sdkDir\bin")) {
    Write-Host "✓ SDK extracted successfully" -ForegroundColor Green
    Write-Host ""
    Write-Host "SDK Structure:" -ForegroundColor Cyan
    Write-Host "  ✓ include/" -ForegroundColor Green
    Write-Host "  ✓ lib/" -ForegroundColor Green
    Write-Host "  ✓ bin/" -ForegroundColor Green
    Write-Host "  ✓ resources/" -ForegroundColor Green
    Write-Host ""
    Write-Host "Ready to build!" -ForegroundColor Green
    exit 0
} else {
    Write-Host "ERROR: SDK extraction failed or incomplete" -ForegroundColor Red
    Write-Host "Missing directories:" -ForegroundColor Red
    if (-not (Test-Path "$sdkDir\include")) { Write-Host "  - include/" -ForegroundColor Red }
    if (-not (Test-Path "$sdkDir\lib")) { Write-Host "  - lib/" -ForegroundColor Red }
    if (-not (Test-Path "$sdkDir\bin")) { Write-Host "  - bin/" -ForegroundColor Red }
    exit 1
}
