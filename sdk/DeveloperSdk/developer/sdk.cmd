@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0internal\RunSdk.ps1" %*
exit /b %errorlevel%
