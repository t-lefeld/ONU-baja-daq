/**
 * wheel_encoder.h - Wheel speed from a Littelfuse 55075 gear-tooth sensor.
 *
 * Maps to simulation/vehicle_data.py channels: wheel_speed_fl/fr (Front node)
 * and wheel_speed_rl/rr (Rear node). One instance of this driver per wheel -
 * call wheel_encoder_init() once per sensor with its own pin/timer, and read
 * each independently.
 *
 * The 55075 is a digital Hall-effect gear-tooth sensor: open-collector output
 * that pulses once per tooth as the wheel hub's tone ring spins past it. This
 * driver counts pulses via a GPIO external interrupt and converts pulse rate
 * to wheel speed using the tone ring's tooth count and the wheel's rolling
 * circumference - both TODOs below since they depend on your actual hub/tone
 * ring hardware, not just the sensor part number.
 *
 * TODO before wiring: confirm the tone ring's tooth count (count them, or
 * check the hub/CV joint part spec) and measure the tire's rolling
 * circumference (roll it one full revolution and measure the distance, more
 * accurate than calculating from nominal tire diameter).
 */

#ifndef WHEEL_ENCODER_H
#define WHEEL_ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    GPIO_TypeDef *port;         /* TODO: verify against schematic - which GPIO port the sensor's signal pin lands on */
    uint16_t      pin;          /* TODO: verify against schematic - e.g. GPIO_PIN_0 */
    uint16_t      teeth;        /* TODO: tone ring tooth count - count them on the actual hub hardware */
    float         circumference_m; /* TODO: measured rolling circumference of this wheel, in meters */
    volatile uint32_t pulse_count;  /* internal - do not set directly */
    uint32_t      last_sample_ms;
    uint32_t      last_pulse_count;
    float         speed_mph;
} wheel_encoder_t;

/**
 * Configure one wheel's encoder state. Does NOT configure the GPIO/EXTI
 * peripheral itself - that still needs to be set up in CubeMX (GPIO_EXTI
 * mode, both-edge or falling-edge trigger depending on the sensor's output
 * polarity) exactly like the existing CAN/SPI/UART peripherals in this
 * project. This call just tells the driver which pin to expect interrupts
 * from and how to convert pulses to speed.
 */
void wheel_encoder_init(wheel_encoder_t *enc, GPIO_TypeDef *port, uint16_t pin,
                         uint16_t teeth, float circumference_m);

/**
 * Call this from the matching pin's HAL_GPIO_EXTI_Callback(). Checks GPIO_Pin
 * against this instance's configured pin before counting, so it's safe to
 * call once per encoder from a shared callback that dispatches by pin number.
 */
void wheel_encoder_on_pulse(wheel_encoder_t *enc, uint16_t GPIO_Pin);

/**
 * Call periodically (e.g. once per main-loop tick, faster than the 500 ms
 * telemetry frame period for a stable reading) to recompute speed from the
 * pulse count accumulated since the last call. Returns the current speed in
 * mph; also cached in enc->speed_mph for can_node.c to read directly.
 */
float wheel_encoder_update(wheel_encoder_t *enc);

#ifdef __cplusplus
}
#endif

#endif /* WHEEL_ENCODER_H */
