@echo off
rem Removes the EDDS plugin from XnView MP. Run as administrator, with XnView MP closed.
setlocal

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)

set "XNVIEW=%ProgramFiles%\XnViewMP\Plugins"
if exist "%XNVIEW%\Xedds.usr" (
  del /f "%XNVIEW%\Xedds.usr" && echo XnView MP plugin removed.
)
