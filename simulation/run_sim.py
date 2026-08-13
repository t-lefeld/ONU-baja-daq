#!/usr/bin/env python3
"""
Entry point for the full-sensor-suite vehicle simulator.

    python run_sim.py                  # start on port 8766, open a browser
    python run_sim.py --port 8800      # different port
    python run_sim.py --no-browser     # just serve, don't open anything
    python run_sim.py --host 0.0.0.0   # let teammates view from their phones

Then pick any dashboard skin, e.g.:
    http://localhost:8766/static/themes/modern.html
    http://localhost:8766/static/themes/orange.html
    http://localhost:8766/static/themes/cyan.html
    http://localhost:8766/static/themes/black.html
    http://localhost:8766/static/themes/white.html
or the gallery landing page at http://localhost:8766/
"""

from __future__ import annotations

import argparse
import socket
import webbrowser

from aiohttp import web

from server import VehicleSimServer


def port_is_free(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
            return True
        except OSError:
            return False


def find_free_port(host: str, preferred: int, tries: int = 20) -> int:
    bind_host = "0.0.0.0" if host == "0.0.0.0" else host
    for offset in range(tries):
        candidate = preferred + offset
        if port_is_free(bind_host, candidate):
            return candidate
    return preferred


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=8766, help="default 8766, falls forward if busy")
    p.add_argument("--host", default="127.0.0.1", help="bind address; 0.0.0.0 to view from a phone")
    p.add_argument("--no-browser", action="store_true", help="don't open anything, just serve")
    args = p.parse_args()

    port = find_free_port(args.host, args.port)
    if port != args.port:
        print(f"port {args.port} busy, using {port} instead")

    sim = VehicleSimServer()
    app = sim.build_app()

    if not args.no_browser:
        open_host = "127.0.0.1" if args.host == "0.0.0.0" else args.host
        webbrowser.open(f"http://{open_host}:{port}/")

    print(f"Vehicle sim serving on http://{args.host}:{port}/")
    print("Single-page app with tabs: Live / Gauges / Schematic / Charts / Map / Session / Logs")

    web.run_app(app, host=args.host, port=port, print=None)


if __name__ == "__main__":
    main()
