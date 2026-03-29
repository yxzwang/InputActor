@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

cd /d "%~dp0"

echo [1/3] Configuring CMake project...
cmake -S cpp_actor -B build-cpp -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :error

echo [2/3] Building Release...
cmake --build build-cpp --config Release
if errorlevel 1 goto :error

set "VIGEMCLIENT_SRC="
if exist ".venv\Lib\site-packages\vgamepad\win\vigem\client\x64\ViGEmClient.dll" (
  set "VIGEMCLIENT_SRC=.venv\Lib\site-packages\vgamepad\win\vigem\client\x64\ViGEmClient.dll"
) else if exist "vendor\ViGEmClient.dll" (
  set "VIGEMCLIENT_SRC=vendor\ViGEmClient.dll"
)

if defined VIGEMCLIENT_SRC (
  echo [3/4] Copying ViGEmClient.dll...
  copy /y "%VIGEMCLIENT_SRC%" "build-cpp\Release\ViGEmClient.dll" >nul
  if errorlevel 1 goto :error
) else (
  echo [3/4] Warning: ViGEmClient.dll source not found, EXE may fail to start gamepad replay.
)

echo [4/4] Done.
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
