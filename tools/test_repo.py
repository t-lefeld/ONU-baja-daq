#!/usr/bin/env python3
"""
Structural audit of the repository.

    python tools/test_repo.py

The other suites prove the code is correct. This one proves the right code is
in the right place - which is a separate risk, because the shared protocol and
driver sources are *copied* into CubeIDE projects. Copies drift. Edit
protocol/telemetry_proto.c, forget to re-copy, and the active boards keep
building against a stale header that still compiles and produces subtly wrong
frames.

Note on "4th Bluepill": the system was redesigned from 4 CAN nodes down to 3
(TLM_NODE_COUNT == 3). That project directory is intentionally left as-is,
NODE_ID == 3 and all - it is retired, not synced, and will fail to compile
with the current protocol/telemetry_proto.h if anyone tries (NODE_ID would
exceed TLM_NODE_COUNT-1). That is deliberate: it stops a forgotten 4th board
from ever being flashed and colliding on the bus.

Also checks the things a fresh clone needs to be true: every project has its
files, node IDs are unique and correctly ordered, the .ioc edits are present,
main.c actually calls the firmware, and none of the old code was left behind
to collide at link time.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJ = ROOT / "firmware" / "projects"

BLUEPILLS = ["1st Bluepill", "2nd Bluepill", "3rd Bluepill"]
RETIRED_BLUEPILL = "4th Bluepill"  # dropped when the system went to 3 nodes; not synced
NUCLEO = "Nucleo CAN Bus Test"

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


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


# --------------------------------------------------------------------


def test_copies_match() -> None:
    """Every vendored copy must be byte-identical to its canonical source."""
    print("-- shared sources: copies match canonical")

    shared = {
        "telemetry_proto.h": ROOT / "protocol" / "telemetry_proto.h",
        "telemetry_proto.c": ROOT / "protocol" / "telemetry_proto.c",
    }
    node = {
        "can_node.h": ROOT / "firmware" / "bluepill_node" / "can_node.h",
        "can_node.c": ROOT / "firmware" / "bluepill_node" / "can_node.c",
    }
    hub = {
        f"{stem}.{ext}": ROOT / "firmware" / "nucleo_hub" / f"{stem}.{ext}"
        for stem in ("telemetry_hub", "lora_e22", "sd_log", "sd_spi", "fat32")
        for ext in ("h", "c")
    }

    def compare(project: str, mapping: dict[str, Path]) -> None:
        for name, canonical in mapping.items():
            sub = "Inc" if name.endswith(".h") else "Src"
            copy = PROJ / project / "Core" / sub / name
            if not copy.exists():
                failures.append(f"{project}: missing Core/{sub}/{name}")
                globals()["checks"] += 1
                continue
            ok(f"{project}: Core/{sub}/{name} matches canonical",
               copy.read_bytes() == canonical.read_bytes())

    for p in BLUEPILLS:
        compare(p, shared)
        compare(p, node)

    compare(NUCLEO, shared)
    compare(NUCLEO, hub)

    # The Bluepill main.c is generated from one template; all three active
    # boards must agree. The retired 4th is not checked against the template
    # or the canonical sources - see the module docstring.
    template = (ROOT / "firmware" / "bluepill_node" / "main_template.c").read_bytes()
    for p in BLUEPILLS:
        ok(f"{p}: main.c matches the template",
           (PROJ / p / "Core" / "Src" / "main.c").read_bytes() == template)

    ok(f"{RETIRED_BLUEPILL}: still present but intentionally unsynced",
       (PROJ / RETIRED_BLUEPILL / "Core" / "Inc" / "node_id.h").exists())


def test_node_ids() -> None:
    print("-- node identity: unique and in order")

    seen = {}
    for i, p in enumerate(BLUEPILLS):
        header = PROJ / p / "Core" / "Inc" / "node_id.h"
        ok(f"{p}: node_id.h exists", header.exists())
        if not header.exists():
            continue

        m = re.search(r"^#define\s+NODE_ID\s+(\d+)\s*$", read(header), re.M)
        ok(f"{p}: node_id.h defines NODE_ID", m is not None)
        if not m:
            continue

        value = int(m.group(1))
        check(f"{p}: NODE_ID", value, i)
        ok(f"{p}: NODE_ID {value} not already used", value not in seen)
        seen[value] = p

    check("all three node IDs present", sorted(seen), [0, 1, 2])

    # The Nucleo must NOT have one - it is the receiver, and a stray node_id.h
    # there would mean someone copied the wrong file set in.
    ok("Nucleo has no node_id.h",
       not (PROJ / NUCLEO / "Core" / "Inc" / "node_id.h").exists())


def test_ioc() -> None:
    print("-- .ioc: required keys present")

    for p in BLUEPILLS:
        text = read(PROJ / p / f"{p}.ioc")
        for key, want in [
            ("CAN.Prescaler", "9"),
            ("CAN.BS1", "CAN_BS1_6TQ"),
            ("CAN.BS2", "CAN_BS2_1TQ"),
            ("CAN.ABOM", "ENABLE"),
            ("PA13.Mode", "Serial_Wire"),
            ("PA14.Mode", "Serial_Wire"),
        ]:
            m = re.search(rf"^{re.escape(key)}=(.*)$", text, re.M)
            check(f"{p}: {key}", m.group(1) if m else None, want)

        ok(f"{p}: No_Debug removed", "VP_SYS_VS_ND" not in text)
        ok(f"{p}: CRLF line endings preserved",
           (PROJ / p / f"{p}.ioc").read_bytes().count(b"\r\n") > 100)

        # Pin list length must match the declared count, or CubeMX complains.
        pins = re.findall(r"^Mcu\.Pin\d+=", text, re.M)
        n = re.search(r"^Mcu\.PinsNb=(\d+)$", text, re.M)
        check(f"{p}: PinsNb matches pin entries", len(pins), int(n.group(1)) if n else -1)

    text = read(PROJ / NUCLEO / f"{NUCLEO}.ioc")
    for key, want in [
        ("CAN1.ABOM", "ENABLE"),
        ("USART1.BaudRate", "9600"),
        ("Dma.Request0", "USART1_TX"),
        ("Dma.USART1_TX.0.Instance", "DMA1_Channel4"),
        ("PA9.Signal", "USART1_TX"),
        ("PA10.Signal", "USART1_RX"),
        ("PB0.GPIO_Label", "E22_M0"),
        ("PA4.GPIO_Label", "E22_M1"),
        ("PA1.GPIO_Label", "E22_AUX"),
        ("PB6.GPIO_Label", "SD_CS"),
        ("PB6.PinState", "GPIO_PIN_SET"),
    ]:
        m = re.search(rf"^{re.escape(key)}=(.*)$", text, re.M)
        check(f"Nucleo: {key}", m.group(1) if m else None, want)

    for irq in ("CAN1_RX0_IRQn", "USART1_IRQn", "DMA1_Channel4_IRQn"):
        ok(f"Nucleo: NVIC.{irq} enabled",
           re.search(rf"^NVIC\.{irq}=true", text, re.M) is not None)

    pins = re.findall(r"^Mcu\.Pin\d+=", text, re.M)
    n = re.search(r"^Mcu\.PinsNb=(\d+)$", text, re.M)
    check("Nucleo: PinsNb matches pin entries", len(pins), int(n.group(1)) if n else -1)

    # SPI1 is driven at register level; adding it to the .ioc would generate a
    # competing MX_SPI1_Init.
    ok("Nucleo: SPI1 deliberately absent from .ioc", "SPI1" not in text)


def test_main_wiring() -> None:
    print("-- main.c: firmware actually called")

    for p in BLUEPILLS:
        text = read(PROJ / p / "Core" / "Src" / "main.c")
        ok(f"{p}: includes can_node.h", '#include "can_node.h"' in text)
        ok(f"{p}: calls can_node_init", "can_node_init(&hcan);" in text)
        ok(f"{p}: calls can_node_task", "can_node_task();" in text)
        ok(f"{p}: CAN init matches the .ioc",
           "hcan.Init.Prescaler = 9;" in text
           and "hcan.Init.TimeSeg2 = CAN_BS2_1TQ;" in text
           and "hcan.Init.AutoBusOff = ENABLE;" in text)

        # Old firmware must be gone or it collides at link time.
        for stale in ("CAN_Sender_Init", "CAN_Sender_Loop", "simulate_sensor"):
            ok(f"{p}: old {stale} removed", stale not in text)

        msp = read(PROJ / p / "Core" / "Src" / "stm32f1xx_hal_msp.c")
        ok(f"{p}: SWD left enabled", "__HAL_AFIO_REMAP_SWJ_NOJTAG();" in msp)
        ok(f"{p}: SWJ_DISABLE removed", "__HAL_AFIO_REMAP_SWJ_DISABLE" not in msp)

    text = read(PROJ / NUCLEO / "Core" / "Src" / "main.c")
    ok("Nucleo: includes telemetry_hub.h", '#include "telemetry_hub.h"' in text)
    ok("Nucleo: calls hub_init", "hub_init(&hcan1, &huart1, &huart2);" in text)
    ok("Nucleo: calls hub_task", "hub_task();" in text)
    ok("Nucleo: AutoBusOff enabled", "hcan1.Init.AutoBusOff = ENABLE;" in text)
    ok("Nucleo: USART1 at 9600", "huart1.Init.BaudRate = 9600;" in text)
    for fn in ("MX_DMA_Init();", "MX_USART1_UART_Init();"):
        ok(f"Nucleo: calls {fn}", fn in text)
    for stale in ("CAN_Receiver_Init", "== CAN logger ready =="):
        ok(f"Nucleo: old {stale} removed", stale not in text)

    msp = read(PROJ / NUCLEO / "Core" / "Src" / "stm32l4xx_hal_msp.c")
    ok("Nucleo: USART1 MSP present", "huart->Instance==USART1" in msp)
    ok("Nucleo: DMA linked to USART1", "__HAL_LINKDMA(huart,hdmatx,hdma_usart1_tx)" in msp)
    ok("Nucleo: DMA request 2", "DMA_REQUEST_2" in msp)

    it = read(PROJ / NUCLEO / "Core" / "Src" / "stm32l4xx_it.c")
    for handler in ("CAN1_RX0_IRQHandler", "USART1_IRQHandler", "DMA1_Channel4_IRQHandler"):
        ok(f"Nucleo: {handler} defined", f"void {handler}(void)" in it)

    # The hub's RX callback lives in telemetry_hub.c; a duplicate in main.c or
    # it.c would be a link error - better to catch it here.
    for f in ("main.c", "stm32l4xx_it.c"):
        body = read(PROJ / NUCLEO / "Core" / "Src" / f)
        ok(f"Nucleo: no duplicate RX callback in {f}",
           "HAL_CAN_RxFifo0MsgPendingCallback" not in body)


def test_originals_untouched() -> None:
    print("-- your original projects: untouched")

    src = Path("/sessions/modest-hopeful-faraday/mnt")
    if not (src / "1st Bluepill").is_dir():
        print("     (skipped: original projects not mounted)")
        return

    for name in BLUEPILLS + [NUCLEO]:
        original = src / name / "Core" / "Src" / "main.c"
        if not original.exists():
            continue
        text = read(original)
        ok(f"{name}: original main.c has no new firmware calls",
           "can_node_task" not in text and "hub_task" not in text)


def test_pc_app() -> None:
    print("-- ground station: files present")

    for rel in [
        "pc_app/run.py", "pc_app/build.bat", "pc_app/telemetry.spec",
        "pc_app/requirements.txt", "pc_app/static/index.html",
        "pc_app/telemetry/__init__.py", "pc_app/telemetry/proto.py",
        "pc_app/telemetry/paths.py", "pc_app/telemetry/settings.py",
        "pc_app/telemetry/sources.py", "pc_app/telemetry/server.py",
        "pc_app/telemetry/recorder.py", "pc_app/telemetry/launcher.py",
        "pc_app/telemetry/pump.py", "pc_app/telemetry/qt_app.py",
        "pc_app/make_shortcut.bat",
        "protocol/telemetry.dbc",
    ]:
        ok(f"{rel} exists", (ROOT / rel).exists())

    spec = read(ROOT / "pc_app" / "telemetry.spec")
    ok("spec bundles static/", "datas=[('static', 'static')]" in spec)
    ok("spec keeps the console", "console=True" in spec)
    ok("spec includes list_ports hidden import", "serial.tools.list_ports" in spec)
    ok("spec excludes QtWebEngine (no embedded Chromium, ever)",
       "PySide6.QtWebEngineCore" in spec and "PySide6.QtWebEngineWidgets" in spec)
    ok("spec no longer blanket-excludes PySide6 itself",
       "'PySide6',\n" not in spec)

    requirements = read(ROOT / "pc_app" / "requirements.txt")
    ok("requirements installs PySide6-Essentials, not the Addons-pulling "
       "umbrella package", "PySide6-Essentials" in requirements)
    ok("requirements does not ask for the full PySide6 umbrella package",
       "\nPySide6>=" not in requirements and "\nPySide6\n" not in requirements)

    build_bat_deps = read(ROOT / "pc_app" / "build.bat")
    ok("build.bat installs PySide6-Essentials rather than the umbrella package",
       "PySide6-Essentials" in build_bat_deps)

    html = read(ROOT / "pc_app" / "static" / "index.html")
    # Strip data: URIs before checking. The favicon is an inline SVG whose
    # content is only partly percent-encoded (single quotes and spaces are
    # left literal, a common shorthand for inline SVG data URIs), so its own
    # xmlns declaration - http://www.w3.org/2000/svg - appears as unencoded
    # text. Stop at the attribute's closing double quote, not the first
    # space or single quote, or the strip barely gets past "image/svg+xml,".
    no_data_uris = re.sub(r'data:[^"]*', '', html)
    ok("UI loads no external http(s) resources",
       "http://" not in no_data_uris and "https://" not in no_data_uris)

    # WebSocket message dispatch (meta/status/history/frame) now lives in the
    # shared module every themed page includes, not inline in index.html -
    # that is the whole point of factoring it out, so check there instead.
    core_js = read(ROOT / "pc_app" / "static" / "js" / "telemetry-core.js")
    ok("UI handles the status message", '"status"' in html or '"status"' in core_js)
    ok("UI has its own icon (for the app-window taskbar entry)",
       'rel="icon"' in html)

    run_py = read(ROOT / "pc_app" / "run.py")
    ok("run.py opens the native PySide6 window by default",
       "run_qt_app" in run_py and "run_native" in run_py)
    ok("run.py has a --web escape hatch to the browser dashboard",
       '"--web"' in run_py)
    ok("run.py's --web path still supports the standalone app-window launcher",
       "open_app_window" in run_py)
    ok("run.py has a --tab escape hatch back to a normal browser tab (--web only)",
       '"--tab"' in run_py)

    build_bat = read(ROOT / "pc_app" / "build.bat")
    ok("build.bat creates the Desktop shortcut", "make_shortcut.bat" in build_bat)

    shortcut_bat = read(ROOT / "pc_app" / "make_shortcut.bat")
    ok("shortcut script points at the built exe", "dist\\Telemetry.exe" in shortcut_bat)
    ok("shortcut script targets the Desktop", "Desktop\\Telemetry.lnk" in shortcut_bat)


def main() -> int:
    print("Repository audit\n")

    for fn in (test_copies_match, test_node_ids, test_ioc, test_main_wiring,
               test_originals_untouched, test_pc_app):
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
