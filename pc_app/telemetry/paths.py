"""
Path resolution that works both from source and inside a PyInstaller build.

Two different notions of "where am I", and conflating them is the classic way a
frozen app breaks:

  resource_dir()  read-only bundled assets. Under a one-file PyInstaller build
                  this is a temp directory that is deleted when the process
                  exits. Fine for static/index.html, useless for logs.

  data_dir()      writable, persistent, and somewhere the user can actually
                  find it. Next to the .exe when frozen, pc_app/ in dev.

Writing logs into resource_dir() would appear to work and then silently vanish
on exit, which is a miserable bug to chase.
"""

from __future__ import annotations

import sys
from pathlib import Path


def is_frozen() -> bool:
    """True when running from a PyInstaller build."""
    return getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS")


def resource_dir() -> Path:
    """Directory holding bundled read-only assets."""
    if is_frozen():
        return Path(sys._MEIPASS)  # type: ignore[attr-defined]
    # telemetry/paths.py -> telemetry/ -> pc_app/
    return Path(__file__).resolve().parent.parent


def data_dir() -> Path:
    """Writable directory that survives the process and the user can browse to."""
    if is_frozen():
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


def static_dir() -> Path:
    return resource_dir() / "static"


def logs_dir() -> Path:
    d = data_dir() / "logs"
    d.mkdir(parents=True, exist_ok=True)
    return d


def settings_path() -> Path:
    return data_dir() / "settings.json"


def describe() -> str:
    """One-line summary, logged at startup so path bugs are obvious in a report."""
    mode = "frozen" if is_frozen() else "source"
    return f"{mode}: resources={resource_dir()} data={data_dir()}"
