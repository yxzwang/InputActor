from __future__ import annotations

import ctypes
from ctypes import wintypes
from typing import Any, Dict, Iterable

from .gamepad import GamepadManager
from .models import SessionMeta

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_XDOWN = 0x0080
MOUSEEVENTF_XUP = 0x0100
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_HWHEEL = 0x1000
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000

XBUTTON1 = 0x0001
XBUTTON2 = 0x0002

SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79


ULONG_PTR = wintypes.WPARAM


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", wintypes.DWORD),
        ("wParamL", wintypes.WORD),
        ("wParamH", wintypes.WORD),
    ]


class _INPUTUNION(ctypes.Union):
    _fields_ = [
        ("mi", MOUSEINPUT),
        ("ki", KEYBDINPUT),
        ("hi", HARDWAREINPUT),
    ]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]


user32 = ctypes.WinDLL("user32", use_last_error=True)
send_input = user32.SendInput
send_input.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
send_input.restype = wintypes.UINT

get_system_metrics = user32.GetSystemMetrics
get_system_metrics.argtypes = (ctypes.c_int,)
get_system_metrics.restype = ctypes.c_int


class WindowsInputSender:
    def __init__(self) -> None:
        self._meta = SessionMeta()
        self._gamepad = GamepadManager()
        self._set_meta_from_system()

    def _set_meta_from_system(self) -> None:
        self._meta.screen.virtual_left = int(get_system_metrics(SM_XVIRTUALSCREEN))
        self._meta.screen.virtual_top = int(get_system_metrics(SM_YVIRTUALSCREEN))
        self._meta.screen.virtual_width = max(
            int(get_system_metrics(SM_CXVIRTUALSCREEN)), 1
        )
        self._meta.screen.virtual_height = max(
            int(get_system_metrics(SM_CYVIRTUALSCREEN)), 1
        )

    def configure(self, meta: SessionMeta) -> None:
        self._meta = meta
        if self._meta.screen.virtual_width <= 0 or self._meta.screen.virtual_height <= 0:
            self._set_meta_from_system()

    def send_event(self, event: Dict[str, Any]) -> bool:
        event_type = str(event.get("type", "")).lower()
        if event_type.startswith("gamepad_"):
            return self._gamepad.send_event(event)
        if event_type == "mouse_move":
            self._send_mouse_move(
                int(event.get("dx", 0) or 0),
                int(event.get("dy", 0) or 0),
            )
            return True
        if event_type in {"key_down", "key_up"}:
            self._send_key(event, key_up=(event_type == "key_up"))
            return True
        if event_type in {
            "mouse_left_down",
            "mouse_right_down",
            "mouse_middle_down",
            "mouse_x1_down",
            "mouse_x2_down",
        }:
            self._send_mouse_button(self._button_from_type(event_type), True)
            return True
        if event_type in {
            "mouse_left_up",
            "mouse_right_up",
            "mouse_middle_up",
            "mouse_x1_up",
            "mouse_x2_up",
        }:
            self._send_mouse_button(self._button_from_type(event_type), False)
            return True
        if event_type in {"mouse_down", "mouse_up"}:
            button = str(event.get("button", "left")).lower()
            self._send_mouse_button(button, event_type == "mouse_down")
            return True
        if event_type in {"mouse_wheel", "mouse_hwheel"}:
            self._send_mouse_wheel(event, horizontal=(event_type == "mouse_hwheel"))
            return True
        return False

    def close(self) -> None:
        self._gamepad.close()

    def prepare(self, events: Iterable[Dict[str, Any]]) -> None:
        for event in events:
            event_type = str(event.get("type", "")).lower()
            if event_type.startswith("gamepad_"):
                self._gamepad.ensure_ready()
                break

    @staticmethod
    def _button_from_type(event_type: str) -> str:
        if "right" in event_type:
            return "right"
        if "middle" in event_type:
            return "middle"
        if "x1" in event_type:
            return "x1"
        if "x2" in event_type:
            return "x2"
        return "left"

    @staticmethod
    def _send_input(payload: INPUT) -> None:
        sent = send_input(1, ctypes.byref(payload), ctypes.sizeof(INPUT))
        if sent != 1:
            raise ctypes.WinError(ctypes.get_last_error())

    def _send_mouse_move(self, dx: int, dy: int) -> None:
        if dx == 0 and dy == 0:
            return
        payload = INPUT(
            type=INPUT_MOUSE,
            mi=MOUSEINPUT(
                dx=dx,
                dy=dy,
                mouseData=0,
                dwFlags=MOUSEEVENTF_MOVE,
                time=0,
                dwExtraInfo=0,
            ),
        )
        self._send_input(payload)

    def _send_key(self, event: Dict[str, Any], key_up: bool) -> None:
        vk = int(event.get("vk", 0) or 0)
        scan = int(event.get("scan", 0) or 0)
        is_extended = bool(event.get("is_extended", False))

        flags = 0
        if key_up:
            flags |= KEYEVENTF_KEYUP
        if is_extended:
            flags |= KEYEVENTF_EXTENDEDKEY
        if vk == 0 and scan > 0:
            flags |= KEYEVENTF_SCANCODE

        payload = INPUT(
            type=INPUT_KEYBOARD,
            ki=KEYBDINPUT(
                wVk=vk,
                wScan=scan,
                dwFlags=flags,
                time=0,
                dwExtraInfo=0,
            ),
        )
        self._send_input(payload)

    def _send_mouse_button(self, button: str, down: bool) -> None:
        flags = 0
        mouse_data = 0
        normalized = button.lower()
        if normalized == "left":
            flags = MOUSEEVENTF_LEFTDOWN if down else MOUSEEVENTF_LEFTUP
        elif normalized == "right":
            flags = MOUSEEVENTF_RIGHTDOWN if down else MOUSEEVENTF_RIGHTUP
        elif normalized == "middle":
            flags = MOUSEEVENTF_MIDDLEDOWN if down else MOUSEEVENTF_MIDDLEUP
        elif normalized == "x1":
            flags = MOUSEEVENTF_XDOWN if down else MOUSEEVENTF_XUP
            mouse_data = XBUTTON1
        elif normalized == "x2":
            flags = MOUSEEVENTF_XDOWN if down else MOUSEEVENTF_XUP
            mouse_data = XBUTTON2
        else:
            return

        payload = INPUT(
            type=INPUT_MOUSE,
            mi=MOUSEINPUT(
                dx=0,
                dy=0,
                mouseData=mouse_data,
                dwFlags=flags,
                time=0,
                dwExtraInfo=0,
            ),
        )
        self._send_input(payload)

    def _send_mouse_wheel(self, event: Dict[str, Any], horizontal: bool) -> None:
        delta = int(event.get("delta", event.get("wheel_delta", 0)) or 0)
        if delta == 0:
            return
        payload = INPUT(
            type=INPUT_MOUSE,
            mi=MOUSEINPUT(
                dx=0,
                dy=0,
                mouseData=delta,
                dwFlags=MOUSEEVENTF_HWHEEL if horizontal else MOUSEEVENTF_WHEEL,
                time=0,
                dwExtraInfo=0,
            ),
        )
        self._send_input(payload)
