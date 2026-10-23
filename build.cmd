@echo off
rem Double-click wrapper for build.ps1 (README.md, "Build and play").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set rc=%errorlevel%
if "%~1"=="" pause
exit /b %rc%
