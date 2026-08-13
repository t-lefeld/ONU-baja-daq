/**
 * cvt_temp_mlx90614.h - E-CVT temperature from a Melexis MLX90614 IR
 * non-contact temperature sensor.
 *
 * Maps to simulation/vehicle_data.py channel: cvt_temp (Rear node).
 *
 * The MLX90614 talks SMBus/I2C at a fixed default 7-bit address of 0x5A
 * (configurable via EEPROM if you ever need multiple on one bus, not needed
 * here since there's only one). Object temperature lives in RAM register
 * 0x07 as a 16-bit word in units of 0.02 K - this driver does the Kelvin ->
 * Celsius conversion for you.
 *
 * TODO before wiring: confirm which I2C peripheral/pins this sensor is on
 * in CubeMX (SCL/SDA), and mount the sensor with a clear, unobstructed
 * line of sight to the CVT housing at the datasheet's rated distance -
 * IR temperature sensors are sensitive to field-of-view and target
 * emissivity, so verify the reading against a contact thermometer once
 * during bring-up.
 */

#ifndef CVT_TEMP_MLX90614_H
#define CVT_TEMP_MLX90614_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f1xx_hal.h"   /* TODO: swap to stm32l4xx_hal.h if this instance runs on the Nucleo hub */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    I2C_HandleTypeDef *hi2c;   /* TODO: which I2C peripheral/instance in CubeMX */
} cvt_temp_sensor_t;

void cvt_temp_init(cvt_temp_sensor_t *s, I2C_HandleTypeDef *hi2c);

/**
 * Reads the object-temperature RAM register. Returns true and fills
 * *out_degc on success; returns false (leaves *out_degc untouched) on an
 * I2C error, so the caller can hold the last-known-good value instead of
 * reporting a bogus 0.0.
 */
bool cvt_temp_read(cvt_temp_sensor_t *s, float *out_degc);

#ifdef __cplusplus
}
#endif

#endif /* CVT_TEMP_MLX90614_H */
