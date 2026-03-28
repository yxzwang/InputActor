@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

cd /d "%~dp0"
set "PYTHON_CMD=python"
set "USE_UV_PIP="

if exist ".venv\Scripts\python.exe" (
  set "PYTHON_CMD=.venv\Scripts\python.exe"
)

echo [0/7] Checking running InputActor process...
tasklist /fi "imagename eq InputActor.exe" | find /i "InputActor.exe" >nul
if not errorlevel 1 (
  echo InputActor.exe is running. Please close it and run build_exe.bat again.
  pause
  exit /b 1
)

echo [1/7] Locating Python...
if /I "%PYTHON_CMD%"=="python" (
  python --version >nul 2>&1
  if errorlevel 1 (
    echo Python not found. Install Python 3.10+ or create .venv\Scripts\python.exe.
    pause
    exit /b 1
  )
)
echo Using Python command: %PYTHON_CMD%

echo [2/7] Checking pip...
"%PYTHON_CMD%" -m pip --version >nul 2>&1
if errorlevel 1 (
  where uv >nul 2>&1
  if errorlevel 1 (
    echo pip is unavailable for %PYTHON_CMD% and 'uv' is not installed.
    echo Install pip, or install uv and try again.
    pause
    exit /b 1
  )
  set "USE_UV_PIP=1"
  echo pip not available, will use uv pip.
)

echo [3/7] Installing dependencies...
if defined USE_UV_PIP (
  uv pip install --python "%PYTHON_CMD%" -r requirements.txt pyinstaller
  if errorlevel 1 goto :error
) else (
  "%PYTHON_CMD%" -m pip install --upgrade pip
  if errorlevel 1 goto :error
  "%PYTHON_CMD%" -m pip install -r requirements.txt pyinstaller
  if errorlevel 1 goto :error
)

echo [4/7] Fetching ViGEmBus installer...
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\fetch_vigembus.ps1" -OutputPath "vendor\ViGEmBusSetup.exe"
if errorlevel 1 goto :error

echo [5/7] Cleaning old build artifacts...
if exist "build" rmdir /s /q "build"
if exist "dist" rmdir /s /q "dist"
if exist "InputActor.spec" del /f /q "InputActor.spec"

echo [6/7] Building EXE...
"%PYTHON_CMD%" -m PyInstaller ^
  --noconfirm ^
  --clean ^
  --onefile ^
  --windowed ^
  --collect-all vgamepad ^
  --add-data "vendor\ViGEmBusSetup.exe;vendor" ^
  --name "InputActor" ^
  main.py
if errorlevel 1 goto :error

echo [7/7] Build complete.
echo EXE path: %cd%\dist\InputActor.exe
echo.
echo You can now run InputActor.exe directly.
pause
exit /b 0

:error
echo Build failed.
pause
exit /b 1
