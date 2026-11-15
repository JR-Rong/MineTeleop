@echo off
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0run-high-performance.ps1" %*
if errorlevel 1 pause
