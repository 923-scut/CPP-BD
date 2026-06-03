@echo off
setlocal
cd /d "%~dp0"

if not exist "build\YingYuBoYi.exe" (
    mingw32-make
    if errorlevel 1 (
        pause
        exit /b 1
    )
)

start "" "%~dp0build\YingYuBoYi.exe"
