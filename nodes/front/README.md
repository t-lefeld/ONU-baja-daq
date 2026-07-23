# Front Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)  
**CAN ID:** 0x010  
**Location:** Front suspension corner  
**Language:** C++

## Sensors

| Sensor | Part | Interface | Connector | Notes |
|--------|------|-----------|-----------|-------|
| Wheel hub encoder | Littelfuse 55075 gear tooth sensor | 3-wire voltage output | DTM04-3P | Wheel speed |
| Suspension travel | Bourns 53AAA-B28-B15L rotary pot on bellcrank | ADC, analog | DTM04-3P | Sealed pot, single-gang (CW/CCW/wiper) |
| Pressure transducer | Anfield T200/T201 | ADC, analog (0.5–4.5V ratiometric) | DT04-3P or DT04-4P (match ordered variant) | 1.5k/3.3k divider scales to ADC-safe ~3.09V max |

CAN bus terminates here — Front is a bus end, needs a 120Ω termination resistor across CANH/CANL.

## CAN Messages

| Message | ID | Rate | Signals |
|---------|----|------|---------|
| FrontSuspension | 0x011 | 100Hz | wheel_speed, travel_mm, pressure_psi |

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
- Pressure transducer ADC pins need their voltage divider (1.5k series + 3.3k to GND) — don't feed the raw 0.5–4.5V signal straight into the ADC pin, it exceeds the STM32's absolute max input rating
- Watchdog timer: node resets and sends error frame if main loop stalls >100ms
- Encoder and suspension-pot connectors are both DTM04-3P — order them in different Deutsch keyways (e.g. Key A vs Key B) so they can't be cross-plugged
