# Suspension Travel Sensor

## Approach

**AS5600 Hall-effect magnetic encoder** mounted on a custom bellcrank
linkage, one per corner — four on the car. The bellcrank converts linear
shock travel to angular rotation, and the AS5600 reads the angle of a
diametrically magnetized magnet on the bellcrank shaft.

**Sensor:** AS5600 (12-bit magnetic rotary position sensor)
**Output:** analog voltage on the `OUT` pin → MCU ADC (see "Why analog" below)
**Linkage:** custom 6061-T6 aluminum bellcrank
**CAD:** parametric Fusion 360 model (see `/hardware/` or contact ME teammate for fabrication files)

## Why the AS5600, and why this reverses an earlier decision

This project previously specified a sealed Bourns 53AAA-B28-B15L rotary
potentiometer here, and this file previously argued *against* the AS5600 on
the grounds that a pot needs no I2C and no magnet alignment, resists
vibration better, and is already sealed.

**That decision was reversed on 2026-10-30. The car uses the AS5600.** The
reasoning: resolution is not a constraint for this application, and the
AS5600 is cheaper. The earlier objections do not survive contact with the
analog-output mode described below — it removes the I2C concern entirely.

The one real cost of the change is magnet mounting: a potentiometer couples
mechanically to the shaft, while the AS5600 needs a diametrically magnetized
magnet held concentric to the sensor at the right air gap. That is a
fabrication requirement the pot did not have. Get it wrong and readings drift
or wrap.

If this gets reversed again, update this section rather than deleting it.
Knowing that both options were considered, and why the choice flipped, is
worth more than a clean-looking file.

## Why analog output, not I2C

**The AS5600's I2C address is fixed at `0x36` with no strapping pins.** Two
of them cannot share a bus. Both the Front and Rear nodes read two corners
each, so I2C is not an option without extra hardware.

The AS5600 can output angle as an **analog voltage** (or PWM) on its `OUT`
pin instead. Using that:

- Both corners on a node read through the ADC, on the existing `PA4`/`PA5`
  pin plan
- The read-and-calibrate pattern is identical to the potentiometer this
  replaced, so `firmware/sensors/suspension_pot.c` stays structurally valid
- No I2C bus contention with the Rear node's MLX90614, which already sits on
  I2C1 at `0x5A`

If a future revision genuinely needs the digital interface (for the status
bits, or 12-bit resolution the ADC can't match), the options are the AS5600L
with a programmable address, an I2C mux such as a TCA9548A, or a second bus.

## Bellcrank Geometry

The bellcrank is designed so the shock-to-rotation relationship is linear
across the expected travel range. This means:

- No linearization needed in firmware
- ADC moving-average filter (N=8) is sufficient for noise rejection
- Calibration is a simple two-point (min travel / max travel) mapping stored
  in firmware

Keep total rotation inside the AS5600's single-turn range. Unlike a pot,
which simply stops, a magnetic encoder **wraps** past its range — travel
would read as a sudden jump to the opposite extreme rather than a clipped
value. Confirm the arm length keeps full bump-to-droop within one turn.

## Calibration Procedure

1. Compress suspension to full bump — record ADC value as `adc_min`
2. Extend suspension to full droop — record ADC value as `adc_max`
3. Update `TRAVEL_ADC_MIN` and `TRAVEL_ADC_MAX` in `sensors.h`
4. Travel in mm = `((adc - adc_min) / (adc_max - adc_min)) * TRAVEL_RANGE_MM`

Before calibrating, confirm the reading is monotonic across the full sweep.
A wrap or a dead spot means the magnet is misaligned or the travel exceeds
one turn — fix that first, or the two-point calibration will encode nonsense.

## Fabrication Notes

- Bellcrank material: 6061-T6 aluminum
- Fabricated by ME teammate from Fusion 360 parametric CAD
- Arm length is parametric — adjust to match specific shock geometry per corner
- Same design used for front and rear corners (different arm length parameter only)
- The AS5600 needs a diametrically magnetized magnet, concentric with the
  sensor and within the datasheet's air gap. Budget a magnet holder on the
  bellcrank shaft; this did not exist in the potentiometer design

## Related: shock nitrogen pressure

A separate pressure transducer taps each shock's Schrader nitrogen valve, to
correlate gas pressure against the travel measured here. Shocks read 85 psi
at full extension and are rated to 300 psi. Each needs a machined metal
adapter for the pressure seal plus a 3D printed housing for mounting and
electronics.

**This is a testing instrument only. It does not go on the car at a race.**
Do not design the harness, enclosures, or channel budget around it.
