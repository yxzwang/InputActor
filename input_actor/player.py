from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

from .models import SessionMeta
from .win_input import WindowsInputSender

StatusCallback = Callable[[str], None]
ProgressCallback = Callable[[int, int, str], None]
FinishCallback = Callable[[str], None]

META_TYPES = {"session_header", "stats"}


def load_jsonl(path: str | Path) -> Tuple[SessionMeta, List[Dict[str, Any]]]:
    data_path = Path(path)
    if not data_path.exists():
        raise FileNotFoundError(f"File not found: {data_path}")
    if data_path.suffix.lower() != ".jsonl":
        raise ValueError("Input file must use .jsonl extension.")

    header: Dict[str, Any] = {}
    events: List[Dict[str, Any]] = []

    with data_path.open("r", encoding="utf-8") as file:
        for line_number, line in enumerate(file, start=1):
            text = line.strip()
            if not text:
                continue
            try:
                record = json.loads(text)
            except json.JSONDecodeError as exc:
                raise ValueError(
                    f"Invalid JSON at line {line_number}: {exc.msg}"
                ) from exc

            if not isinstance(record, dict):
                continue
            event_type = str(record.get("type", "")).lower()
            if event_type == "session_header":
                header = record
                continue
            if event_type in META_TYPES:
                continue
            if "t_qpc" not in record:
                continue
            events.append(record)

    if not events:
        raise ValueError("No replayable events were found in the file.")
    return SessionMeta.from_header(header), events


class InputPlayer:
    def __init__(
        self,
        sender: Optional[WindowsInputSender] = None,
        on_status: Optional[StatusCallback] = None,
        on_progress: Optional[ProgressCallback] = None,
        on_finish: Optional[FinishCallback] = None,
    ) -> None:
        self._sender = sender or WindowsInputSender()
        self._on_status = on_status or (lambda _msg: None)
        self._on_progress = on_progress or (lambda _current, _total, _event_type: None)
        self._on_finish = on_finish or (lambda _reason: None)

        self._events: List[Dict[str, Any]] = []
        self._meta = SessionMeta()
        self._path = ""

        self._lock = threading.Lock()
        self._worker: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._pause_gate = threading.Event()
        self._pause_gate.set()

        self._state = "idle"
        self._current = 0

    @property
    def state(self) -> str:
        with self._lock:
            return self._state

    @property
    def total_events(self) -> int:
        with self._lock:
            return len(self._events)

    @property
    def current_index(self) -> int:
        with self._lock:
            return self._current

    @property
    def source_path(self) -> str:
        with self._lock:
            return self._path

    def load(self, path: str | Path) -> None:
        if self.state in {"running", "paused"}:
            raise RuntimeError("Cannot load while playback is running.")
        meta, events = load_jsonl(path)
        self._sender.configure(meta)
        with self._lock:
            self._path = str(path)
            self._meta = meta
            self._events = events
            self._current = 0
            self._state = "ready"
        self._on_status(f"Loaded {len(events)} events from {path}")
        self._on_progress(0, len(events), "-")

    def start(self) -> bool:
        with self._lock:
            if not self._events:
                raise RuntimeError("No input sequence loaded.")
            if self._worker and self._worker.is_alive():
                return False

            self._stop_event.clear()
            self._pause_gate.set()
            self._current = 0
            self._state = "running"
            self._worker = threading.Thread(target=self._run, name="input-player", daemon=True)
            self._worker.start()
        self._on_status("Playback started.")
        return True

    def toggle_pause(self) -> bool:
        with self._lock:
            if self._state == "running":
                self._pause_gate.clear()
                self._state = "paused"
                self._on_status("Playback paused.")
                return True
            if self._state == "paused":
                self._pause_gate.set()
                self._state = "running"
                self._on_status("Playback resumed.")
                return True
        return False

    def stop(self) -> bool:
        with self._lock:
            if self._state not in {"running", "paused"}:
                return False
            self._state = "stopping"
            self._stop_event.set()
            self._pause_gate.set()
        self._on_status("Stopping playback...")
        return True

    def _run(self) -> None:
        reason = "completed"
        try:
            events = self._events_snapshot()
            total = len(events)
            qpc_freq = max(int(self._meta.qpc_freq), 1)

            first_t = int(events[0]["t_qpc"])
            started_wall = time.perf_counter()
            for index, event in enumerate(events, start=1):
                if self._stop_event.is_set():
                    reason = "stopped"
                    break

                target_offset = (int(event["t_qpc"]) - first_t) / qpc_freq
                target_wall = started_wall + target_offset
                ok, started_wall = self._wait_until(target_wall, started_wall)
                if not ok:
                    reason = "stopped"
                    break

                try:
                    self._sender.send_event(event)
                except Exception as exc:  # noqa: BLE001
                    reason = "error"
                    self._on_status(f"Playback failed: {exc}")
                    break

                with self._lock:
                    self._current = index
                self._on_progress(index, total, str(event.get("type", "-")))
            else:
                reason = "completed"
        finally:
            with self._lock:
                self._pause_gate.set()
                if reason == "completed":
                    self._state = "finished"
                elif reason == "stopped":
                    self._state = "stopped"
                else:
                    self._state = "error"

            if reason == "completed":
                self._on_status("Playback finished.")
            elif reason == "stopped":
                self._on_status("Playback stopped.")
            self._on_finish(reason)

    def _wait_until(self, target_wall: float, started_wall: float) -> Tuple[bool, float]:
        paused_since: Optional[float] = None
        while True:
            if self._stop_event.is_set():
                return False, started_wall

            if self._pause_gate.is_set():
                if paused_since is not None:
                    paused_duration = time.perf_counter() - paused_since
                    started_wall += paused_duration
                    target_wall += paused_duration
                    paused_since = None

                remaining = target_wall - time.perf_counter()
                if remaining <= 0:
                    return True, started_wall
                time.sleep(min(remaining, 0.005))
                continue

            if paused_since is None:
                paused_since = time.perf_counter()
            time.sleep(0.05)

    def _events_snapshot(self) -> List[Dict[str, Any]]:
        with self._lock:
            return list(self._events)
