# Front Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)  
**CAN ID:** 0x010  
**Location:** Front suspension corner  
**Language:** C++

## Sensors

| Sensor | Interface | Signal | Notes |
|--------|-----------|--------|-------|
| Suspension travel | ADC | Analog | Bourns rotary pot on bellcrank linkage |
| Steering angle | ADC | Analog | TBD |

## CAN Messages

| Message | ID | Rate | Signals |
|---------|----|------|---------|
| FrontSuspension | 0x011 | 100Hz | travel_mm, steering_deg |

## Firmware Structure

```
front/
└── src/
    ├── main.cpp
    ├── sensors.cpp/.h    ← ADC read, moving-average filter
    └── can_tx.cpp/.h     ← CAN message packing and transmit
```

## Notes

- Blue Pill CAN peripheral requires external CAN transceiver (SN65HVD230 or equivalent)
- ADC moving-average filter (N=8) applied to suspension travel — no additional calibration needed given linear bellcrank geometry
- Watchdog timer: node resets and sends error frame if main loop stalls >100ms
