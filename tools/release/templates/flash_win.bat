@echo off
REM SPDX-License-Identifier: BSD-3-Clause
REM Copyright (c) 2026, Ambiq
setlocal
cd /d "%~dp0"

where JLink.exe >nul 2>&1
if errorlevel 1 (
  if exist "%ProgramFiles%\SEGGER\JLink\JLink.exe" (
    set "JLINK=%ProgramFiles%\SEGGER\JLink\JLink.exe"
  ) else (
    echo SEGGER J-Link was not found. Install it, then run this helper again.
    pause
    exit /b 1
  )
) else (
  set "JLINK=JLink.exe"
)

"%JLINK%" -nogui 1 -device @JLINK_DEVICE@ -if SWD -speed @SWD_SPEED@ -commandfile downloadfw.jlink
set "status=%ERRORLEVEL%"
if "%status%"=="0" (echo Flash completed successfully.) else (echo Flash failed with exit code %status%.)
pause
exit /b %status%
