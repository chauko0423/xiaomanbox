@echo off
cd /d "%~dp0"
where node >nul 2>&1
if errorlevel 1 (
  echo Please install official Node.js 22 or newer, including npm.
  pause
  exit /b 1
)
node deploy.mjs doctor
if errorlevel 1 goto :end
node deploy.mjs install
:end
pause
