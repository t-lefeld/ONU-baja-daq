# Front Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)  
**CAN ID:** 0x010  
**Location:** Toebox, mounted to the frame  
**Language:** C++

## Sensors

| Sensor | Part | Count | Interface | Connector | Notes |
|--------|------|-------|-----------|-----------|-------|
| Wheel hub encoder | Littelfuse 55505-00-02-A-ND gear tooth sensor | 2 (FL, FR) | 3-wire voltage output | DTM04-3P | One per front wheel |
| Suspension travel | AS5600 Hall-effect encoder on bellcrank | 2 (FL, FR) | ADC, analog (see note) | DTM04-3P | One per corner |
| Brake pressure transducer | Anfield T200/T201 | 2 | ADC, analog (0.5–4.5V ratiometric) | DT04-3P or DT04-4P (match ordered variant) | 1.5k/3.3k divider scales to ADC-safe ~3.09V max |
| Shock nitrogen pressure | Pressure transducer on Schrader valve | 2 | ADC, analog | — | **Testing only — does not go on the car at a race** |

### Brake pressure is read here for the whole car

Both master cylinders are in the front and cover the front *and* rear brake
circuits, so there is no brake pressure sensor on the Rear node. The
transducer tees in alongside the brake switch, which means **the brake light
keeps working even if this board fails.** Preserve that — brake-light
triggering must never depend on the DAQ.

### Two AS5600s on one node: do not put both on I2C

The AS5600's I2C address is fixed at `0x36` with no strapping pins, so FL and
FR cannot share an I2C bus. Use the AS5600's **analog output** on its `OUT`
pin and read both through the ADC — this keeps the existing `PA4`/`PA5` pin
plan and the same read-and-calibrate pattern as the pot it replaced.
Alternatives if you ever need the digital interface: the AS5600L
(programmable address), an I2C mux, or a second bus.

CAN bus terminates here — Front is a bus end, needs a 120Ω termination resistor across CANH/CANL.

## CAN Messages

| Message | ID | Rate | Signals |
|---------|----|------|---------|
| FrontSuspension | 0x011 | 100Hz | wheel_speed_fl, wheel_speed_fr, travel_fl_mm, travel_fr_mm, brake_pressure_psi |

## Firmware Structure

```
front/
└── src/
    ├── main.cpp
    ├── sensors.cpp/.h    ← ADC read, moving-average filter
    └── can_tx.cpp/.h     ← CAN message packing and transmit
```

## Notes

- Blue Pill CAN peripheral requires external CAN transceiver (SN65HVD230, confirmed 3.3V-native part)
- ADC moving-average filter (N=8) applied to suspension travel — no additional calibration needed given linear bellcrank geometry
- Wheel speed sensor input needs a real pull-up (4.7k) to 3.3V — it's an open-drain output, not driven high/low on its own
- Two wheel encoders on one board share EXTI lines if their pins have the same pin number on different ports — see `firmware/PINOUT.md` before assigning
- Pressure transducer ADC pins need their voltage divider (1.5k series + 3.3k to GND) — don't feed the raw 0.5–4.5V signal straight into the ADC pin, it exceeds the STM32's absolute max input rating
- Watchdog timer: node resets and sends error frame if main loop stalls >100ms
- Encoder and suspension-pot connectors are both DTM04-3P — order them in different Deutsch keyways (e.g. Key A vs Key B) so they can't be cross-plugged
