@echo off
start "" /wait "%~dp0VideoWebcam.exe" --register
if errorlevel 1 (
  echo Camera registration failed.
  pause
  exit /b 1
)
echo VideoWebcam installed for the current Windows user. Restart your camera app.
pause
