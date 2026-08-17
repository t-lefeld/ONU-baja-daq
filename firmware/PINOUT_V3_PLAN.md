# v3 pin assignments — history / working notes

**Superseded 2026-08-16 as the pin source of truth by
`V3_CUBEMX_AND_SENSOR_INTEGRATION_GUIDE.md` (repo root), which now has the
corrected Front/Rear pins from the real PCB plus full CubeMX config detail
and calibration steps in one place. Use that file, not this one, when
actually wiring things up.** This file is kept as the working log of how
those pins got decided (the original suggestions, then the real PCB
screenshots, then the CVT I2C pin problem) — useful history, not the
current plan.

---

Written 2026-08-16. This turns `PINOUT.md`'s "suggestions, not settings"
into actual chosen pins, so there's a concrete checklist to work from board
by board. **Nothing here is applied to any `.ioc` yet** — per the standing
rule in `PINOUT.md` and `HANDOFF.md`, `.ioc` files only get touched through
the STM32CubeMX GUI (Generate Code), never hand-edited. This file is the
plan you follow while doing that; it isn't a substitute for it.

Every pin below is picked to avoid what's already assigned (see
`PINOUT.md`'s "Currently assigned" tables) and the EXTI-sharing trap on the
Bluepills (two GPIOs with the same pin *number*, e.g. PA0 and PB0, share one
EXTI line — only one can fire).

You don't have to use these exact pins — swap anything that conflicts with
your actual protoboard/breakout layout. What matters is keeping the two
wheel encoders on different pin *numbers* from each other, and staying off
the pins already listed as taken.

---

## Hub (Nucleo-L476RG) — node 0, GPS + IMU

Already taken (do not reuse): PA1, PA2, PA3, PA4, PA5, PA6, PA7, PA9, PA10,
PA11, PA12, PB0, PB6.

| Sensor | Peripheral | Pins | CubeMX steps |
|---|---|---|---|
| NEO-M8N GPS | USART3, RX only (module TX → MCU RX; module's RX line can be left unconnected since this is read-only NMEA) | PC5 = USART3_RX (PC4 = USART3_TX, wire it up too even if unused, costs nothing) | Enable USART3 in Asynchronous mode, default 9600 baud (NEO-M8N's power-on default — raise it later if you reconfigure the module), Generate Code |
| BNO080 IMU | I2C1 | PB8 = I2C1_SCL, PB9 = I2C1_SDA, PB3 = RST (GPIO output), PB4 = INT (GPIO input, EXTI optional — `imu_bno080.c` can also be polled) | Enable I2C1 (standard 100kHz to start), set PB3 as GPIO_Output, PB4 as GPIO_Input (or GPIO_EXTI_Falling if you want interrupt-driven reads), Generate Code |

Confirm PB3/PB4 are actually broken out on your Nucleo's headers before
committing to them — Nucleo-64 boards expose the full Morpho connector, so
they should be, but double check against the physical board in hand.

## Front and Rear Bluepill — nodes 1 and 2

**Superseded 2026-08-16 — these are Tate's actual PCB pin assignments**
(from the "Firewall PCB" schematic, one shared board design populated two
ways: Front gets the pressure-transducer header, Rear gets the CVT
thermistor header, everything else — encoders, suspension — is identical
on both). Replaces the PB0/PB1/PA0-2 suggestions this section used to have.

Already taken (do not reuse): PA11, PA12, PA13, PA14.

| Sensor | Peripheral | Pins | Board | CubeMX steps |
|---|---|---|---|---|
| Wheel encoder L | GPIO + EXTI | PA0 | Front + Rear | GPIO_EXTI_Rising (or Falling, match your encoder's output), Generate Code |
| Wheel encoder R | GPIO + EXTI | PA1 | Front + Rear | **Different pin number from L, satisfies the EXTI-sharing rule** — PA0/PA1 are separate EXTI lines |
| Suspension pot L | ADC1 (IN4) | PA4 | Front + Rear | Enable ADC1, IN4 channel |
| Suspension pot R | ADC1 (IN5) | PA5 | Front + Rear | Same ADC1, IN5 channel — remember `suspension_pot.c`'s `TODO` about re-selecting the channel via `HAL_ADC_ConfigChannel()` before each read, since it shares the ADC with the encoders' neighbors |
| Pressure transducer 1 (primary) | ADC1 (IN6) | PA6 | **Front only** | Same ADC1, IN6. Needs the resistor divider from `pressure_transducer.h`'s `TODO` to bring 0.5-4.5V into 3.3V range |
| Pressure transducer 2 (redundant/backup) | ADC1 (IN7) | PA7 | **Front only** | Same sensor reading as transducer 1 — read it, but treat transducer 1 as the value that goes on the wire unless it looks wrong. Worth deciding now: average both, or fail over to #2 only if #1 reads implausibly (stuck/out of range)? |
| CVT thermistor (MLX90614, I2C, address 0x5A) | I2C1 | **PB7/PB8 as currently laid out is NOT a valid hardware I2C1 pair on the F103** (valid pairs are PB6+PB7 default, or PB8+PB9 remapped) | **Rear only** | Tate is fixing this on the PCB — simplest fix is rerouting just the SCL trace from PB8 to PB6 (keeps PB7/SDA as already wired, uses I2C1 in its default, non-remapped mode, no AFIO remap needed in firmware). Update this row once the board's corrected. |

## Motor / E-CVT — node 3

No pins at all — the ODrive S1 is its own CAN node on the same bus the hub
already listens to. `odrive_can.c` reads it from the existing CAN peripheral.

The one thing to lock down: the ODrive's `axis_node_id` (set via `odrivetool`
or the ODrive GUI, not in this codebase) **must stay below 16**, or its CAN
traffic collides with the `0x200-0x21F` block v2 already uses for node
pages — see `protocol/V2_DESIGN_NOTES.md`'s collision writeup.
`HUB2_ODRIVE_AXIS_NODE_ID` in `telemetry_hub_v2.h` defaults to `0`
(ODrive's own factory default) and the hub firmware refuses to boot
(`Error_Handler()`) if it's ever set to exactly 16, as a guardrail — but
nothing catches 1-15 conflicting with something else on the bus, so just
leave the ODrive at its default `axis_node_id=0` unless you have a specific
reason to change it, and if you do change it, update
`HUB2_ODRIVE_AXIS_NODE_ID` to match before flashing.

---

## Order of work per board (from `PINOUT.md`, repeated here for convenience)

1. Open that board's `.ioc` in STM32CubeMX.
2. Assign the peripherals from the table above, **Generate Code**.
3. Verify against `firmware/CUBEMX_SETUP.md`'s checks (catches the
   silently-dropped-key failure mode).
4. Fill in the `TODO:` constants in the matching `firmware/sensors/*.h` file
   (calibration constants, exact pin macros CubeMX generated).
5. Tell me which sensor's ready and I'll wire its driver call into
   `can_node_v2.c` (or `telemetry_hub_v2.c` for the Hub), replacing the
   simulated channel it's standing in for — one sensor at a time, so nothing
   gets flipped on before it's actually wired and calibrated.
6. Verify against `DATA_VERIFICATION_CHECKLIST.md` — physically perturb the
   sensor, confirm the dashboard channel moves the right direction before
   trusting it.
