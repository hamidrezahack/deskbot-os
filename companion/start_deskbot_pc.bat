@echo off
rem ---- DeskBot PC helper launcher ----
rem Keep this file in the SAME folder as deskbot_pc.py.
rem Change the token below to the same value as PC_TOKEN in secrets.h
set DESKBOT_TOKEN=deskbot1234

cd /d "%~dp0"
title DeskBot PC helper (close this window to stop it)
where python >/dev/null 2>nul
if %errorlevel%==0 (
  python deskbot_pc.py
) else (
  py deskbot_pc.py
)
echo.
echo The helper stopped. Press any key to close.
pause >nul
