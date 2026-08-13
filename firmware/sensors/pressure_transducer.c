/**
 * pressure_transducer.c - see pressure_transducer.h for calibration/divider TODOs.
 */

#include "pressure_transducer.h"

#define ADC_VREF        3.3f
#define ADC_MAX_COUNT   4095.0f
#define ADC_TIMEOUT_MS  10u

void pressure_transducer_init(pressure_transducer_t *pt, ADC_HandleTypeDef *hadc, uint32_t channel,
                               float volts_at_zero_psi, float volts_at_full_scale, float full_scale_psi)
{
    pt->hadc                = hadc;
    pt->channel               = channel;
    pt->volts_at_zero_psi     = volts_at_zero_psi;
    pt->volts_at_full_scale   = volts_at_full_scale;
    pt->full_scale_psi        = full_scale_psi;
}

float pressure_transducer_read(pressure_transducer_t *pt)
{
    /* TODO: same shared-ADC caveat as suspension_pot_read() - if this shares
     * a peripheral with other analog channels, reselect the channel via
     * HAL_ADC_ConfigChannel() before HAL_ADC_Start(). */

    if (HAL_ADC_Start(pt->hadc) != HAL_OK)
    {
        return 0.0f;
    }

    if (HAL_ADC_PollForConversion(pt->hadc, ADC_TIMEOUT_MS) != HAL_OK)
    {
        HAL_ADC_Stop(pt->hadc);
        return 0.0f;
    }

    uint32_t raw = HAL_ADC_GetValue(pt->hadc);
    HAL_ADC_Stop(pt->hadc);

    float volts = ((float)raw / ADC_MAX_COUNT) * ADC_VREF;

    float span = pt->volts_at_full_scale - pt->volts_at_zero_psi;
    float frac = (span != 0.0f) ? (volts - pt->volts_at_zero_psi) / span : 0.0f;

    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    return frac * pt->full_scale_psi;
}
