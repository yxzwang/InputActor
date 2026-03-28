from __future__ import annotations

import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional, Tuple

try:
    import winreg
except Exception:  # noqa: BLE001
    winreg = None  # type: ignore[assignment]


VIGEM_BUS_UNINSTALL_HINTS = (
    "vigembus",
    "virtual gamepad emulation bus",
)
SUCCESS_EXIT_CODES = {0, 1641, 3010}


def is_vigembus_installed() -> bool:
    return _has_vigembus_uninstall_entry() or _has_vigembus_service_key()


def ensure_vigembus_installed() -> Tuple[bool, str]:
    if is_vigembus_installed():
        return True, "ViGEmBus is already installed."

    installer = find_bundled_vigembus_installer()
    if installer is None:
        return (
            False,
            "ViGEmBus is not installed and bundled installer was not found.",
        )

    log_path = Path(tempfile.gettempdir()) / "InputActor-ViGEmBus-install.log"
    exit_code, output = _run_installer_elevated(installer, log_path)

    if exit_code == 1223:
        return False, "User canceled the UAC prompt for ViGEmBus installation."

    if exit_code in SUCCESS_EXIT_CODES:
        for _ in range(12):
            if is_vigembus_installed():
                return True, f"ViGEmBus installed successfully (exit code {exit_code})."
            time.sleep(0.5)
        return (
            False,
            f"Installer finished with code {exit_code}, but ViGEmBus was not detected.",
        )

    details = f"Installer exit code: {exit_code}."
    if output:
        details = f"{details} {output}"
    return False, details


def find_bundled_vigembus_installer() -> Optional[Path]:
    candidates = []

    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        candidates.append(Path(meipass) / "vendor" / "ViGEmBusSetup.exe")

    module_root = Path(__file__).resolve().parent.parent
    candidates.append(module_root / "vendor" / "ViGEmBusSetup.exe")
    candidates.append(Path.cwd() / "vendor" / "ViGEmBusSetup.exe")

    for path in candidates:
        if path.exists():
            return path
    return None


def _run_installer_elevated(installer_path: Path, log_path: Path) -> Tuple[int, str]:
    exe = _ps_quote(str(installer_path))
    args = _ps_quote(f'/qn /norestart /log "{str(log_path)}"')

    script = (
        "$ErrorActionPreference='Stop';"
        f"$p=Start-Process -FilePath '{exe}' -ArgumentList '{args}' -Verb RunAs -PassThru -Wait;"
        "if ($null -eq $p -or $null -eq $p.ExitCode) { exit 1 };"
        "exit $p.ExitCode"
    )

    proc = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            script,
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    output = f"{proc.stdout}\n{proc.stderr}".strip()
    if proc.returncode != 0 and "canceled by the user" in output.lower():
        return 1223, output

    # Keep log path in output so troubleshooting remains actionable.
    if log_path.exists():
        output = f"{output} Install log: {str(log_path)}" if output else f"Install log: {str(log_path)}"

    return int(proc.returncode), output


def _has_vigembus_uninstall_entry() -> bool:
    if winreg is None:
        return False

    roots = (
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
        (
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
        ),
    )

    for hive, base_key in roots:
        try:
            with winreg.OpenKey(hive, base_key) as uninstall_root:
                subkey_count = winreg.QueryInfoKey(uninstall_root)[0]
                for index in range(subkey_count):
                    subkey_name = winreg.EnumKey(uninstall_root, index)
                    try:
                        with winreg.OpenKey(uninstall_root, subkey_name) as subkey:
                            display_name, _ = winreg.QueryValueEx(subkey, "DisplayName")
                    except Exception:  # noqa: BLE001
                        continue

                    if not isinstance(display_name, str):
                        continue
                    normalized = display_name.strip().lower()
                    if any(token in normalized for token in VIGEM_BUS_UNINSTALL_HINTS):
                        return True
        except Exception:  # noqa: BLE001
            continue

    return False


def _has_vigembus_service_key() -> bool:
    if winreg is None:
        return False
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Services\ViGEmBus",
        ):
            return True
    except Exception:  # noqa: BLE001
        return False


def _ps_quote(value: str) -> str:
    return value.replace("'", "''")
