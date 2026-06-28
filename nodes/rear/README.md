# Rear Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)  
**CAN ID:** 0x020  
**Location:** Rear suspension corner  
**Language:** C++

## Sensors

| Sensor | Interface | Signal | Notes |
|--------|-----------|--------|-------|
| Suspension travel | ADC | Analog | Bourns rotary pot on bellcrank linkage |
| Drivetrain sensor | TBD | TBD | TBD |

## CAN Messages

| Message | ID | Rate | Signals |
|---------|----|------|---------|
| RearSuspension | 0x021 | 100Hz | travel_mm |

## Firmware Structure

```
rear/
└── src/
    ├── main.cpp
    ├── sensors.cpp/.h    ← ADC read, moving-average filter
    └── can_tx.cpp/.h     ← CAN message packing and transmit
```

## Notes

- Same Blue Pill + SN65HVD230 transceiver setup as front node
- Shares bellcrank linkage design with front — same Fusion 360 parametric CAD, different arm length
