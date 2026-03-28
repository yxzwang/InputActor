@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

cd /d "%~dp0"

echo [0/5] Checking running InputActor process...
tasklist /fi "imagename eq InputActor.exe" | find /i "InputActor.exe" >nul
if not errorlevel 1 (
  echo InputActor.exe is running. Please close it and run build_exe.bat again.
  pause
  exit /b 1
)

echo [1/5] Checking Python...
python --version >nul 2>&1
if errorlevel 1 (
  echo Python not found. Please install Python 3.10+ and add it to PATH.
  pause
  exit /b 1
)

echo [2/5] Installing dependencies...
python -m pip install --upgrade pip
if errorlevel 1 goto :error
python -m pip install -r requirements.txt pyinstaller
if errorlevel 1 goto :error

echo [3/5] Cleaning old build artifacts...
if exist "build" rmdir /s /q "build"
if exist "dist" rmdir /s /q "dist"
if exist "InputActor.spec" del /f /q "InputActor.spec"

echo [4/5] Building EXE...
python -m PyInstaller ^
  --noconfirm ^
  --clean ^
  --onefile ^
  --windowed ^
  --collect-all vgamepad ^
  --name "InputActor" ^
  main.py
if errorlevel 1 goto :error

echo [5/5] Build complete.
echo EXE path: %cd%\dist\InputActor.exe
echo.
echo You can now run InputActor.exe directly.
pause
exit /b 0

:error
echo Build failed.
pause
exit /b 1
