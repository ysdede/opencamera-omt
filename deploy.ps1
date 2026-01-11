#!/usr/bin/env pwsh
# deploy.ps1 - Quick build and deploy to connected device
# Usage: .\deploy.ps1 [debug|release] [--clean] [--launch]
#   --clean : Uninstall first (clears all app data including preferences)

param(
    [string]$BuildType = "debug",
    [switch]$Launch = $true,
    [switch]$Clean = $false
)

$ErrorActionPreference = "Continue" # Don't stop on stderr warnings (like deprecation notes)
$adb = "adb"

Write-Host "=== Open Camera - Fast Deploy ===" -ForegroundColor Cyan
Write-Host "Build Type: $BuildType" -ForegroundColor Yellow

# Check device
$devices = & $adb devices | Select-String "device$"
if (-not $devices) {
    Write-Host "ERROR: No device connected!" -ForegroundColor Red
    exit 1
}
Write-Host "Device: Connected" -ForegroundColor Green

# Check if app is already installed
$packageName = "net.sourceforge.opencamera"
$installed = & $adb shell pm list packages | Select-String $packageName

if ($installed) {
    if ($Clean) {
        Write-Host "`nClean install requested - uninstalling existing app..." -ForegroundColor Yellow
        & $adb uninstall $packageName | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Old app uninstalled (preferences cleared)" -ForegroundColor Green
        } else {
            Write-Host "Warning: Could not uninstall old app" -ForegroundColor Yellow
        }
    } else {
        Write-Host "`nExisting app detected - will upgrade (preserving preferences)" -ForegroundColor Green
        # NOTE: gradle installDebug will upgrade in place, preserving SharedPreferences
    }
}

# Build and install
$task = if ($BuildType -eq "release") { "installRelease" } else { "installDebug" }
Write-Host "`nBuilding and installing..." -ForegroundColor Cyan

$startTime = Get-Date

# Use gradlew.bat for Windows
if (Test-Path ".\gradlew.bat") {
    $gradle = ".\gradlew.bat"
} else {
    $gradle = "./gradlew"
}

& $gradle $task --quiet

$elapsed = (Get-Date) - $startTime

if ($LASTEXITCODE -eq 0) {
    Write-Host "Installed successfully in $([math]::Round($elapsed.TotalSeconds, 1))s" -ForegroundColor Green
    
    if ($Launch) {
        Write-Host "`nLaunching app..." -ForegroundColor Cyan
        & $adb shell am start -n "net.sourceforge.opencamera/net.sourceforge.opencamera.MainActivity"
        Write-Host "App launched!" -ForegroundColor Green
    }
} else {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host "`n=== Done ===" -ForegroundColor Cyan
