@echo off
rem Installs the EDDS WIC codec (Windows Explorer thumbnails for .edds files).
rem Run as administrator from the release folder (EddsWic.dll next to this file).
setlocal
cd /d "%~dp0"

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)
if not exist EddsWic.dll (
  echo EddsWic.dll not found next to install.bat. Build the project first, then run dist\install.bat.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\EddsViewer"

if not exist "%APPDIR%" mkdir "%APPDIR%"
if exist "%APPDIR%\EddsWic.dll" regsvr32 /s /u "%APPDIR%\EddsWic.dll"
copy /y EddsWic.dll "%APPDIR%\EddsWic.dll" >nul || (
  echo Could not replace "%APPDIR%\EddsWic.dll": it is probably in use by Explorer.
  echo Run uninstall.bat, sign out or restart, then run install.bat again.
  exit /b 1
)
regsvr32 /s "%APPDIR%\EddsWic.dll" || (
  echo Codec registration failed. If Microsoft Defender attack surface reduction rules are
  echo enforced on this PC, allow the DLL first ^(admin PowerShell^):
  echo   Add-MpPreference -AttackSurfaceReductionOnlyExclusions "%APPDIR%\EddsWic.dll"
  exit /b 1
)
echo Windows codec installed: %APPDIR%\EddsWic.dll
echo.
echo Done. If .edds files still show blank icons, run clear-thumbnails.bat.
