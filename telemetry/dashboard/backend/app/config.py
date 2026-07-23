"""
Configuration for the pit-side telemetry logging backend.

The single most important idea here: storage is a *path*, not a device.
Point LOG_STORAGE_PATH at a local folder for dev/testing now, and at the
real SD card mount point later (e.g. /media/sdcard on Linux, or a drive
letter like E:\\ on Windows) once hardware is available. No code changes
are required to switch -- only this config value.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field


def _default_storage_path() -> str:
    return os.environ.get("LOG_STORAGE_PATH", "./data/sd_mock")


def _default_flush_interval() -> float:
    return float(os.environ.get("LOG_FLUSH_INTERVAL_SECONDS", "0.5"))


@dataclass(frozen=True)
class Settings:
    # Base path for all session log files. Defaults to a local mock
    # "SD card" folder so the whole pipeline can be validated without
    # real hardware.
    storage_path: str = field(default_factory=_default_storage_path)

    # How often (seconds) the background task commits/flushes buffered
    # writes to disk for the currently active session. This bounds how
    # much data could be lost in a crash without fsyncing on every
    # single packet (which would be too slow at real telemetry rates).
    flush_interval_seconds: float = field(default_factory=_default_flush_interval)


def get_settings() -> Settings:
    """Re-reads environment each call so tests/config overrides that set
    LOG_STORAGE_PATH before constructing Settings() take effect."""
    return Settings()
