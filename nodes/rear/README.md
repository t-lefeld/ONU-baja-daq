# Rear Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)  
**CAN ID:** 0x020  
**Location:** By the gas tank, mounted to the frame  
**Language:** C++

## Sensors

| Sensor | Part | Count | Interface | Connector | Notes |
|--------|------|-------|-----------|-----------|-------|
| Gear tooth encoder | Littelfuse 55505-00-02-A-ND | 1 | 3-wire voltage output | DTM04-3P | **Reads the brake disc, not a wheel hub** — only one at the rear |
| Suspension travel | AS5600 Hall-effect encoder on bellcrank | 2 (RL, RR) | ADC, analog (see note) | DTM04-3P | Same bellcrank design as Front, different arm length |
| Temperature | MLX90614 (non-contact IR) | 1 | I2C (SMBus, addr 0x5A) | DTM04-4P (VDD/GND/SDA/SCL) | CVT and brake disc temperature; mounted for unobstructed line-of-sight, away from exhaust radiant heat |

### Two AS5600s on one node: do not put both on I2C

Same constraint as the Front node. The AS5600's address is fixed at `0x36`,
so RL and RR cannot share a bus. Use the AS5600's **analog output** and read
both through the ADC, keeping the `PA4`/`PA5` pin plan. This also leaves I2C1
free for the MLX90614, which is already on it at `0x5A`.

CAN bus terminates here — Rear is a bus end, needs a 120Ω termination resistor across CANH/CANL.

## CAN Messages

| Message | ID | Rate | Signals |
|---------|----|------|---------|
| RearSuspension | 0x021 | 100Hz | disc_speed, travel_rl_mm, travel_rr_mm, temp_c |

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
- Gear tooth sensor input needs a real pull-up (4.7k) to 3.3V — same as Front
- The rear encoder targets the brake disc rather than a wheel hub, so its pulses-per-revolution and tooth geometry differ from the front pair — calibrate it separately, don't reuse the front constant
- No brake pressure sensor here: both master cylinders are in the front and cover both circuits (see `nodes/front/README.md`)
- MLX90614 uses SMBus timing with clock-stretching — check the STM32 I2C peripheral's timeout settings so it doesn't error out on stretched clocks. Add 4.7k I2C pull-ups if not already present on the breakout board. Give it its own local decoupling (10uF bulk + 100nF ceramic) — the thermopile is sensitive to supply noise
- I2C over a long cable run to the CVT has no built-in bus buffering — worth confirming wire length stays within MLX90614's I2C spec
- Watchdog timer: node resets and sends error frame if main loop stalls >100ms
