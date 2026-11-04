@echo off
rem HS_Coop one-click installer: double-click this file. It finds Heat Signature through Steam and installs the mod.
setlocal
set "PS1=%~dp0files\install.ps1"
if not exist "%PS1%" set "PS1=%~dp0install.ps1"
if not exist "%PS1%" (
  echo The installer's files are missing.
  echo Unzip the whole zip file first, then run Install.bat from the unzipped folder.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
