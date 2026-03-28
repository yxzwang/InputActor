from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, Optional

MAX_XINPUT_GAMEPAD_INDEX = 3


class GamepadUnavailableError(RuntimeError):
    pass


@dataclass
class _PadState:
    left_stick_x: int = 0
    left_stick_y: int = 0
    right_stick_x: int = 0
    right_stick_y: int = 0
    left_trigger: int = 0
    right_trigger: int = 0


class GamepadManager:
    def __init__(self) -> None:
        self._backend: Optional[_VGamepadBackend] = None

    def send_event(self, event: Dict[str, Any]) -> bool:
        event_type = str(event.get("type", "")).lower()
        if not event_type.startswith("gamepad_"):
            return False
        self._ensure_backend().handle_event(event)
        return True

    def close(self) -> None:
        if self._backend is None:
            return
        self._backend.close()
        self._backend = None

    def _ensure_backend(self) -> "_VGamepadBackend":
        if self._backend is None:
            self._backend = _VGamepadBackend()
        return self._backend


class _VGamepadBackend:
    def __init__(self) -> None:
        try:
            import vgamepad as vg  # type: ignore[import-not-found]
        except Exception as exc:  # noqa: BLE001
            raise GamepadUnavailableError(
                "Gamepad replay requires Python package 'vgamepad' and ViGEmBus driver. "
                "Install by running: pip install vgamepad; then install ViGEmBus."
            ) from exc

        self._vg = vg
        self._pads: Dict[int, Any] = {}
        self._states: Dict[int, _PadState] = {}
        self._button_map = self._build_button_map()

    def handle_event(self, event: Dict[str, Any]) -> None:
        event_type = str(event.get("type", "")).lower()

        if event_type == "gamepad_connected":
            pad, _state = self._get_or_create_pad(event)
            pad.reset()
            pad.update()
            return

        if event_type == "gamepad_disconnected":
            pad, _state = self._get_or_create_pad(event)
            pad.reset()
            pad.update()
            return

        if event_type in {"gamepad_button_down", "gamepad_button_up"}:
            self._handle_button(event, down=(event_type == "gamepad_button_down"))
            return

        if event_type == "gamepad_axis":
            self._handle_axis(event)
            return

        raise ValueError(f"Unsupported gamepad event type: {event_type}")

    def close(self) -> None:
        for pad in self._pads.values():
            try:
                pad.reset()
                pad.update()
            except Exception:  # noqa: BLE001
                pass
            try:
                pad.__del__()
            except Exception:  # noqa: BLE001
                pass
        self._pads.clear()
        self._states.clear()

    def _handle_button(self, event: Dict[str, Any], down: bool) -> None:
        control = self._normalize_control_name(str(event.get("control", "")))
        button = self._button_map.get(control)
        if button is None:
            raise ValueError(f"Unsupported gamepad button control: {control}")

        pad, _state = self._get_or_create_pad(event)
        if down:
            pad.press_button(button=button)
        else:
            pad.release_button(button=button)
        pad.update()

    def _handle_axis(self, event: Dict[str, Any]) -> None:
        control = self._normalize_control_name(str(event.get("control", "")))
        value = event.get("value", 0)

        pad, state = self._get_or_create_pad(event)

        if control == "left_stick_x":
            state.left_stick_x = self._clamp_stick_value(value)
            pad.left_joystick(x_value=state.left_stick_x, y_value=state.left_stick_y)
        elif control == "left_stick_y":
            state.left_stick_y = self._clamp_stick_value(value)
            pad.left_joystick(x_value=state.left_stick_x, y_value=state.left_stick_y)
        elif control == "right_stick_x":
            state.right_stick_x = self._clamp_stick_value(value)
            pad.right_joystick(x_value=state.right_stick_x, y_value=state.right_stick_y)
        elif control == "right_stick_y":
            state.right_stick_y = self._clamp_stick_value(value)
            pad.right_joystick(x_value=state.right_stick_x, y_value=state.right_stick_y)
        elif control == "left_trigger":
            state.left_trigger = self._clamp_trigger_value(value)
            pad.left_trigger(value=state.left_trigger)
        elif control == "right_trigger":
            state.right_trigger = self._clamp_trigger_value(value)
            pad.right_trigger(value=state.right_trigger)
        else:
            raise ValueError(f"Unsupported gamepad axis control: {control}")

        pad.update()

    def _get_or_create_pad(self, event: Dict[str, Any]) -> tuple[Any, _PadState]:
        idx = self._normalize_pad_index(event.get("gamepad_index", 0))
        for candidate in range(idx + 1):
            if candidate in self._pads:
                continue
            self._pads[candidate] = self._vg.VX360Gamepad()
            self._states[candidate] = _PadState()
        pad = self._pads[idx]
        state = self._states[idx]
        return pad, state

    @staticmethod
    def _normalize_pad_index(raw: Any) -> int:
        try:
            value = int(raw)
        except Exception:  # noqa: BLE001
            return 0
        if value < 0:
            return 0
        if value > MAX_XINPUT_GAMEPAD_INDEX:
            raise ValueError(
                f"Unsupported gamepad_index: {value}. "
                f"Supported range is 0-{MAX_XINPUT_GAMEPAD_INDEX}."
            )
        return value

    @staticmethod
    def _clamp_stick_value(raw: Any) -> int:
        try:
            value = int(round(float(raw)))
        except Exception:  # noqa: BLE001
            value = 0
        return max(min(value, 32767), -32768)

    @staticmethod
    def _clamp_trigger_value(raw: Any) -> int:
        try:
            as_float = float(raw)
        except Exception:  # noqa: BLE001
            as_float = 0.0

        if 0.0 <= as_float <= 1.0:
            scaled = int(round(as_float * 255))
            return max(min(scaled, 255), 0)

        if as_float > 255.0:
            # Some recorders may emit trigger values in a wider integer range.
            as_float = as_float / 32767.0 * 255.0

        value = int(round(as_float))
        return max(min(value, 255), 0)

    @staticmethod
    def _normalize_control_name(raw: str) -> str:
        normalized = raw.strip().lower().replace("-", "_").replace(" ", "_")

        aliases = {
            "lb": "left_shoulder",
            "rb": "right_shoulder",
            "ls": "left_thumb",
            "rs": "right_thumb",
            "left_stick_press": "left_thumb",
            "right_stick_press": "right_thumb",
            "left_thumbstick": "left_thumb",
            "right_thumbstick": "right_thumb",
            "select": "back",
            "view": "back",
            "menu": "start",
            "options": "start",
            "lt": "left_trigger",
            "rt": "right_trigger",
            "l2": "left_trigger",
            "r2": "right_trigger",
        }
        return aliases.get(normalized, normalized)

    def _build_button_map(self) -> Dict[str, Any]:
        buttons = self._vg.XUSB_BUTTON

        def resolve(*names: str) -> Any:
            for name in names:
                if hasattr(buttons, name):
                    return getattr(buttons, name)
            return None

        mapping = {
            "a": resolve("XUSB_GAMEPAD_A", "A"),
            "b": resolve("XUSB_GAMEPAD_B", "B"),
            "x": resolve("XUSB_GAMEPAD_X", "X"),
            "y": resolve("XUSB_GAMEPAD_Y", "Y"),
            "dpad_up": resolve("XUSB_GAMEPAD_DPAD_UP", "DPAD_UP"),
            "dpad_down": resolve("XUSB_GAMEPAD_DPAD_DOWN", "DPAD_DOWN"),
            "dpad_left": resolve("XUSB_GAMEPAD_DPAD_LEFT", "DPAD_LEFT"),
            "dpad_right": resolve("XUSB_GAMEPAD_DPAD_RIGHT", "DPAD_RIGHT"),
            "start": resolve("XUSB_GAMEPAD_START", "START"),
            "back": resolve("XUSB_GAMEPAD_BACK", "BACK", "SELECT"),
            "guide": resolve("XUSB_GAMEPAD_GUIDE", "GUIDE"),
            "left_shoulder": resolve(
                "XUSB_GAMEPAD_LEFT_SHOULDER",
                "XUSB_GAMEPAD_LEFT_SHOULDER_BUTTON",
                "LEFT_SHOULDER",
            ),
            "right_shoulder": resolve(
                "XUSB_GAMEPAD_RIGHT_SHOULDER",
                "XUSB_GAMEPAD_RIGHT_SHOULDER_BUTTON",
                "RIGHT_SHOULDER",
            ),
            "left_thumb": resolve(
                "XUSB_GAMEPAD_LEFT_THUMB",
                "XUSB_GAMEPAD_LEFT_THUMB_BUTTON",
                "LEFT_THUMB",
            ),
            "right_thumb": resolve(
                "XUSB_GAMEPAD_RIGHT_THUMB",
                "XUSB_GAMEPAD_RIGHT_THUMB_BUTTON",
                "RIGHT_THUMB",
            ),
        }
        return {name: value for name, value in mapping.items() if value is not None}
