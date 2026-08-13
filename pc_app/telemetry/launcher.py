"""
Opens the dashboard as a standalone window instead of a browser tab.

Chromium-based browsers (Edge, Chrome) support --app=<url>, which opens a
window with no tabs, no address bar, and its own taskbar and Alt-Tab entry -
the closest thing to a native desktop app without adding a GUI toolkit or an
embedded-browser library (pywebview, CEF) to the frozen build. Those work, but
they roughly double the size of the .exe and each has its own bundling
failure modes with PyInstaller that are hard to debug without a Windows
machine in the loop. --app mode needs nothing beyond a browser the user
already has.

Edge is tried first because it ships with every Windows 10/11 install -
nothing extra to require. Chrome is the fallback. If neither is found, or this
isn't Windows, opening degrades to a normal browser tab rather than failing
outright: a tab is a worse experience, not a broken one.
"""

from __future__ import annotations

import logging
import platform
import subprocess
import webbrowser
from pathlib import Path
from typing import Callable

log = logging.getLogger("telemetry")

try:
    import winreg  # type: ignore[import]
except ImportError:  # not on Windows
    winreg = None  # type: ignore[assignment]


# Windows registers an app's exe here so `start chrome` works from anywhere
# without the exe being on PATH. This is more reliable than searching PATH,
# since neither browser puts itself there by default.
_APP_PATH_KEYS = [
    (r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe", "Edge"),
    (r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe", "Chrome"),
]

# Checked if the registry lookup comes up empty - seen on some locked-down or
# portable installs where App Paths was never registered.
_FALLBACK_PATHS = [
    (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", "Edge"),
    (r"C:\Program Files\Microsoft\Edge\Application\msedge.exe", "Edge"),
    (r"C:\Program Files\Google\Chrome\Application\chrome.exe", "Chrome"),
    (r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe", "Chrome"),
]


def _registry_browser_path(key_path: str) -> str | None:
    if winreg is None:
        return None
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        try:
            with winreg.OpenKey(hive, key_path) as key:
                value, _ = winreg.QueryValueEx(key, None)
                if value:
                    return value
        except OSError:
            continue
    return None


def find_app_browser(
    exists: Callable[[str], bool] = lambda p: Path(p).exists(),
    registry_lookup: Callable[[str], str | None] = _registry_browser_path,
) -> tuple[str, str] | None:
    """
    First Chromium browser found, as (path, name), or None.

    `exists` and `registry_lookup` are injectable so the search order is
    testable without touching the real filesystem or Windows registry.
    """
    for key_path, name in _APP_PATH_KEYS:
        path = registry_lookup(key_path)
        if path and exists(path):
            return path, name

    for path, name in _FALLBACK_PATHS:
        if exists(path):
            return path, name

    return None


def build_launch_args(browser_path: str, url: str, profile_dir: Path) -> list[str]:
    """
    Command line for a standalone app window.

    The dedicated --user-data-dir is what actually forces a new window: if
    pointed at the user's normal profile, a browser that's already running
    just hands the URL to its existing process as an ordinary tab and quietly
    ignores --app. A private, dedicated profile guarantees a fresh process and
    the tab-less window we're asking for - and keeps it out of the user's
    regular history and bookmarks.
    """
    return [
        browser_path,
        f"--app={url}",
        f"--user-data-dir={profile_dir}",
        "--window-size=1180,820",
        "--no-first-run",
        "--no-default-browser-check",
    ]


def open_app_window(url: str, profile_dir: Path) -> bool:
    """
    Try to open `url` as a standalone window.

    Returns True if a dedicated app-window process was launched, False if it
    fell back to the system's default browser tab. Never raises: a failure
    here should degrade the experience, not take down the app that already
    started successfully.
    """
    if platform.system() == "Windows":
        found = find_app_browser()
        if found:
            path, name = found
            try:
                profile_dir.mkdir(parents=True, exist_ok=True)
                subprocess.Popen(
                    build_launch_args(path, url, profile_dir),
                    close_fds=True,
                )
                log.info("Opened as a standalone window (%s)", name)
                return True
            except OSError as exc:
                log.warning("Could not launch %s in app mode (%s); "
                           "opening a browser tab instead", name, exc)
        else:
            log.info("No Edge or Chrome found - opening a browser tab instead")

    webbrowser.open(url)
    return False
