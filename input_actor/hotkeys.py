from __future__ import annotations

import ctypes
import queue
import threading
import time
from ctypes import wintypes
from dataclasses import dataclass
from typing import Any, Dict, List, Tuple

WM_HOTKEY = 0x0312
PM_REMOVE = 0x0001
PM_NOREMOVE = 0x0000

MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008
MOD_NOREPEAT = 0x4000

VK_NAME_MAP = {
    "tab": 0x09,
    "enter": 0x0D,
    "return": 0x0D,
    "esc": 0x1B,
    "escape": 0x1B,
    "space": 0x20,
    "pgup": 0x21,
    "pageup": 0x21,
    "pgdn": 0x22,
    "pagedown": 0x22,
    "end": 0x23,
    "home": 0x24,
    "left": 0x25,
    "up": 0x26,
    "right": 0x27,
    "down": 0x28,
    "insert": 0x2D,
    "ins": 0x2D,
    "delete": 0x2E,
    "del": 0x2E,
    "backspace": 0x08,
}

MODIFIER_MAP = {
    "ctrl": MOD_CONTROL,
    "control": MOD_CONTROL,
    "alt": MOD_ALT,
    "shift": MOD_SHIFT,
    "win": MOD_WIN,
    "windows": MOD_WIN,
    "cmd": MOD_WIN,
}


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class MSG(ctypes.Structure):
    _fields_ = [
        ("hwnd", wintypes.HWND),
        ("message", wintypes.UINT),
        ("wParam", wintypes.WPARAM),
        ("lParam", wintypes.LPARAM),
        ("time", wintypes.DWORD),
        ("pt", POINT),
    ]


@dataclass(frozen=True)
class ParsedHotkey:
    modifiers: int
    vk: int


user32 = ctypes.WinDLL("user32", use_last_error=True)
register_hotkey = user32.RegisterHotKey
register_hotkey.argtypes = (wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT)
register_hotkey.restype = wintypes.BOOL

unregister_hotkey = user32.UnregisterHotKey
unregister_hotkey.argtypes = (wintypes.HWND, ctypes.c_int)
unregister_hotkey.restype = wintypes.BOOL

peek_message = user32.PeekMessageW
peek_message.argtypes = (
    ctypes.POINTER(MSG),
    wintypes.HWND,
    wintypes.UINT,
    wintypes.UINT,
    wintypes.UINT,
)
peek_message.restype = wintypes.BOOL


def parse_hotkey(expr: str) -> ParsedHotkey:
    text = expr.strip().lower()
    if not text:
        raise ValueError("Hotkey cannot be empty.")

    tokens = [token.strip() for token in text.split("+") if token.strip()]
    if not tokens:
        raise ValueError("Invalid hotkey expression.")

    modifiers = 0
    key_token = tokens[-1]
    for token in tokens[:-1]:
        if token not in MODIFIER_MAP:
            raise ValueError(f"Unsupported modifier: {token}")
        modifiers |= MODIFIER_MAP[token]

    vk = _parse_vk(key_token)
    return ParsedHotkey(modifiers=modifiers, vk=vk)


def _parse_vk(token: str) -> int:
    if token in VK_NAME_MAP:
        return VK_NAME_MAP[token]

    if len(token) == 1:
        char = token.upper()
        if "A" <= char <= "Z" or "0" <= char <= "9":
            return ord(char)

    if token.startswith("f") and token[1:].isdigit():
        num = int(token[1:])
        if 1 <= num <= 24:
            return 0x70 + num - 1

    raise ValueError(f"Unsupported hotkey key: {token}")


