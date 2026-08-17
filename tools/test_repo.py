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
(TLM_NODE_COUNT == 3). The project directory for it was deleted outright on
2026-08-16 (Tate's call - he wanted it gone rather than kept around as a
retired/unsynced guardrail) once the v2 bench tests confirmed the 3-node
system works without it. There is no board left to accidentally flash, so
there's nothing here to check for it anymore.

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
NUCLEO = "Nucleo CAN Bus Test"

# v1 -> v2 cutover (see HANDOFF.md): Front/Rear + the hub now run the v2
# protocol with simulated channel values. v1 files themselves are untouched
# either way - this split is only about which project's main.c calls what.
#
# TEMPORARY, 2026-08-14: the 1st Bluepill (v1 NODE_ID 0, otherwise idle in
# v2 - node 0 is the Hub itself, node 3 has no physical board in the final
# design) is standing in for node 3 (E-CVT/Motor) for the v2 bench test, so
# the hub's reassembler gets exercised by a real 4th CAN transmitter. It is
# NOT a v1 fallback spare while wearing this hat. See can_node_v2.h and
# HUB2_SIMULATE_NODE3 in telemetry_hub_v2.h.
V1_ONLY_BLUEPILLS: list[str] = []
V2_BLUEPILLS = ["2nd Bluepill", "3rd Bluepill"]
MOTOR_STANDIN_BLUEPILL = "1st Bluepill"
MOTOR_STANDIN_NODE_ID = 3

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
    shared_v2 = {
        "telemetry_proto_v2.h": ROOT / "protocol" / "telemetry_proto_v2.h",
        "telemetry_proto_v2.c": ROOT / "protocol" / "telemetry_proto_v2.c",
    }
    node_v2 = {
        "can_node_v2.h": ROOT / "firmware" / "bluepill_node" / "can_node_v2.h",
        "can_node_v2.c": ROOT / "firmware" / "bluepill_node" / "can_node_v2.c",
    }
    hub_v2 = {
        "telemetry_hub_v2.h": ROOT / "firmware" / "nucleo_hub" / "telemetry_hub_v2.h",
        "telemetry_hub_v2.c": ROOT / "firmware" / "nucleo_hub" / "telemetry_hub_v2.c",
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

    # v1's mirrored copies stay present and byte-identical everywhere,
    # cut over to v2 or not - that's what "never modify v1 files" buys you:
    # the spare Bluepill (or a hardware regression) can always fall back to
    # them without resyncing anything.
    for p in BLUEPILLS:
        compare(p, shared)
        compare(p, node)
    compare(NUCLEO, shared)
    compare(NUCLEO, hub)

    # v2's mirrored copies, everywhere a v2 firmware image actually gets
    # built from them - including the motor stand-in, which is a real v2
    # Bluepill build (NODE_ID 3) even though its role is temporary.
    all_v2_bluepills = V2_BLUEPILLS + [MOTOR_STANDIN_BLUEPILL]
    for p in all_v2_bluepills:
        compare(p, shared_v2)
        compare(p, node_v2)
    compare(NUCLEO, shared_v2)
    compare(NUCLEO, hub_v2)

    # The Bluepill main.c is generated from one template; a v1-only spare
    # (none right now - see V1_ONLY_BLUEPILLS) would need to match it
    # exactly. The retired 4th is not checked against the template or the
    # canonical sources - see the module docstring.
    template = (ROOT / "firmware" / "bluepill_node" / "main_template.c").read_bytes()
    for p in V1_ONLY_BLUEPILLS:
        ok(f"{p}: main.c matches the template",
           (PROJ / p / "Core" / "Src" / "main.c").read_bytes() == template)

    # All three active Bluepills got the same mechanical v2 edit (can_node.h
    # -> can_node_v2.h, can_node_init/task -> can_node_v2_init/task) applied
    # to the same starting template - node identity lives entirely in
    # node_id.h, not main.c. Through the end of v2 bench testing this meant
    # all three main.c files agreed byte-for-byte, since none of them had
    # any board-specific peripherals yet.
    #
    # UPDATED 2026-08-16 for v3: that byte-identity assumption breaks by
    # design once real per-board sensors get wired in - CubeMX regenerates
    # main.c's MX_*_Init() calls to match whatever peripherals THAT board's
    # .ioc actually has (Front's ADC1_IN6/IN7 pressure transducers, Rear's
    # I2C1 CVT thermistor, etc.), so Front/Rear/motor-standin main.c files
    # are now expected to diverge. The real invariant - that the v2 cutover
    # (can_node_v2.h include, v2 init/task calls, CAN timing matching the
    # .ioc) was applied correctly to each board - is already checked above,
    # per-board, independent of whether the boards match each other. This
    # byte-identity check is intentionally gone; don't re-add it as boards
    # keep diverging through the rest of v3 bring-up.



def test_node_ids() -> None:
    print("-- node identity: unique and in order")

    # TEMPORARY, 2026-08-14: the 1st Bluepill's NODE_ID is 3 (motor stand-in),
    # not its usual 0 (index in BLUEPILLS) - see MOTOR_STANDIN_BLUEPILL.
    expected = {p: (MOTOR_STANDIN_NODE_ID if p == MOTOR_STANDIN_BLUEPILL else i)
                for i, p in enumerate(BLUEPILLS)}

    seen = {}
    for p in BLUEPILLS:
        header = PROJ / p / "Core" / "Inc" / "node_id.h"
        ok(f"{p}: node_id.h exists", header.exists())
        if not header.exists():
            continue

        m = re.search(r"^#define\s+NODE_ID\s+(\d+)\s*$", read(header), re.M)
        ok(f"{p}: node_id.h defines NODE_ID", m is not None)
        if not m:
            continue

        value = int(m.group(1))
        check(f"{p}: NODE_ID", value, expected[p])
        ok(f"{p}: NODE_ID {value} not already used", value not in seen)
        seen[value] = p

    check("all node IDs present (0 vacated by the motor stand-in, 1/2/3 in use)",
          sorted(seen), [1, 2, 3])

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

    for p in V1_ONLY_BLUEPILLS:
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

    # v1's can_node.c hard-errors at compile time if NODE_ID is outside
    # 0..TLM_NODE_COUNT-1 (0-2) - true regardless of whether can_node.c's
    # functions are ever called, since PlatformIO's build_src_filter compiles
    # every file under Core/Src/ on its own. The motor stand-in's NODE_ID (3)
    # is out of that range, so its platformio.ini MUST exclude can_node.c or
    # the PlatformIO build fails even though the CubeIDE build (which doesn't
    # need this exclusion - can_node.c has no colliding symbols, it's just
    # dead code there) is fine. This bit Tate on 2026-08-14 - see HANDOFF.md.
    pio = read(PROJ / MOTOR_STANDIN_BLUEPILL / "platformio.ini")
    ok(f"{MOTOR_STANDIN_BLUEPILL}: platformio.ini excludes v1 can_node.c "
       f"(NODE_ID {MOTOR_STANDIN_NODE_ID} is out of v1's valid range)",
       "-<Core/Src/can_node.c>" in pio)

    for p in V2_BLUEPILLS + [MOTOR_STANDIN_BLUEPILL]:
        text = read(PROJ / p / "Core" / "Src" / "main.c")
        ok(f"{p}: includes can_node_v2.h", '#include "can_node_v2.h"' in text)
        ok(f"{p}: calls can_node_v2_init", "can_node_v2_init(&hcan);" in text)
        ok(f"{p}: calls can_node_v2_task", "can_node_v2_task();" in text)
        # v1's can_node.c is still mirrored in but no longer called - a stale
        # include or call here would mean the cutover was only half-done.
        ok(f"{p}: v1 can_node.h not included", '#include "can_node.h"' not in text)
        ok(f"{p}: v1 can_node_init not called", "can_node_init(&hcan);" not in text)
        ok(f"{p}: v1 can_node_task not called", "can_node_task();" not in text)
        ok(f"{p}: CAN init matches the .ioc",
           "hcan.Init.Prescaler = 9;" in text
           and "hcan.Init.TimeSeg2 = CAN_BS2_1TQ;" in text
           and "hcan.Init.AutoBusOff = ENABLE;" in text)

        for stale in ("CAN_Sender_Init", "CAN_Sender_Loop", "simulate_sensor"):
            ok(f"{p}: old {stale} removed", stale not in text)

        msp = read(PROJ / p / "Core" / "Src" / "stm32f1xx_hal_msp.c")
        ok(f"{p}: SWD left enabled", "__HAL_AFIO_REMAP_SWJ_NOJTAG();" in msp)
        ok(f"{p}: SWJ_DISABLE removed", "__HAL_AFIO_REMAP_SWJ_DISABLE" not in msp)

    text = read(PROJ / NUCLEO / "Core" / "Src" / "main.c")
    ok("Nucleo: includes telemetry_hub_v2.h", '#include "telemetry_hub_v2.h"' in text)
    ok("Nucleo: calls hub2_init", "hub2_init(&hcan1, &huart1, &huart2);" in text)
    ok("Nucleo: calls hub2_task", "hub2_task();" in text)
    # TEMPORARY, 2026-08-14: the 1st Bluepill is transmitting real CAN pages
    # for node 3 (motor stand-in) during the v2 bench test, so the hub must
    # not ALSO fabricate node 3 locally - both would write s_reasm[3] and
    # interleave two epochs into one torn burst.
    #
    # CORRECTED, 2026-08-16: this MUST be a compiler -D flag, not a #define in
    # main.c - found the hard way on the bench, where a main.c #define here
    # silently did nothing for weeks. main.c and telemetry_hub_v2.c are
    # separate translation units; telemetry_hub_v2.c does its own #include of
    # telemetry_hub_v2.h, which never sees a #define placed in main.c, so the
    # header's #ifndef default (1, simulate) always won regardless of what
    # main.c said. The old regex check for a main.c #define is deliberately
    # gone - that pattern is now known-wrong and should never come back.
    # Checking both build systems' actual -D mechanism instead.
    ok("Nucleo: main.c does NOT use the broken main.c-#define pattern for "
       "HUB2_SIMULATE_NODE3 (silently no-ops - see telemetry_hub_v2.h)",
       not re.search(r"#define\s+HUB2_SIMULATE_NODE3\s+0", text))
    pio_nucleo = read(PROJ / NUCLEO / "platformio.ini")
    ok("Nucleo: platformio.ini defines HUB2_SIMULATE_NODE3=0 as a build flag "
       "(motor stand-in is on the bus)",
       "-D HUB2_SIMULATE_NODE3=0" in pio_nucleo)
    cproject = read(PROJ / NUCLEO / ".cproject")
    ok("Nucleo: .cproject defines HUB2_SIMULATE_NODE3=0 as a preprocessor "
       "symbol (motor stand-in is on the bus)",
       cproject.count('value="HUB2_SIMULATE_NODE3=0"') >= 2)
    # v1's telemetry_hub.c is still mirrored in (untouched, per the "never
    # modify v1 files" rule) but must be excluded from the build - both files
    # define HAL_CAN_RxFifo0MsgPendingCallback as a strong symbol, so linking
    # both in is a duplicate-symbol error, and main.c must not call the v1
    # entry points either.
    ok("Nucleo: v1 telemetry_hub.h not included",
       '#include "telemetry_hub.h"' not in text)
    ok("Nucleo: v1 hub_init not called",
       "hub_init(&hcan1, &huart1, &huart2);" not in text)
    ok("Nucleo: v1 hub_task not called", "hub_task();" not in text)
    ok("Nucleo: .cproject excludes v1 telemetry_hub.c from the build",
       cproject.count('excluding="Core/Src/telemetry_hub.c"') >= 2)

    # PlatformIO is a SEPARATE build path from CubeIDE's .cproject - it has
    # its own build_src_filter and does not read .cproject at all, so the
    # same exclusion has to be repeated there or PlatformIO happily compiles
    # (and links, and fails) both telemetry_hub.c and telemetry_hub_v2.c.
    # This bit Tate on 2026-08-14 - see HANDOFF.md.
    pio = read(PROJ / NUCLEO / "platformio.ini")
    ok("Nucleo: platformio.ini excludes v1 telemetry_hub.c from the build",
       "-<Core/Src/telemetry_hub.c>" in pio)

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

    # The hub's RX callback lives in telemetry_hub_v2.c (now the only one of
    # the pair actually compiled in); a duplicate definition in main.c or
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
    # XML/SVG namespace URIs (xmlns="http://www.w3.org/2000/svg" and the
    # same string passed to document.createElementNS in inline <script>) are
    # inert identifiers required by the SVG spec, not network fetches - an
    # inline <svg viewBox=...> schematic (added 2026-08-16) needs one and
    # isn't a wifi dependency. Strip known-safe namespace URIs specifically,
    # rather than every "http://" substring, so an actual <link>/<script
    # src>/@import/fetch() to a real origin still fails this check.
    SAFE_NS_URIS = (
        "http://www.w3.org/2000/svg",
        "http://www.w3.org/1999/xhtml",
        "http://www.w3.org/1999/xlink",
    )
    checked = no_data_uris
    for uri in SAFE_NS_URIS:
        checked = checked.replace(uri, "")
    ok("UI loads no external http(s) resources",
       "http://" not in checked and "https://" not in checked)

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
