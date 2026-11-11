@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-system.ps1"
echo.
pause
