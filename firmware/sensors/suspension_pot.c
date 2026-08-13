/**
 * suspension_pot.c - see suspension_pot.h for calibration TODOs.
 */

#include "suspension_pot.h"

#define ADC_VREF        3.3f
#define ADC_MAX_COUNT   4095.0f   /* 12-bit ADC */
#define ADC_TIMEOUT_MS  10u

void suspension_pot_init(suspension_pot_t *sp, ADC_HandleTypeDef *hadc, uint32_t channel,
                          float volts_at_min_travel, float volts_at_max_travel, float max_travel_mm)
{
    sp->hadc                = hadc;
    sp->channel              = channel;
    sp->volts_at_min_travel  = volts_at_min_travel;
    sp->volts_at_max_travel  = volts_at_max_travel;
    sp->max_travel_mm        = max_travel_mm;
}

float suspension_pot_read(suspension_pot_t *sp)
{
    /* TODO: this assumes a single-channel ADC config where the channel was
     * already selected via CubeMX (rank 1, matching sp->channel). If this
     * ADC is shared with other analog inputs (e.g. pressure_transducer on
     * the same peripheral), it needs an ADC_ChannelConfTypeDef re-selection
     * call here before starting the conversion - see pressure_transducer.c
     * for the same pattern, since both sensors will likely share one ADC. */

    if (HAL_ADC_Start(sp->hadc) != HAL_OK)
    {
        return 0.0f;
    }

    if (HAL_ADC_PollForConversion(sp->hadc, ADC_TIMEOUT_MS) != HAL_OK)
    {
        HAL_ADC_Stop(sp->hadc);
        return 0.0f;
    }

    uint32_t raw = HAL_ADC_GetValue(sp->hadc);
    HAL_ADC_Stop(sp->hadc);

    float volts = ((float)raw / ADC_MAX_COUNT) * ADC_VREF;

    float span = sp->volts_at_max_travel - sp->volts_at_min_travel;
    float frac = (span != 0.0f) ? (volts - sp->volts_at_min_travel) / span : 0.0f;

    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    return frac * sp->max_travel_mm;
}
