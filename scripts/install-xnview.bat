@echo off
rem Installs Xedds.usr, the EDDS read plugin for XnView MP.
rem Run as administrator from the release folder (Xedds.usr next to this file), with XnView MP closed.
setlocal
cd /d "%~dp0"

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)
if not exist Xedds.usr (
  echo Xedds.usr not found next to install-xnview.bat.
  exit /b 1
)

set "XNVIEW=%ProgramFiles%\XnViewMP\Plugins"
if not exist "%XNVIEW%" (
  echo XnView MP not found in "%ProgramFiles%\XnViewMP".
  exit /b 1
)
copy /y Xedds.usr "%XNVIEW%\Xedds.usr" >nul || (
  echo Could not copy Xedds.usr: close XnView MP and try again.
  exit /b 1
)
echo XnView MP plugin installed: %XNVIEW%\Xedds.usr
echo If XnView MP cannot load it, Microsoft Defender attack surface reduction rules may block it:
echo   Add-MpPreference -AttackSurfaceReductionOnlyExclusions "%XNVIEW%\Xedds.usr"
