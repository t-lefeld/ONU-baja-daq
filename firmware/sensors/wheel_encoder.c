/**
 * wheel_encoder.c - see wheel_encoder.h for the pin/tooth-count TODOs.
 */

#include "wheel_encoder.h"

#define MPS_PER_MPH 0.44704f

void wheel_encoder_init(wheel_encoder_t *enc, GPIO_TypeDef *port, uint16_t pin,
                         uint16_t teeth, float circumference_m)
{
    enc->port              = port;
    enc->pin                = pin;
    enc->teeth              = teeth;
    enc->circumference_m    = circumference_m;
    enc->pulse_count        = 0u;
    enc->last_sample_ms     = HAL_GetTick();
    enc->last_pulse_count   = 0u;
    enc->speed_mph          = 0.0f;
}

void wheel_encoder_on_pulse(wheel_encoder_t *enc, uint16_t GPIO_Pin)
{
    if (GPIO_Pin == enc->pin)
    {
        enc->pulse_count++;
    }
}

float wheel_encoder_update(wheel_encoder_t *enc)
{
    uint32_t now = HAL_GetTick();
    uint32_t dt_ms = now - enc->last_sample_ms;

    /* Guard against a near-zero interval producing a huge/garbage reading -
     * skip this update and try again next call rather than divide by ~0. */
    if (dt_ms < 20u)
    {
        return enc->speed_mph;
    }

    uint32_t count_now = enc->pulse_count;   /* volatile - snapshot once */
    uint32_t pulses = count_now - enc->last_pulse_count;

    if (enc->teeth > 0u && enc->circumference_m > 0.0f)
    {
        float revs = (float)pulses / (float)enc->teeth;
        float meters = revs * enc->circumference_m;
        float dt_s = (float)dt_ms * 0.001f;
        float mps = meters / dt_s;
        enc->speed_mph = mps / MPS_PER_MPH;
    }

    enc->last_sample_ms   = now;
    enc->last_pulse_count = count_now;

    return enc->speed_mph;
}
