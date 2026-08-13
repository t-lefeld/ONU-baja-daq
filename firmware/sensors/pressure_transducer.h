/**
 * pressure_transducer.h - Brake line pressure from an Anfield T200/T201
 * pressure transducer.
 *
 * Maps to simulation/vehicle_data.py channel: brake_pressure_f (Front node).
 *
 * These are standard automotive-style ratiometric transducers: 0.5V output
 * at 0 psi, 4.5V output at full-scale psi, linear in between, powered from
 * 5V (NOT 3.3V - check your specific part's datasheet for supply voltage
 * and full-scale range, T200 vs T201 differ). Since the STM32's ADC input
 * only tolerates up to its VDDA (3.3V typically), a 0.5-4.5V sensor output
 * needs a voltage divider to bring it into range - TODO: size that divider
 * and account for it in the calibration constants below (the two constants
 * should reflect what the ADC pin actually sees after the divider, not the
 * sensor's raw output).
 *
 * TODO before wiring: confirm supply voltage and full-scale psi rating on
 * your exact part's datasheet/label, size the divider, then calibrate
 * volts_at_zero_psi / volts_at_full_scale against the divided (post-divider)
 * voltage.
 */

#ifndef PRESSURE_TRANSDUCER_H
#define PRESSURE_TRANSDUCER_H

#include <stdint.h>
#include "stm32f1xx_hal.h"   /* TODO: swap to stm32l4xx_hal.h if this instance runs on the Nucleo hub */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ADC_HandleTypeDef *hadc;      /* TODO: which ADC peripheral/instance in CubeMX */
    uint32_t            channel;   /* TODO: ADC_CHANNEL_x matching the divided-output pin */
    float                volts_at_zero_psi;   /* TODO: calibrate - post-divider voltage at 0 psi (nominally sensor's 0.5V / divider ratio) */
    float                volts_at_full_scale; /* TODO: calibrate - post-divider voltage at full-scale (nominally sensor's 4.5V / divider ratio) */
    float                full_scale_psi;       /* TODO: from the transducer's datasheet/part label */
} pressure_transducer_t;

void pressure_transducer_init(pressure_transducer_t *pt, ADC_HandleTypeDef *hadc, uint32_t channel,
                               float volts_at_zero_psi, float volts_at_full_scale, float full_scale_psi);

/** Blocking single-conversion read, same pattern as suspension_pot_read().
 *  Returns psi, clamped to [0, full_scale_psi]. */
float pressure_transducer_read(pressure_transducer_t *pt);

#ifdef __cplusplus
}
#endif

#endif /* PRESSURE_TRANSDUCER_H */
