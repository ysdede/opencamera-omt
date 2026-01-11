#!/usr/bin/env pwsh
# logcat.ps1 - View app logs in real-time
# Usage: .\logcat.ps1

$adb = "adb"

Write-Host "=== Open Camera - Logcat ===" -ForegroundColor Cyan
Write-Host "Press Ctrl+C to stop" -ForegroundColor Yellow
Write-Host ""

# Clear existing logs and start fresh
& $adb logcat -c

# Show logs from our app and OMT components (including native)
& $adb logcat -v time `
    "MainActivity:V" `
    "MainUI:V" `
    "Preview:V" `
    "OMTStreamingManager:V" `
    "OMTSender:V" `
    "OMTBridge:V" `
    "libomt-android:V" `
    "CameraController:V" `
    "CameraController2:V" `
    "NsdManager:V" `
    "AndroidRuntime:E" `
    "*:S"
