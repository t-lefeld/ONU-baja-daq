#!/usr/bin/env python3
"""
Tests for telemetry/launcher.py - opening the dashboard as a standalone
app window instead of a browser tab.

    python tools/test_launcher.py

The real behaviour touches the Windows registry and spawns a browser process,
neither of which exists in this Linux sandbox. So every entry point takes its
filesystem/registry/process-spawn dependency as an injectable function, and
these tests supply fakes - which also makes the search order and fallback
chain explicit and checkable, rather than only discoverable by reading a
Windows box's actual state.
"""

from __future__ import annotations

import sys
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pc_app"))

failures: list[str] = []
checks = 0


def check(label: str, got, want) -> None:
    global checks
    checks += 1
    if got != want:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r}")


def ok(label: str, cond: bool) -> None:
    global checks
    checks += 1
    if not cond:
        failures.append(label)


# ====================================================================
# find_app_browser: search order and fallback
# ====================================================================


def test_find_app_browser() -> None:
    print("-- find_app_browser: search order")
    from telemetry import launcher

    # Nothing anywhere: no registry hit, no fallback path exists.
    result = launcher.find_app_browser(exists=lambda p: False, registry_lookup=lambda k: None)
    check("nothing found returns None", result, None)

    # Edge registered, Chrome not: Edge wins, and it must be checked first
    # regardless of what's in the registry, since it ships with Windows.
    def reg_edge_only(key: str) -> str | None:
        return r"C:\Edge\msedge.exe" if "msedge" in key else None

    result = launcher.find_app_browser(
        exists=lambda p: p == r"C:\Edge\msedge.exe",
        registry_lookup=reg_edge_only,
    )
    check("edge found via registry", result, (r"C:\Edge\msedge.exe", "Edge"))

    # Both registered: Edge must still win the tie, since it's tried first.
    def reg_both(key: str) -> str | None:
        if "msedge" in key:
            return r"C:\Edge\msedge.exe"
        if "chrome" in key:
            return r"C:\Chrome\chrome.exe"
        return None

    result = launcher.find_app_browser(exists=lambda p: True, registry_lookup=reg_both)
    check("edge preferred when both are registered", result, (r"C:\Edge\msedge.exe", "Edge"))

    # Registry has a stale entry (uninstalled but key left behind) - `exists`
    # must gate it, or we'd hand Popen a path that immediately fails.
    result = launcher.find_app_browser(
        exists=lambda p: False,
        registry_lookup=reg_both,
    )
    check("stale registry entry rejected without a filesystem hit", result, None)

    # Registry empty, but a fallback path exists: Chrome fallback is found.
    chrome_fallback = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
    result = launcher.find_app_browser(
        exists=lambda p: p == chrome_fallback,
        registry_lookup=lambda k: None,
    )
    check("falls back to a known install path", result, (chrome_fallback, "Chrome"))

    # Registry lookup itself is never called for platforms without winreg -
    # the real _registry_browser_path returns None cleanly rather than raising.
    ok("real registry lookup does not raise when winreg is unavailable",
       launcher._registry_browser_path(r"SOFTWARE\doesnotmatter") is None
       or launcher.winreg is not None)


# ====================================================================
# build_launch_args: what actually gets executed
# ====================================================================


def test_build_launch_args() -> None:
    print("-- build_launch_args: command line shape")
    from telemetry import launcher

    url = "http://127.0.0.1:8765/"
    profile = Path("/tmp/telemetry-profile")
    args = launcher.build_launch_args(r"C:\Edge\msedge.exe", url, profile)

    check("browser path is argv[0]", args[0], r"C:\Edge\msedge.exe")

    app_flags = [a for a in args if a.startswith("--app=")]
    check("exactly one --app flag", len(app_flags), 1)
    check("--app carries the exact URL", app_flags[0], f"--app={url}")

    profile_flags = [a for a in args if a.startswith("--user-data-dir=")]
    check("exactly one --user-data-dir flag", len(profile_flags), 1)
    ok("profile dir is the dedicated one, not the user's real profile",
       str(profile) in profile_flags[0])

    ok("no flag appears twice", len(args) == len(set(args)))

    # A dedicated profile is the whole point - without it, --app silently
    # degrades to a tab in an already-running browser instead of a window.
    ok("user-data-dir present specifically to force a fresh window",
       any("user-data-dir" in a for a in args))


