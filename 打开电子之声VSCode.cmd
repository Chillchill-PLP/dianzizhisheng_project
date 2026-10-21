@echo off
"C:\Users\Administrator\AppData\Local\Microsoft\WindowsApps\pwsh.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Open-Embedded.ps1"
if errorlevel 1 pause