class WinHotkeyManager:
    def __init__(self) -> None:
        self._actions: queue.Queue[str] = queue.Queue()
        self._commands: queue.Queue[tuple[str, Any, Dict[str, Any], threading.Event]] = (
            queue.Queue()
        )
        self._ready = threading.Event()
        self._thread = threading.Thread(
            target=self._message_loop,
            name="win-hotkeys",
            daemon=True,
        )
        self._thread.start()
        if not self._ready.wait(timeout=2.0):
            raise RuntimeError("Hotkey service startup timed out.")

    def register(self, mapping: Dict[str, str]) -> None:
        self._invoke("register", mapping)

    def unregister_all(self) -> None:
        self._invoke("unregister", None)

    def shutdown(self) -> None:
        if self._thread.is_alive():
            try:
                self._invoke("stop", None)
            except Exception:
                pass
            self._thread.join(timeout=1.0)

    def poll_actions(self) -> List[str]:
        actions: List[str] = []
        while True:
            try:
                actions.append(self._actions.get_nowait())
            except queue.Empty:
                break
        return actions

    def _invoke(self, command: str, payload: Any) -> None:
        result: Dict[str, Any] = {}
        done = threading.Event()
        self._commands.put((command, payload, result, done))
        if not done.wait(timeout=2.0):
            raise RuntimeError("Hotkey command timed out.")
        error = result.get("error")
        if error is not None:
            raise error

    def _message_loop(self) -> None:
        # Ensure this thread owns a message queue before RegisterHotKey is used.
        dummy = MSG()
        peek_message(ctypes.byref(dummy), None, 0, 0, PM_NOREMOVE)

        id_to_action: Dict[int, str] = {}
        active_ids: List[int] = []
        running = True
        self._ready.set()

        while running:
            self._drain_hotkey_messages(id_to_action)
            running = self._drain_commands(id_to_action, active_ids, running)
            time.sleep(0.01)

        self._unregister_all_impl(active_ids, id_to_action)

    def _drain_hotkey_messages(self, id_to_action: Dict[int, str]) -> None:
        message = MSG()
        while peek_message(
            ctypes.byref(message),
            None,
            WM_HOTKEY,
            WM_HOTKEY,
            PM_REMOVE,
        ):
            action = id_to_action.get(int(message.wParam))
            if action:
                self._actions.put(action)

    def _drain_commands(
        self,
        id_to_action: Dict[int, str],
        active_ids: List[int],
        running: bool,
    ) -> bool:
        while True:
            try:
                command, payload, result, done = self._commands.get_nowait()
            except queue.Empty:
                return running

            try:
                if command == "register":
                    self._register_impl(payload, active_ids, id_to_action)
                elif command == "unregister":
                    self._unregister_all_impl(active_ids, id_to_action)
                elif command == "stop":
                    self._unregister_all_impl(active_ids, id_to_action)
                    running = False
                else:
                    raise ValueError(f"Unknown hotkey command: {command}")
            except Exception as exc:  # noqa: BLE001
                result["error"] = exc
            finally:
                done.set()

    def _register_impl(
        self,
        mapping: Dict[str, str],
        active_ids: List[int],
        id_to_action: Dict[int, str],
    ) -> None:
        if not mapping:
            raise ValueError("Hotkey mapping is empty.")

        self._unregister_all_impl(active_ids, id_to_action)
        seen: set[Tuple[int, int]] = set()

        try:
            for hotkey_id, (action, expr) in enumerate(mapping.items(), start=1):
                parsed = parse_hotkey(expr)
                key = (parsed.modifiers, parsed.vk)
                if key in seen:
                    raise ValueError("Start/Pause/Stop hotkeys must be different.")
                seen.add(key)

                modifiers = parsed.modifiers | MOD_NOREPEAT
                ok = register_hotkey(None, hotkey_id, modifiers, parsed.vk)
                if not ok:
                    error_code = ctypes.get_last_error()
                    raise OSError(
                        error_code,
                        f"Cannot register hotkey '{expr}'. "
                        f"It may be used by another app (win32={error_code}).",
                    )

                active_ids.append(hotkey_id)
                id_to_action[hotkey_id] = action
        except Exception:
            self._unregister_all_impl(active_ids, id_to_action)
            raise

    @staticmethod
    def _unregister_all_impl(
        active_ids: List[int],
        id_to_action: Dict[int, str],
    ) -> None:
        for hotkey_id in active_ids:
            unregister_hotkey(None, hotkey_id)
        active_ids.clear()
        id_to_action.clear()
