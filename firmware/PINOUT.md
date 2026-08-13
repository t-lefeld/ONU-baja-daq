# Pinout — what's assigned now, what v2 needs

Audited 2026-08-12 against the `.ioc` files, the driver sources, and the
wiring tables in `nucleo_hub/INTEGRATION.md` and `BRINGUP_GUIDE.md`.

---

## ⚠ Read this before wiring anything new

**Nothing in the "what v2 needs" half of this document is configured yet.**
The pins listed there are suggestions, not settings. Adding a peripheral to
this project is a two-step job and the first step is not optional:

1. **Open the project's `.ioc` in STM32CubeMX, assign the peripheral, and
   Generate Code.** That is what creates `MX_ADC1_Init()`, `MX_I2C1_Init()`,
   the GPIO clock enables, the EXTI NVIC entries, and the `hadc1`/`hi2c1`
   handles the drivers expect to be handed.
2. Only then fill in the matching `TODO:` constants in
   `firmware/sensors/*.h` and wire the driver into the node.

**Do not hand-edit a `.ioc`.** I deliberately have not generated any, and I'd
push back if asked to: CubeMX writes them with its own key ordering and
version stamps, and a key it doesn't recognise is *silently dropped* rather
than rejected. A silently dropped setting looks exactly like everything
working — right up until the peripheral behaves differently than the code
assumes. `firmware/CUBEMX_SETUP.md` documents this failure mode from the
edits already made to these files, and the verification steps it lists exist
because of it.

If a driver's `hadc`/`hi2c` handle is never initialised by CubeMX, the read
call returns zeros rather than erroring — so a missing step here shows up as
a channel that reads a plausible, constant, completely fake value.

---

## Currently assigned — v1, working on hardware

### Nucleo-L476RG (hub)

| Pin | Function | Peripheral | Notes |
|---|---|---|---|
| PA1 | E22 AUX | GPIO in | open-drain, needs pull-up |
| PA2 | debug TX | USART2 | ST-Link Virtual COM Port |
| PA3 | debug RX | USART2 | |
| PA4 | E22 M1 | GPIO out | Arduino A2 |
| PA5 | SD CLK | SPI1 SCK | **also drives LD2** — flickers with traffic |
| PA6 | SD DO | SPI1 MISO | |
| PA7 | SD DI | SPI1 MOSI | |
| PA9 | E22 RXD | USART1 TX | Arduino D8 |
| PA10 | E22 TXD | USART1 RX | Arduino D2 |
| PA11 | CAN1 RX | CAN1 | |
| PA12 | CAN1 TX | CAN1 | |
| PB0 | E22 M0 | GPIO out | Arduino A3 |
| PB6 | SD CS | GPIO out | idles high |

**PA5/PA6/PA7 are deliberately absent from the `.ioc`.** `sd_spi.c` drives
SPI1 through its registers directly — it enables the clock, configures the
pins and sets the baud rate itself — because the project has no
`stm32l4xx_hal_spi.c`. Adding SPI1 in CubeMX would generate an
`MX_SPI1_Init()` that `sd_spi_init()` immediately overrides, which is
harmless but makes it look like two things own the peripheral. Verified
consistent: the `.ioc`, `CUBEMX_SETUP.md` and `sd_spi.c` all agree. **Leave
them unassigned.**

E22 M1 and AUX sit on PA4/PA1 rather than PB1/PB2 so all five E22 signals
reach the Arduino header instead of needing the Morpho connector.

### Bluepill STM32F103C8 (CAN nodes)

| Pin | Function | Notes |
|---|---|---|
| PA11 | CAN RX | |
| PA12 | CAN TX | |
| PA13 | SWDIO | debug — do not reuse |
| PA14 | SWCLK | debug — do not reuse |

Everything else is free. All three boards are identical; only
`Core/Inc/node_id.h` differs.

---

## What v2 needs — not configured, suggestions only

### Hub (Nucleo) — node 0

| Sensor | Needs | Free candidates | Watch out for |
|---|---|---|---|
| NEO-M8N GPS | 1× UART RX (TX optional) | USART3 on PC4/PC5, or LPUART1 | Only the module's TX → your RX matters for read-only NMEA |
| BNO080 IMU | I2C (SCL+SDA) + INT + RST | I2C1 on PB8/PB9 | Needs a RST pulse at power-up; that GPIO is board-specific and the driver expects the caller to do it |

Avoid PA5/PA6/PA7 even though they look free in CubeMX — `sd_spi.c` owns them.

### Front / Rear Bluepills — nodes 1 and 2

Each needs 2 wheel encoders, 2 suspension pots, plus one of brake pressure
(Front) or CVT temp (Rear).

| Sensor | Needs | Notes |
|---|---|---|
| Wheel encoder ×2 | GPIO + EXTI, one per wheel | **see the EXTI trap below** |
| Suspension pot ×2 | ADC channel each | ADC1 channels live on PA0–PA7, PB0–PB1 |
| Brake pressure (Front) | ADC channel | 0.5–4.5 V sensor needs a divider to reach 3.3 V |
| CVT temp (Rear) | I2C | MLX90614 at 0x5A, fixed |

**The EXTI trap.** On STM32F1 the external-interrupt lines are shared by pin
*number*, not by port: PA0 and PB0 both drive EXTI0, and only one can be
enabled at a time. Putting the two wheel encoders on PA0 and PB0 means the
second one silently never fires. **Give each encoder a different pin number**
— e.g. PB0 and PB1 rather than PA0 and PB0.

Also note `wheel_encoder.c` counts edges in the EXTI callback and converts on
a timer. If both encoders share a callback, dispatch on `GPIO_Pin` —
`wheel_encoder_on_pulse()` already checks that internally, so calling it once
per encoder from a shared handler is safe.

**ADC sharing.** `suspension_pot.c` and `pressure_transducer.c` both do a
blocking single conversion and assume the channel is already selected. If two
sensors share one ADC peripheral — which they will — each read must
re-select its channel via `HAL_ADC_ConfigChannel()` first. Both drivers carry
a `TODO:` at exactly that spot.

### Motor / E-CVT — node 3

No pins. The ODrive S1 is its own CAN node on the bus the hub already
listens to; `odrive_can.c` decodes its frames from the existing CAN
peripheral. The only configuration is the ODrive's own `axis_node_id`, which
must not collide with the `0x200` block v2 uses for node pages — see
`protocol/V2_DESIGN_NOTES.md`.

---

## Order of work

1. Decide pins per board and write them down here.
2. CubeMX: assign, Generate Code, verify against `CUBEMX_SETUP.md`'s checks.
3. Fill the `TODO:` constants in `firmware/sensors/*.h`.
4. Calibrate the analog channels by hand (endpoint voltages — the drivers
   cannot guess these; they depend on how the pot is physically linked).
5. Wire the driver call into the node, replacing one simulated channel.
6. Verify against `DATA_VERIFICATION_CHECKLIST.md` — physically perturb the
   sensor and confirm the reading moves in the right direction. A channel
   that does not respond to a real stimulus is disconnected, stuck, or still
   reading the simulator.
