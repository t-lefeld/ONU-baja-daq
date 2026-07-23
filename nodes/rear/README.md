# Rear Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)  
**CAN ID:** 0x020  
**Location:** Rear suspension corner  
**Language:** C++

## Sensors

| Sensor | Part | Interface | Connector | Notes |
|--------|------|-----------|-----------|-------|
| Wheel hub encoder | Littelfuse 55075 gear tooth sensor | 3-wire voltage output | DTM04-3P | Wheel speed |
| Suspension travel | Bourns 53AAA-B28-B15L rotary pot on bellcrank | ADC, analog | DTM04-3P | Same bellcrank design as Front, different arm length |
| E-CVT belt temperature | MLX90614 (non-contact IR) | I2C (SMBus, addr 0x5A) | DTM04-4P (VDD/GND/SDA/SCL) | Reads CVT belt temp; mounted for unobstructed line-of-sight to belt, away from exhaust radiant heat |

CAN bus terminates here — Rear is a bus end, needs a 120Ω termination resistor across CANH/CANL.

## CAN Messages

| Message | ID | Rate | Signals |
|---------|----|------|---------|
| RearSuspension | 0x021 | 100Hz | wheel_speed, travel_mm, belt_temp_c |

## Firmware Structure

```
rear/
└── src/
    ├── main.cpp
    ├── sensors.cpp/.h    ← ADC read, moving-average filter, I2C read
    └── can_tx.cpp/.h     ← CAN message packing and transmit
```

## Notes

- Same Blue Pill + SN65HVD230 transceiver setup as Front node
- Shares bellcrank linkage design with Front — same Fusion 360 parametric CAD, different arm length
- Wheel speed sensor input needs a real pull-up (4.7k) to 3.3V — same as Front
- MLX90614 uses SMBus timing with clock-stretching — check the STM32 I2C peripheral's timeout settings so it doesn't error out on stretched clocks. Add 4.7k I2C pull-ups if not already present on the breakout board. Give it its own local decoupling (10uF bulk + 100nF ceramic) — the thermopile is sensitive to supply noise
- I2C over a long cable run to the CVT has no built-in bus buffering — worth confirming wire length stays within MLX90614's I2C spec
- Watchdog timer: node resets and sends error frame if main loop stalls >100ms
