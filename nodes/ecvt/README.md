# eCVT Node

**MCU:** STM32 Blue Pill (STM32F103C8T6)  
**CAN ID:** 0x030  
**Location:** CVT assembly  
**Status:** ⏸ Blocked — pending mechanical specs from team

## Planned Sensors

| Sensor | Interface | Signal | Notes |
|--------|-----------|--------|-------|
| Primary RPM | TBD | TBD | Pending mech layout |
| Secondary RPM | TBD | TBD | Pending mech layout |
| Belt temperature | TBD | TBD | Pending mech layout |
| Actuator position | TBD | TBD | Pending mech layout |

## Planned CAN Messages

| Message | ID | Rate | Signals |
|---------|----|------|---------|
| eCVT_Status | 0x031 | 50Hz | primary_rpm, secondary_rpm, belt_temp, actuator_pos |

## Blocking Items

- Mechanical team to confirm sensor mounting locations and access points
- Actuator type and feedback interface TBD
- Enclosure form factor TBD pending mech clearance

## Notes

This node is intentionally left sparse until mechanical specs are received. Do not begin firmware or PCB work until sensor interfaces are confirmed.
