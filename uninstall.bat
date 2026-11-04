@echo off
rem Removes the HS_Coop co-op mod from Heat Signature again: double-click this file.
setlocal
set "PS1=%~dp0files\install.ps1"
if not exist "%PS1%" set "PS1=%~dp0install.ps1"
if not exist "%PS1%" (
  echo The installer's files are missing.
  echo Unzip the whole zip file first, then run Uninstall.bat from the unzipped folder.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Uninstall %*
