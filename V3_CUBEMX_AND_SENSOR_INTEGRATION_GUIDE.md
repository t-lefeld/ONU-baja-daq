# V3: STM32CubeMX Pinout & Physical Sensor Integration Guide

This guide details the complete hardware pinout matrix, STM32CubeMX `.ioc` configuration instructions, sensor driver hooks, and physical calibration procedures for the **v3 Live Sensor Telemetry System**.

**Corrected 2026-08-16 to match Tate's actual PCB ("Firewall PCB" schematic)
for Front/Rear.** This file previously had the earlier suggested pins (from
`firmware/PINOUT_V3_PLAN.md`'s first draft) for Front/Rear wheel encoders,
suspension pots, and brake pressure — those have been replaced below with
what's actually traced on the board. `firmware/PINOUT_V3_PLAN.md` still
exists as the running log of that discovery (screenshots, the back-and-forth
on the CVT I2C pin problem) but this file is now the single canonical pinout
to build from. Hub pins are unchanged (no PCB for the Hub shared yet — these
remain suggestions until you have one).

---

## 1. Conflict-Free Hardware Pinout Matrix

### A. Hub Board: STM32L476RG Nucleo-64 (Node 0 & Aggregator)

| Function / Sensor | Pin | Peripheral | Configuration in CubeMX | Notes / Wiring |
| :--- | :---: | :---: | :--- | :--- |
| **CAN Bus RX** | `PA11` | `CAN1` | CAN1_RX, 500 kbit/s | Shared differential CAN transceiver |
| **CAN Bus TX** | `PA12` | `CAN1` | CAN1_TX, 500 kbit/s | Shared differential CAN transceiver |
| **LoRa E22 TXD** | `PA10` | `USART1` | USART1_RX (Asynchronous, 9600 / 115200) | Arduino Header D2 |
| **LoRa E22 RXD** | `PA9` | `USART1` | USART1_TX (**DMA Enabled**, Normal mode) | Arduino Header D8 |
| **LoRa E22 M0** | `PB0` | `GPIO_Output` | Push-Pull, Low, Pull-down | Arduino Header A3 |
| **LoRa E22 M1** | `PA4` | `GPIO_Output` | Push-Pull, Low, Pull-down | Arduino Header A2 |
| **LoRa E22 AUX** | `PA1` | `GPIO_Input` | Pull-up (Open-Drain output from E22) | Arduino Header A1 |
| **microSD Card SCK** | `PA5` | *Direct Register* | **Leave unassigned in CubeMX** | Driven directly by `sd_spi.c` |
| **microSD Card MISO**| `PA6` | *Direct Register* | **Leave unassigned in CubeMX** | Driven directly by `sd_spi.c` |
| **microSD Card MOSI**| `PA7` | *Direct Register* | **Leave unassigned in CubeMX** | Driven directly by `sd_spi.c` |
| **microSD Card CS** | `PB6` | `GPIO_Output` | Output Push-Pull, High (idles high) | Software Chip Select |
| **ST-Link Debug TX**| `PA2` | `USART2` | USART2_TX (Asynchronous, 115200 8N1) | VCP telemetry mirroring |
| **ST-Link Debug RX**| `PA3` | `USART2` | USART2_RX (Asynchronous, 115200 8N1) | VCP console input |
| **NEO-M8N GPS RX** | `PC5` | `USART3` | USART3_RX (Asynchronous, 9600 8N1, **RX Interrupt**) | Connects to GPS Module TX pin |
| **BNO080 IMU SCL** | `PB8` | `I2C1` | I2C1_SCL (Standard 100 kHz / Fast 400 kHz) | $4.7\,\text{k}\Omega$ external pull-up |
| **BNO080 IMU SDA** | `PB9` | `I2C1` | I2C1_SDA (Standard 100 kHz / Fast 400 kHz) | $4.7\,\text{k}\Omega$ external pull-up |
| **BNO080 IMU INT** | `PB1` | `GPIO_EXTI1`| External Interrupt, Falling edge, Pull-up | Data ready interrupt |
| **BNO080 IMU RST** | `PB2` | `GPIO_Output` | Push-Pull, High (pulse low on boot) | Hardware reset line |

---

### B. Front Node: STM32F103C8 Bluepill (Node 1)

**Pins below match the actual "Firewall PCB" schematic, not the original
suggestion.** Front's connector is populated with two pressure transducers
(primary + redundant/backup), not one.

| Function / Sensor | Pin | Peripheral | Configuration in CubeMX | Notes / Wiring |
| :--- | :---: | :---: | :--- | :--- |
| **CAN Bus RX** | `PA11` | `CAN1` | CAN_RX, 500 kbit/s | Connects to CAN Transceiver RXD |
| **CAN Bus TX** | `PA12` | `CAN1` | CAN_TX, 500 kbit/s | Connects to CAN Transceiver TXD |
| **SWD Debug** | `PA13 / PA14`| `SYS_SWD` | Serial Wire | Do not reassign |
| **Wheel Speed FL** (`FWHEELSPDL`) | `PA0` | `GPIO_EXTI0` | Input with Pull-up, Falling Edge Interrupt | Hall gear-tooth sensor (open-collector) |
| **Wheel Speed FR** (`FWHEELSPDR`) | `PA1` | `GPIO_EXTI1` | Input with Pull-up, Falling Edge Interrupt | **Must be a different pin number from FL** (avoids EXTI-line conflict) |
| **Suspension Pot FL** (`FPOTL`) | `PA4` | `ADC1_IN4` | 12-bit Single Conversion, 239.5 cycles sample | Center wiper of Bourns 53AAA |
| **Suspension Pot FR** (`FPOTR`) | `PA5` | `ADC1_IN5` | 12-bit Single Conversion, 239.5 cycles sample | Center wiper of Bourns 53AAA |
| **Pressure Transducer 1 (primary)** (`PREST1`) | `PA6` | `ADC1_IN6` | 12-bit Single Conversion, 239.5 cycles sample | From resistor divider ($0.5\text{V}-4.5\text{V} \rightarrow 0.36\text{V}-3.28\text{V}$) — this is the value that goes on the wire as `brake_pressure_f` |
| **Pressure Transducer 2 (redundant/backup)** (`PREST2`) | `PA7` | `ADC1_IN7` | 12-bit Single Conversion, 239.5 cycles sample | Same physical measurement as PREST1 — read for sanity-checking/failover, not sent as a separate channel |

---

### C. Rear Node: STM32F103C8 Bluepill (Node 2)

**Pins below match the actual "Firewall PCB" schematic** (same board design
as Front, populated with the CVT thermistor connector instead of pressure
transducers).

| Function / Sensor | Pin | Peripheral | Configuration in CubeMX | Notes / Wiring |
| :--- | :---: | :---: | :--- | :--- |
| **CAN Bus RX** | `PA11` | `CAN1` | CAN_RX, 500 kbit/s | Connects to CAN Transceiver RXD |
| **CAN Bus TX** | `PA12` | `CAN1` | CAN_TX, 500 kbit/s | Connects to CAN Transceiver TXD |
| **SWD Debug** | `PA13 / PA14`| `SYS_SWD` | Serial Wire | Do not reassign |
| **Wheel Speed RL** | `PA0` | `GPIO_EXTI0` | Input with Pull-up, Falling Edge Interrupt | Hall gear-tooth sensor |
| **Wheel Speed RR** | `PA1` | `GPIO_EXTI1` | Input with Pull-up, Falling Edge Interrupt | **Must be a different pin number from RL** |
| **Suspension Pot RL** | `PA4` | `ADC1_IN4` | 12-bit Single Conversion, 239.5 cycles sample | Center wiper of Bourns 53AAA |
| **Suspension Pot RR** | `PA5` | `ADC1_IN5` | 12-bit Single Conversion, 239.5 cycles sample | Center wiper of Bourns 53AAA |
| **CVT Temp MLX90614 SCL** | `PB6` | `I2C1` | I2C1_SCL (Standard 100 kHz) | $4.7\,\text{k}\Omega$ pull-up (Address 0x5A). **PCB currently traces this to PB8 — not a valid hardware I2C1 pin paired with PB7. Reroute this trace to PB6 before bring-up** (default I2C1 pins are PB6/PB7; no AFIO remap needed once fixed). |
| **CVT Temp MLX90614 SDA** | `PB7` | `I2C1` | I2C1_SDA (Standard 100 kHz) | $4.7\,\text{k}\Omega$ pull-up (Address 0x5A). Already correctly wired on the current PCB — only the SCL trace needs to move. |

---

### D. Powertrain Node: ODrive S1 (Node 3)
* **Physical Interface:** Directly connected to the shared differential CAN bus.
* **Configuration:** Set `axis_node_id = 0` via `odrivetool`. (Safe range is $0-15$; do not use $16$ to avoid collision with v2 base ID $0x200$).

---

## 2. Step-by-Step STM32CubeMX Generation Protocol

1. **Open CubeMX:**
   * Open `firmware/projects/Nucleo CAN Bus Test/Nucleo CAN Bus Test.ioc` (for Hub) or `Bluepill CAN Node 0/1/2.ioc` (for Nodes).
2. **Assign Peripherals according to the tables above:**
   * Enable NVIC interrupts for CAN1 RX0, USART3 RX, EXTI0, and EXTI1.
   * On Hub USART1 (LoRa), ensure DMA TX is enabled.
3. **Generate Code:**
   * In Project Manager $\rightarrow$ Code Generator $\rightarrow$ Check *"Keep User Code when re-generating"*.
   * Click **Generate Code**.
4. **Compile & Verify:**
   * Build in STM32CubeIDE or PlatformIO to confirm zero unresolved symbols.

---

## 3. Physical Sensor Calibration Checklist

Before trusting live data in the pit:

### 1. Suspension Pots (Bourns 53AAA)
* Measure raw voltage at full compression: $V_{min}$ (e.g., $0.45\,\text{V}$).
* Measure raw voltage at full extension: $V_{max}$ (e.g., $2.85\,\text{V}$).
* Enter measured values into `suspension_pot_init(&sp, &hadc1, channel, V_min, V_max, travel_mm)`.

### 2. Wheel Encoders (Littelfuse 55075)
* Count exact tone ring tooth count ($N_{teeth}$, typically 18–36).
* Measure tire rolling circumference ($C_m$, roll tire 1 revolution under load, measure distance in meters, e.g. $1.72\,\text{m}$).
* Enter into `wheel_encoder_init(&enc, port, pin, N_teeth, C_m)`.

### 3. Brake Pressure Transducers (Anfield T200/T201) — two, PREST1 + PREST2
* Sensor outputs $0.5\,\text{V}-4.5\,\text{V}$ for $0-2000\,\text{psi}$.
* Voltage divider ($R_1 = 10\,\text{k}\Omega, R_2 = 27\,\text{k}\Omega$) scales $4.5\,\text{V} \rightarrow 3.28\,\text{V}$.
* Calibrate zero offset ($V_0 \approx 0.36\,\text{V}$ at 0 psi) and span ($K_{psi/V} \approx 685\,\text{psi/V}$) — **for both PREST1 and PREST2 separately**, they're two physical sensors and may not read identically even at the same pressure.
* Decide before wiring in: does `brake_pressure_f` come from PREST1 alone with PREST2 read-but-unused (simplest), or averaged, or PREST2 as failover if PREST1 reads implausibly? Pick one and note it in `can_node_v2.c` where the channel is packed.

### 4. MLX90614 CVT Infrared Thermometer
* Align sensor aiming directly at the CVT belt center with $20\,\text{mm}-40\,\text{mm}$ standoff.
* Compare cold temperature reading against ambient thermometer ($<2^\circ\text{C}$ error).

### 5. NEO-M8N GPS & BNO080 IMU
* Walk antenna near window / outdoors $\rightarrow$ confirm satellites rise to $\ge 8$ within 45s.
* Place Hub flat on table $\rightarrow$ verify `accel_z` reads $1.000\,\text{g} \pm 0.05\,\text{g}$, `accel_x/y` read $\approx 0.00\,\text{g}$.
