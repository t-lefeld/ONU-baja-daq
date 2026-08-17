
# PlatformIO + Black Magic Probe workflow for this project

This replaces STM32CubeIDE + your ST-Link with **VS Code + PlatformIO + a
Black Pill flashed as a Black Magic Probe (BMP)**. Your `.ioc` files and all
CubeMX-generated source are untouched and don't care which probe/tool talks
to them - this is purely a different way to build/flash/debug the exact same
code.

I've already added a `platformio.ini` to each of your 4 active projects:

```
firmware/projects/1st Bluepill/platformio.ini
firmware/projects/2nd Bluepill/platformio.ini
firmware/projects/3rd Bluepill/platformio.ini
firmware/projects/Nucleo CAN Bus Test/platformio.ini
```

Each one points PlatformIO at that project's existing `Core/` and `Drivers/`
folders - the same source CubeIDE already builds - so nothing needed to be
regenerated from CubeMX.

## 1. Flash the Black Pill with Black Magic Probe firmware

No ST-Link needed for this - the STM32F411 has a native USB DFU bootloader.

1. Check your exact Black Pill board revision against the **supported boards
   list in the official Black Magic Probe firmware repo** before buying/flashing
   - BMP firmware is picky about exact hardware revisions.
2. Hold BOOT0, plug the Black Pill into your PC over USB - it enumerates as a
   DFU device.
3. Flash the BMP firmware:
   ```
   dfu-util -a 0 -s 0x08000000:leave -D blackmagic.bin
   ```
   (or ST's DfuSe tool on Windows if you prefer a GUI)
4. Power cycle without BOOT0 held. It now enumerates as **two COM/tty ports**:
   the first is the GDB server (used for flashing/debugging), the second is a
   plain UART passthrough.

## 2. Wire the BMP's SWD output to whichever target board

Same 4 signals as always - SWDIO, SWCLK, GND, 3.3V/target-voltage-sense - from
the Black Pill's SWD pins out to whichever Bluepill or the Nucleo you're
programming. This is still a real physical connection, so the connector
reliability fix (proper crimped/keyed cable, not loose jumpers) still applies
here exactly like it did with the ST-Link.

## 3. Install PlatformIO

VS Code → Extensions → search "PlatformIO IDE" → install. It pulls in the
`ststm32` platform automatically the first time you build a project that
uses it.

## 4. Open a project

In VS Code with PlatformIO installed, use **File → Open Folder** and point it
directly at one of:

```
firmware/projects/1st Bluepill
firmware/projects/2nd Bluepill
firmware/projects/3rd Bluepill
firmware/projects/Nucleo CAN Bus Test
```

PlatformIO detects the `platformio.ini` already sitting there and treats the
folder as a PlatformIO project - no `pio project init` needed since that file
already exists.

**Before your first build**, edit the `upload_port` line in that folder's
`platformio.ini` to match the BMP's actual GDB-server COM/tty port on your
system (check Device Manager on Windows, or `ls /dev/ttyACM*` on Linux/macOS).

## 5. Build / flash / debug

- **Build**: PlatformIO's checkmark icon in the VS Code status bar, or
  `pio run`.
- **Upload**: PlatformIO's right-arrow icon, or `pio run -t upload` - compiles
  and flashes over the BMP via GDB, no OpenOCD involved.
- **Debug**: PlatformIO's debug icon starts a GDB session through the BMP,
  breakpoints and all, directly in VS Code.

## 6. Daily loop

1. Need to change pin/peripheral config? Open the `.ioc` in CubeMX, adjust,
   Generate Code - same as always, still lands in `Core/`.
2. Write your logic inside the `/* USER CODE BEGIN */ ... /* USER CODE END */`
   blocks - CubeMX regeneration won't touch anything inside those.
3. Hit Upload in VS Code. Done - no separate OpenOCD step, no CubeProgrammer
   window, no switching to CubeIDE at all.

## Repo-specific notes

- **Only `Core/Inc/node_id.h` differs** between the 1st/2nd/3rd Bluepill
  projects - the `platformio.ini` in each is otherwise identical.
- **`4th Bluepill` was deleted 2026-08-16** - it was retired when the system
  went from 4 CAN nodes to 3, then removed outright once v2 bench testing
  confirmed 3 nodes was correct. Only three Bluepill projects remain.
- **`src_dir = .` must live in a `[platformio]` section**, not inside
  `[env:...]` - it's silently ignored there, which looks like the setting
  "didn't do anything" if you miss this.
- **The vendored `Drivers/` folder, `Core/Startup`, and
  `Core/Src/system_*.c` are deliberately excluded from the PlatformIO build**
  (they're still used by the CubeIDE build, untouched). PlatformIO's
  `stm32cube` framework always compiles its own bundled copies of the HAL
  driver, CMSIS device files, and startup file as separate framework
  libraries, regardless of `build_src_filter` - so if this project's own
  vendored copies of those are also included, you get "multiple definition"
  linker errors for every HAL function. Each `platformio.ini` now only
  supplies this project's actual application code
  (`Core/Src/main.c`, `can_node.c`/`telemetry_hub.c`, `telemetry_proto.c`,
  `stm32*xx_hal_msp.c`, `stm32*xx_it.c`, `syscalls.c`, `sysmem.c`) plus
  `Core/Inc` headers - critically including this project's own
  `stm32f1xx_hal_conf.h`/`stm32l4xx_hal_conf.h`, which correctly overrides
  the framework's generic template because `Core/Inc` is first on the
  include path. The linker script's `-I Drivers/...` flags were removed for
  the same reason: the framework's own HAL/CMSIS headers now supply those
  declarations, matching the framework's compiled `.o` files exactly.
- The linker scripts (`STM32F103C8TX_FLASH.ld` / `STM32L476RGTX_FLASH.ld`)
  had `(READONLY)` removed from 5 section definitions - that keyword needs
  GCC11+, and PlatformIO's bundled toolchain is older.
- I couldn't compile-test any of this myself (no ARM toolchain in my
  sandbox), so this reflects the actual errors hit and fixed while working
  through it live with you, not a pre-verified template.
