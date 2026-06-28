# Suspension Travel Sensor

## Approach

Sealed Bourns rotary potentiometer mounted on a custom bellcrank linkage. The bellcrank converts linear shock travel to angular pot rotation, giving a direct analog voltage proportional to suspension position.

**Sensor:** Bourns rotary potentiometer (sealed, IP67)  
**Output:** Analog voltage → MCU ADC  
**Linkage:** Custom 6061-T6 aluminum bellcrank  
**CAD:** Parametric Fusion 360 model (see `/hardware/` or contact ME teammate for fabrication files)

## Why a Potentiometer

The AS5600 magnetic encoder was evaluated and deprioritized. The Bourns pot is simpler to integrate (no I2C, no magnet alignment), more robust to vibration, and the sealed variant handles the Baja environment without additional weatherproofing.

## Bellcrank Geometry

The bellcrank is designed so the shock-to-rotation relationship is linear across the expected travel range. This means:

- No linearization needed in firmware
- ADC moving-average filter (N=8) is sufficient for noise rejection
- Calibration is a simple two-point (min travel / max travel) voltage-to-mm mapping stored in firmware

## Calibration Procedure

1. Compress suspension to full bump — record ADC value as `adc_min`
2. Extend suspension to full droop — record ADC value as `adc_max`
3. Update `TRAVEL_ADC_MIN` and `TRAVEL_ADC_MAX` in `sensors.h`
4. Travel in mm = `((adc - adc_min) / (adc_max - adc_min)) * TRAVEL_RANGE_MM`

## Fabrication Notes

- Bellcrank material: 6061-T6 aluminum
- Fabricated by ME teammate from Fusion 360 parametric CAD
- Arm length is parametric — adjust to match specific shock geometry per corner
- Same design used for front and rear corners (different arm length parameter only)
