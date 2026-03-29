# InputActor C++ Version

This directory contains a C++ rewrite of InputActor.
The original Python implementation remains unchanged under `input_actor/` for feature comparison.

## Implemented Features

- JSONL replay loader (`session_header` timing + event stream)
- Keyboard replay (`key_down` / `key_up`)
- Mouse replay (move, buttons, wheel/hwheel)
- Gamepad replay (`gamepad_connected`, `gamepad_button_down/up`, `gamepad_axis`) through ViGEmClient
- Desktop GUI (Win32)
- Start/Pause/Stop controls
- Start delay countdown
- Runtime progress + log
- Global hotkeys:
  - `Ctrl+Alt+F6` Start
  - `Ctrl+Alt+F7` Pause/Resume
  - `Ctrl+Alt+F8` Stop

## Build

From repository root:

```powershell
.\build_cpp.bat
```

Or manually:

```powershell
cmake -S cpp_actor -B build-cpp -G "Visual Studio 17 2022" -A x64
cmake --build build-cpp --config Release
```

Output executable:

```text
build-cpp\Release\InputActorCpp.exe
```

`build_cpp.bat` embeds both `ViGEmClient.dll` and `ViGEmBusSetup.exe` into `InputActorCpp.exe`.
It also copies `ViGEmClient.dll` beside the EXE as an optional fallback.

## Notes

- Gamepad replay requires ViGEmBus and `ViGEmClient.dll`.
- If missing, the app attempts elevation and runs bundled `vendor\ViGEmBusSetup.exe` silently.
- Preferred distribution is single file: `InputActorCpp.exe`.
- Keeping `ViGEmClient.dll` beside EXE is optional fallback.
