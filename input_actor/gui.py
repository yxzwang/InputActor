from __future__ import annotations

import queue
import time
from datetime import datetime
from pathlib import Path
from tkinter import Tk, filedialog, messagebox, scrolledtext
from tkinter import StringVar
from tkinter import ttk

from .hotkeys import WinHotkeyManager
from .player import InputPlayer
from .win_input import WindowsInputSender


DEFAULT_START_HOTKEY = "ctrl+alt+f6"
DEFAULT_PAUSE_HOTKEY = "ctrl+alt+f7"
DEFAULT_STOP_HOTKEY = "ctrl+alt+f8"
DEFAULT_START_DELAY_SECONDS = 3.0


class InputActorApp:
    def __init__(self, root: Tk) -> None:
        self.root = root
        self.root.title("Input Actor")
        self.root.geometry("820x560")
        self.root.minsize(780, 520)

        self.path_var = StringVar(value=self._default_input_path())
        self.status_var = StringVar(value="Ready.")
        self.state_var = StringVar(value="idle")
        self.progress_var = StringVar(value="0 / 0")
        self.event_var = StringVar(value="-")

        self.start_hotkey_var = StringVar(value=DEFAULT_START_HOTKEY)
        self.pause_hotkey_var = StringVar(value=DEFAULT_PAUSE_HOTKEY)
        self.stop_hotkey_var = StringVar(value=DEFAULT_STOP_HOTKEY)
        self.start_delay_var = StringVar(value=str(int(DEFAULT_START_DELAY_SECONDS)))

        self._event_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self._hotkeys = WinHotkeyManager()
        self._countdown_active = False
        self._countdown_end_monotonic = 0.0
        self._countdown_job: str | None = None
        self._countdown_last_second = -1

        self.player = InputPlayer(
            sender=WindowsInputSender(),
            on_status=lambda msg: self._event_queue.put(("status", msg)),
            on_progress=lambda current, total, event_type: self._event_queue.put(
                ("progress", (current, total, event_type))
            ),
            on_finish=lambda reason: self._event_queue.put(("finish", reason)),
        )

        self._build_ui()
        self._register_hotkeys()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(60, self._drain_events)

        if self.path_var.get():
            self.load_sequence(show_dialog=False)

    def _build_ui(self) -> None:
        container = ttk.Frame(self.root, padding=12)
        container.pack(fill="both", expand=True)

        self._build_file_row(container)
        self._build_hotkey_row(container)
        self._build_controls_row(container)
        self._build_progress_row(container)
        self._build_log_row(container)

    def _build_file_row(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Input Sequence", padding=10)
        frame.pack(fill="x")

        ttk.Label(frame, text="Path:").pack(side="left")
        ttk.Entry(frame, textvariable=self.path_var).pack(
            side="left", fill="x", expand=True, padx=(8, 8)
        )
        ttk.Button(frame, text="Browse", command=self.choose_file).pack(side="left")
        ttk.Button(frame, text="Load", command=self.load_sequence).pack(
            side="left", padx=(8, 0)
        )

    def _build_hotkey_row(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Hotkeys", padding=10)
        frame.pack(fill="x", pady=(10, 0))

        ttk.Label(frame, text="Start").grid(row=0, column=0, sticky="w")
        ttk.Entry(frame, width=22, textvariable=self.start_hotkey_var).grid(
            row=0, column=1, padx=(6, 14), sticky="w"
        )
        ttk.Label(frame, text="Pause/Resume").grid(row=0, column=2, sticky="w")
        ttk.Entry(frame, width=22, textvariable=self.pause_hotkey_var).grid(
            row=0, column=3, padx=(6, 14), sticky="w"
        )
        ttk.Label(frame, text="Stop").grid(row=0, column=4, sticky="w")
        ttk.Entry(frame, width=22, textvariable=self.stop_hotkey_var).grid(
            row=0, column=5, padx=(6, 14), sticky="w"
        )

        apply_button = ttk.Button(frame, text="Apply Hotkeys", command=self._register_hotkeys)
        apply_button.grid(row=0, column=6, sticky="e")

    def _build_controls_row(self, parent: ttk.Frame) -> None:
        frame = ttk.Frame(parent, padding=(0, 10, 0, 0))
        frame.pack(fill="x")

        self.start_button = ttk.Button(frame, text="Start", command=self.start_playback)
        self.start_button.pack(side="left")

        self.pause_button = ttk.Button(
            frame, text="Pause", command=self.toggle_pause_resume
        )
        self.pause_button.pack(side="left", padx=(8, 0))

        self.stop_button = ttk.Button(frame, text="Stop", command=self.stop_playback)
        self.stop_button.pack(side="left", padx=(8, 0))

        ttk.Label(frame, text="Start Delay(s):").pack(side="left", padx=(20, 6))
        ttk.Entry(frame, width=8, textvariable=self.start_delay_var).pack(side="left")

        ttk.Label(frame, text="State:").pack(side="left", padx=(20, 4))
        ttk.Label(frame, textvariable=self.state_var).pack(side="left")

    def _build_progress_row(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Playback Progress", padding=10)
        frame.pack(fill="x")

        self.progress_bar = ttk.Progressbar(frame, mode="determinate", maximum=100, value=0)
        self.progress_bar.pack(fill="x")

        detail = ttk.Frame(frame)
        detail.pack(fill="x", pady=(8, 0))

        ttk.Label(detail, text="Events:").grid(row=0, column=0, sticky="w")
        ttk.Label(detail, textvariable=self.progress_var).grid(
            row=0, column=1, sticky="w", padx=(6, 20)
        )
        ttk.Label(detail, text="Current Event:").grid(row=0, column=2, sticky="w")
        ttk.Label(detail, textvariable=self.event_var).grid(row=0, column=3, sticky="w", padx=(6, 0))

        ttk.Label(detail, textvariable=self.status_var).grid(
            row=1, column=0, columnspan=4, sticky="w", pady=(8, 0)
        )

    def _build_log_row(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Runtime Log", padding=10)
        frame.pack(fill="both", expand=True, pady=(10, 0))

        self.log_text = scrolledtext.ScrolledText(frame, height=12, state="disabled")
        self.log_text.pack(fill="both", expand=True)

    def choose_file(self) -> None:
        current = self.path_var.get().strip()
        initial_dir = str(Path(current).resolve().parent) if current else str(Path.cwd())
        selected = filedialog.askopenfilename(
            title="Choose input sequence",
            initialdir=initial_dir,
            filetypes=[("JSONL files", "*.jsonl"), ("All files", "*.*")],
        )
        if selected:
            self.path_var.set(selected)

    def load_sequence(self, show_dialog: bool = True) -> bool:
        path = self.path_var.get().strip()
        if not path:
            if show_dialog:
                messagebox.showerror("Load failed", "Please choose a .jsonl file first.")
            return False

        try:
            self.player.load(path)
        except Exception as exc:  # noqa: BLE001
            self._set_status(f"Load failed: {exc}")
            if show_dialog:
                messagebox.showerror("Load failed", str(exc))
            self._refresh_buttons()
            return False

        self._set_status(f"Loaded: {path}")
        self._refresh_buttons()
        return True

    def start_playback(self) -> None:
        if self.player.total_events == 0 and not self.load_sequence(show_dialog=True):
            return
        if self._countdown_active:
            self._set_status("Countdown is already running.")
            return

        current_state = self.player.state
        if current_state == "paused":
            self._set_status("Playback is paused. Use Pause/Resume hotkey to continue.")
            return
        if current_state == "running":
            self._set_status("Playback is already running.")
            return

        delay_seconds = self._parse_start_delay_seconds()
        if delay_seconds is None:
            return
        if delay_seconds <= 0:
            self._start_player_now()
            return

        self._start_countdown(delay_seconds)

    def _start_player_now(self) -> None:
        try:
            started = self.player.start()
        except Exception as exc:  # noqa: BLE001
            self._set_status(f"Cannot start playback: {exc}")
            messagebox.showerror("Start failed", str(exc))
            self._refresh_buttons()
            return

        if not started:
            self._set_status("Playback is already running.")
        self._refresh_buttons()

    def toggle_pause_resume(self) -> None:
        if self._countdown_active:
            self._set_status("Cannot pause during countdown.")
            return
        changed = self.player.toggle_pause()
        if not changed:
            self._set_status("Pause/Resume is available only while running.")
        self._refresh_buttons()

    def stop_playback(self) -> None:
        if self._cancel_countdown("Countdown canceled."):
            return
        stopped = self.player.stop()
        if stopped:
            self._set_status("Stopping requested.")
        else:
            self._set_status("Nothing is running.")
        self._refresh_buttons()

    def _parse_start_delay_seconds(self) -> float | None:
        text = self.start_delay_var.get().strip()
        if not text:
            self.start_delay_var.set(str(int(DEFAULT_START_DELAY_SECONDS)))
            return DEFAULT_START_DELAY_SECONDS
        try:
            value = float(text)
        except ValueError:
            messagebox.showerror("Invalid delay", "Start delay must be a number (seconds).")
            return None
        if value < 0:
            messagebox.showerror("Invalid delay", "Start delay cannot be negative.")
            return None
        return value

    def _start_countdown(self, delay_seconds: float) -> None:
        self._countdown_active = True
        self._countdown_end_monotonic = time.monotonic() + delay_seconds
        self._countdown_last_second = -1
        self._set_status(f"Countdown started: playback will begin in {delay_seconds:.1f}s.")
        self._refresh_buttons()
        self._tick_countdown()

    def _tick_countdown(self) -> None:
        if not self._countdown_active:
            return

        remaining = self._countdown_end_monotonic - time.monotonic()
        if remaining <= 0:
            self._countdown_active = False
            self._countdown_job = None
            self._countdown_last_second = -1
            self._set_status("Countdown finished. Starting playback...")
            self._start_player_now()
            return

        remain_int = int(remaining + 0.999)
        if remain_int != self._countdown_last_second:
            self._countdown_last_second = remain_int
            self.status_var.set(f"Starting in {remain_int}s...")

        self._countdown_job = self.root.after(100, self._tick_countdown)

    def _cancel_countdown(self, status_text: str | None = None) -> bool:
        if not self._countdown_active:
            return False
        if self._countdown_job is not None:
            try:
                self.root.after_cancel(self._countdown_job)
            except Exception:  # noqa: BLE001
                pass
        self._countdown_active = False
        self._countdown_job = None
        self._countdown_last_second = -1
        if status_text:
            self._set_status(status_text)
        self._refresh_buttons()
        return True

    def _register_hotkeys(self) -> None:
        start_hotkey = self.start_hotkey_var.get().strip().lower()
        pause_hotkey = self.pause_hotkey_var.get().strip().lower()
        stop_hotkey = self.stop_hotkey_var.get().strip().lower()

        if not start_hotkey or not pause_hotkey or not stop_hotkey:
            messagebox.showerror("Hotkey error", "All three hotkeys are required.")
            return

        try:
            self._hotkeys.register(
                {
                    "start": start_hotkey,
                    "pause": pause_hotkey,
                    "stop": stop_hotkey,
                }
            )
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("Hotkey error", str(exc))
            self._set_status(f"Failed to register hotkeys: {exc}")
            return

        self._set_status(
            f"Hotkeys registered: start={start_hotkey}, pause={pause_hotkey}, stop={stop_hotkey}"
        )

    def _unregister_hotkeys(self) -> None:
        self._hotkeys.unregister_all()

    def _drain_events(self) -> None:
        for action in self._hotkeys.poll_actions():
            if action == "start":
                self.start_playback()
            elif action == "pause":
                self.toggle_pause_resume()
            elif action == "stop":
                self.stop_playback()

        while True:
            try:
                key, payload = self._event_queue.get_nowait()
            except queue.Empty:
                break

            if key == "status":
                self._set_status(str(payload))
            elif key == "progress":
                current, total, event_type = payload  # type: ignore[misc]
                self._update_progress(int(current), int(total), str(event_type))
            elif key == "finish":
                pass

        self._refresh_buttons()
        self.root.after(60, self._drain_events)

    def _set_status(self, text: str) -> None:
        self.status_var.set(text)
        self._append_log(text)

    def _update_progress(self, current: int, total: int, event_type: str) -> None:
        total = max(total, 1)
        percent = min(max(current / total * 100, 0), 100)
        self.progress_bar.configure(value=percent)
        self.progress_var.set(f"{current} / {total}")
        self.event_var.set(event_type)

    def _append_log(self, text: str) -> None:
        timestamp = datetime.now().strftime("%H:%M:%S")
        line = f"[{timestamp}] {text}\n"
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line)
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _refresh_buttons(self) -> None:
        if self._countdown_active:
            self.state_var.set("countdown")
            self.start_button.state(["disabled"])
            self.pause_button.state(["disabled"])
            self.stop_button.state(["!disabled"])
            self.pause_button.configure(text="Pause")
            return

        state = self.player.state
        self.state_var.set(state)

        if state in {"ready", "finished", "stopped", "error", "idle"}:
            self.start_button.state(["!disabled"])
        else:
            self.start_button.state(["disabled"])

        if state in {"running", "paused"}:
            self.pause_button.state(["!disabled"])
            self.stop_button.state(["!disabled"])
        else:
            self.pause_button.state(["disabled"])
            self.stop_button.state(["disabled"])

        if state == "paused":
            self.pause_button.configure(text="Resume")
        else:
            self.pause_button.configure(text="Pause")

    def _on_close(self) -> None:
        self._cancel_countdown(None)
        try:
            self.player.stop()
        except Exception:  # noqa: BLE001
            pass
        self._hotkeys.shutdown()
        self.root.destroy()

    @staticmethod
    def _default_input_path() -> str:
        default = Path.cwd() / "20260218_232051" / "input.jsonl"
        if default.exists():
            return str(default)

        for candidate in Path.cwd().rglob("*.jsonl"):
            return str(candidate)
        return ""


def run_app() -> None:
    root = Tk()
    InputActorApp(root)
    root.mainloop()
