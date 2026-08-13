/**
 * suspension_pot.h - Suspension travel from a Bourns 53AAA-B28-B15L rotary
 * potentiometer mounted on a bellcrank.
 *
 * Maps to simulation/vehicle_data.py channels: suspension_fl/fr (Front node)
 * and suspension_rl/rr (Rear node). One instance per corner.
 *
 * The pot is a simple resistive voltage divider - wire one outer leg to
 * 3.3V, the other outer leg to GND, and the wiper (center leg) to an ADC
 * input pin. As the bellcrank sweeps through its arc, the wiper voltage
 * moves linearly between two endpoints you calibrate by hand (there is no
 * universal "0 mm = X volts" - it depends entirely on how the pot is
 * physically mounted and linked to the bellcrank).
 *
 * TODO before wiring: pick an ADC-capable pin (TODO in the .c file), then
 * calibrate volts_at_min_travel / volts_at_max_travel by hand: compress the
 * suspension fully, read the raw ADC voltage, note it; extend it fully,
 * read again. Those two numbers replace the placeholders below.
 */

#ifndef SUSPENSION_POT_H
#define SUSPENSION_POT_H

#include <stdint.h>
#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ADC_HandleTypeDef *hadc;      /* TODO: which ADC peripheral/instance in CubeMX */
    uint32_t            channel;   /* TODO: ADC_CHANNEL_x matching the pin the wiper is on */
    float                volts_at_min_travel;  /* TODO: calibrate by hand - see header comment */
    float                volts_at_max_travel;  /* TODO: calibrate by hand - see header comment */
    float                max_travel_mm;         /* TODO: physical suspension travel range for this corner, in mm */
} suspension_pot_t;

/** Configure calibration constants. Does not touch ADC peripheral config -
 *  that's still a CubeMX .ioc setting like every other peripheral here. */
void suspension_pot_init(suspension_pot_t *sp, ADC_HandleTypeDef *hadc, uint32_t channel,
                          float volts_at_min_travel, float volts_at_max_travel, float max_travel_mm);

/**
 * Blocking single-conversion read (matches the style of a simple polled ADC
 * read - swap to DMA/interrupt-driven if this needs to run faster than the
 * 500 ms telemetry period allows for a blocking call). Returns travel in mm,
 * clamped to [0, max_travel_mm].
 */
float suspension_pot_read(suspension_pot_t *sp);

#ifdef __cplusplus
}
#endif

#endif /* SUSPENSION_POT_H */
