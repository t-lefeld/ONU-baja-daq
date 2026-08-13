"""
Small JSON settings file, so double-clicking the .exe remembers what worked
last time instead of making you pass flags you cannot pass to a double-click.

Deliberately tiny and forgiving: a corrupt or hand-edited file falls back to
defaults rather than refusing to start. A telemetry viewer that will not open
because its config has a stray comma is worse than one that ignores it.
"""

from __future__ import annotations

import json
import logging
from typing import Any

from .paths import settings_path

log = logging.getLogger("telemetry")

DEFAULTS: dict[str, Any] = {
    "port": None,        # last serial port that worked, e.g. "COM7"
    "baud": 9600,
    "http_port": 8765,
    "host": "127.0.0.1",
    "csv": False,        # auto-record CSV on every run
    "open_browser": True,
}


def load() -> dict[str, Any]:
    path = settings_path()
    values = dict(DEFAULTS)

    if not path.exists():
        return values

    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        log.warning("Ignoring unreadable settings file %s (%s)", path, exc)
        return values

    if not isinstance(raw, dict):
        log.warning("Ignoring settings file %s: expected a JSON object", path)
        return values

    # Only accept keys we know, with the type the default implies. Stops a
    # stray edit turning into a confusing runtime error deep in the stack.
    for key, default in DEFAULTS.items():
        if key not in raw:
            continue
        value = raw[key]
        if default is None or value is None or isinstance(value, type(default)):
            values[key] = value
        else:
            log.warning("Ignoring settings key %r: expected %s",
                        key, type(default).__name__)

    return values


def save(values: dict[str, Any]) -> bool:
    """
    Merge @p values into the stored settings and write the whole file.

    Merging rather than overwriting matters: save({"port": "COM7"}) should
    change the port and leave everything else alone. Writing only the keys
    passed in would quietly reset the rest to defaults on the next load, which
    is the kind of bug you notice weeks later when a setting keeps forgetting
    itself.

    Always writes the complete key set, so the file doubles as documentation of
    what can be configured.
    """
    path = settings_path()

    merged = load()
    merged.update({k: v for k, v in values.items() if k in DEFAULTS})

    try:
        path.write_text(json.dumps(merged, indent=2) + "\n", encoding="utf-8")
        return True
    except OSError as exc:
        # Read-only location, e.g. the exe was dropped in Program Files.
        # Not worth failing the run over.
        log.warning("Could not save settings to %s (%s)", path, exc)
        return False


def remember_port(port: str | None, baud: int) -> None:
    if not port:
        return
    values = load()
    if values.get("port") == port and values.get("baud") == baud:
        return
    values["port"] = port
    values["baud"] = baud
    save(values)
