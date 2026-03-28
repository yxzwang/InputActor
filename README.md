# InputActor

InputActor replays keyboard, mouse, and gamepad input sequences on Windows from a JSONL file.

## Features

- Replay events with timing preserved from `t_qpc` and `qpc_freq`.
- Supports keyboard `key_down` / `key_up` events.
- Supports mouse movement and common mouse button/wheel events.
- Supports gamepad events (`gamepad_connected`, `gamepad_button_down/up`, `gamepad_axis`) via a virtual Xbox 360 controller.
- GUI for:
  - selecting an input sequence file
  - loading and starting playback
  - visual progress display (event count + progress bar + current event type)
  - configurable start delay in seconds (default: 3s)
- Three customizable global hotkeys:
  - Start
  - Pause/Resume
  - Stop

## Input Format

The app expects a JSONL stream similar to:

```json
{"type":"session_header","qpc_freq":10000000,"screen":{"virtual_left":0,"virtual_top":0,"virtual_width":1920,"virtual_height":1080}}
{"type":"mouse_move","t_qpc":1234567890,"dx":12,"dy":-4}
{"type":"key_down","t_qpc":1234567990,"vk":65,"scan":30,"is_extended":false}
{"type":"key_up","t_qpc":1234568090,"vk":65,"scan":30,"is_extended":false}
{"type":"gamepad_button_down","t_qpc":1234568190,"gamepad_index":0,"control":"a"}
{"type":"gamepad_axis","t_qpc":1234568290,"gamepad_index":0,"control":"left_stick_x","value":12000}
{"type":"stats","t_qpc":1234568990}
```

`session_header` and `stats` are metadata and are not replayed.
For `mouse_move`, replay uses only `dx` and `dy` (relative movement).

## Example Input

`example/input.jsonl` is a keyboard/mouse example input sequence.
`example/input-gamepad.jsonl` is an example sequence that also includes gamepad operations.

## Requirements

- Windows 10/11
- Python 3.10+
- For gamepad replay: ViGEmBus driver installed (required by `vgamepad`)

Install dependencies:

```powershell
python -m pip install -r requirements.txt
```

## Run

```powershell
python main.py
```

## One-Click EXE Build

Double-click `build_exe.bat` in the project root, or run:

```powershell
.\build_exe.bat
```

After success, the executable is:

```text
dist\InputActor.exe
```

If `InputActor.exe` is running, close it before building.

## Notes

- Global hotkeys use Windows native `RegisterHotKey`.
- Avoid using control hotkeys that may appear inside your recorded sequence.
- Gamepad replay uses a virtual Xbox 360 gamepad through `vgamepad`.