# ====================================================================
# open_app_window: end-to-end decision, with everything faked
# ====================================================================


def test_open_app_window() -> None:
    print("-- open_app_window: platform and failure handling")
    from telemetry import launcher

    calls = {"popen": [], "webbrowser": []}

    def fake_popen_ok(args, **kwargs):
        calls["popen"].append(args)
        return mock.Mock()

    def fake_popen_fails(args, **kwargs):
        calls["popen"].append(args)
        raise OSError("no such file")

    def fake_webbrowser_open(url):
        calls["webbrowser"].append(url)
        return True

    with mock.patch.object(launcher, "platform") as m_platform, \
         mock.patch.object(launcher, "find_app_browser") as m_find, \
         mock.patch.object(launcher.subprocess, "Popen", side_effect=fake_popen_ok), \
         mock.patch.object(launcher.webbrowser, "open", side_effect=fake_webbrowser_open), \
         mock.patch("pathlib.Path.mkdir", return_value=None):

        # Windows, browser found: launches a dedicated process, no tab.
        m_platform.system.return_value = "Windows"
        m_find.return_value = (r"C:\Edge\msedge.exe", "Edge")
        calls["popen"].clear(); calls["webbrowser"].clear()

        result = launcher.open_app_window("http://x/", Path("/tmp/p"))
        check("reports a real app window", result, True)
        check("one process spawned", len(calls["popen"]), 1)
        check("no tab opened", len(calls["webbrowser"]), 0)

        # Windows, no browser found: falls back to a tab.
        m_find.return_value = None
        calls["popen"].clear(); calls["webbrowser"].clear()

        result = launcher.open_app_window("http://x/", Path("/tmp/p"))
        check("reports a fallback tab", result, False)
        check("nothing spawned", len(calls["popen"]), 0)
        check("tab opened instead", len(calls["webbrowser"]), 1)

        # Windows, browser found, but launching it fails (e.g. antivirus
        # blocked it, or the path from a stale registry entry is wrong):
        # must not raise, must still end up with a tab.
        m_find.return_value = (r"C:\Edge\msedge.exe", "Edge")
        with mock.patch.object(launcher.subprocess, "Popen", side_effect=fake_popen_fails):
            calls["popen"].clear(); calls["webbrowser"].clear()
            try:
                result = launcher.open_app_window("http://x/", Path("/tmp/p"))
                raised = False
            except Exception:  # noqa: BLE001
                raised = True

            ok("a launch failure does not raise", not raised)
            check("falls back to a tab on launch failure", result, False)
            check("tab opened after the failed spawn", len(calls["webbrowser"]), 1)

        # Not Windows: never even tries to find or spawn a browser.
        m_platform.system.return_value = "Linux"
        calls["popen"].clear(); calls["webbrowser"].clear()

        result = launcher.open_app_window("http://x/", Path("/tmp/p"))
        check("non-Windows reports a tab", result, False)
        check("non-Windows never spawns a process", len(calls["popen"]), 0)
        check("non-Windows opens a tab", len(calls["webbrowser"]), 1)


def main() -> int:
    print("Standalone app-window launcher\n")

    for fn in (test_find_app_browser, test_build_launch_args, test_open_app_window):
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            import traceback
            failures.append(f"{fn.__name__} raised: {exc}\n{traceback.format_exc()}")

    print(f"\n  {checks - len(failures)}/{checks} checks passed")

    if failures:
        print(f"\n  {len(failures)} FAILURE(S):\n")
        for f in failures:
            print(f"   - {f}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
