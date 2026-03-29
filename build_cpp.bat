@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

cd /d "%~dp0"

echo [1/5] Preparing ViGEmBus installer...
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\fetch_vigembus.ps1" -OutputPath "vendor\ViGEmBusSetup.exe"
if errorlevel 1 goto :error

set "VIGEMCLIENT_SRC="
if exist ".venv\Lib\site-packages\vgamepad\win\vigem\client\x64\ViGEmClient.dll" (
  set "VIGEMCLIENT_SRC=.venv\Lib\site-packages\vgamepad\win\vigem\client\x64\ViGEmClient.dll"
) else if exist "vendor\ViGEmClient.dll" (
  set "VIGEMCLIENT_SRC=vendor\ViGEmClient.dll"
)

if not defined VIGEMCLIENT_SRC (
  echo ViGEmClient.dll source not found.
  echo Expected .venv\Lib\site-packages\vgamepad\win\vigem\client\x64\ViGEmClient.dll
  echo Or place a copy at vendor\ViGEmClient.dll
  goto :error
)

echo [2/5] Configuring CMake project...
cmake -S cpp_actor -B build-cpp -G "Visual Studio 17 2022" -A x64 ^
  -DVIGEMCLIENT_DLL_PATH="%cd%\%VIGEMCLIENT_SRC%" ^
  -DVIGEMBUS_SETUP_PATH="%cd%\vendor\ViGEmBusSetup.exe"
if errorlevel 1 goto :error

echo [3/5] Building Release...
cmake --build build-cpp --config Release
if errorlevel 1 goto :error

if defined VIGEMCLIENT_SRC (
  echo [4/5] Copying ViGEmClient.dll ^(optional runtime fallback^)...
  copy /y "%VIGEMCLIENT_SRC%" "build-cpp\Release\ViGEmClient.dll" >nul
  if errorlevel 1 goto :error
)

echo [5/5] Done.
echo EXE path: %cd%\build-cpp\Release\InputActorCpp.exe
if exist "build-cpp\Release\ViGEmClient.dll" (
  echo DLL path: %cd%\build-cpp\Release\ViGEmClient.dll
)
pause
exit /b 0

:error
echo Build failed.
pause
exit /b 1
