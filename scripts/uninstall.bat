@echo off
rem Removes the EDDS WIC codec. Run as administrator.
setlocal

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\EddsViewer"

if exist "%APPDIR%\EddsWic.dll" (
  regsvr32 /s /u "%APPDIR%\EddsWic.dll"
  rem Explorer may still hold the DLL; it can then be deleted after a restart.
  del /f "%APPDIR%\EddsWic.dll" 2>nul
  if exist "%APPDIR%\EddsWic.dll" echo EddsWic.dll is in use, delete "%APPDIR%" after a restart.
  rmdir "%APPDIR%" 2>nul
  echo Windows codec removed.
)
