# Display Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)
**CAN ID:** not yet assigned
**Location:** Driver-facing
**Role:** Shows live data to the driver

## What it is

A scrolling LED readout built from **four daisy-chained MAX7219 modules**,
driven over SPI from a Blue Pill. It **listens on the CAN bus and never
transmits** — it is a consumer of data, not a source of it. Nothing else on
the car depends on it, so a failure here costs the driver their readout and
nothing more.

**Status:** PCB and enclosure are done. Firmware is not yet in this repo.

## What it displays

General data for the driver — the exact channel set is still being decided.
Likely candidates are speed, battery voltage, and brake status, but treat
that as a starting point rather than a decision.

## Hardware

| Part | Count | Interface | Notes |
|------|-------|-----------|-------|
| MAX7219 LED matrix module | 4 | SPI, daisy-chained | Chained DOUT → DIN; the whole chain is one long shift register |
| STM32F103 Blue Pill | 1 | — | Same MCU as the corner nodes |
| SN65HVD230 CAN transceiver | 1 | — | Required; the Blue Pill's CAN peripheral has no transceiver on board |

Power comes from the firewall node like every other DAQ node — see
`nodes/firewall/README.md`.

## How this node came about

It started as a single Teensy driving a composite-video screen. That was
split into two MCUs — a "dumb" CAN/GPS node plus a separate video renderer —
so a video bug could not take down CAN and GPS. The video route was then
dropped entirely in favor of the MAX7219 chain, which is far simpler and
needs no video timing. The dino game that lived on the video plan went with
it.

GPS now lives on the firewall node, not here.

## Notes

- Four daisy-chained MAX7219s are addressed as one 32-bit shift register: you
  clock out all four devices' data in one burst, so a single-module update
  still shifts through the whole chain
- The MAX7219 has its own display brightness register — worth driving it from
  an ambient reading or a fixed daytime value, since the default is dim in
  sunlight
- Keep the SPI run short, or slow the clock; these modules are usually sold on
  ribbon cable with no termination
- Since this node only listens, set its CAN filter to accept just the IDs it
  renders rather than everything on the bus
- No `nodes/display/` entry existed before 2026-10-30; if you find display
  details contradicting this file elsewhere in the repo, this one is current
