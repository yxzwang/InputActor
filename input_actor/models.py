from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict


@dataclass
class SessionScreen:
    virtual_left: int = 0
    virtual_top: int = 0
    virtual_width: int = 1
    virtual_height: int = 1


@dataclass
class SessionMeta:
    qpc_freq: int = 10_000_000
    screen: SessionScreen = field(default_factory=SessionScreen)

    @classmethod
    def from_header(cls, header: Dict[str, Any]) -> "SessionMeta":
        qpc_freq = int(header.get("qpc_freq", 10_000_000) or 10_000_000)
        screen_raw = header.get("screen") or {}
        screen = SessionScreen(
            virtual_left=int(screen_raw.get("virtual_left", 0) or 0),
            virtual_top=int(screen_raw.get("virtual_top", 0) or 0),
            virtual_width=max(int(screen_raw.get("virtual_width", 1) or 1), 1),
            virtual_height=max(int(screen_raw.get("virtual_height", 1) or 1), 1),
        )
        return cls(qpc_freq=max(qpc_freq, 1), screen=screen)
