@echo off
start "" /wait "%~dp0VideoWebcam.exe" --unregister
if errorlevel 1 (
  echo Camera removal failed. Close camera apps and try again.
  pause
  exit /b 1
)
echo VideoWebcam has been removed. You may now delete this folder.
pause
