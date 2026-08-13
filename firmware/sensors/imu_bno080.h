/**
 * imu_bno080.h - Accel/gyro from a CEVA/Hillcrest BNO080 9DOF IMU.
 *
 * Maps to simulation/vehicle_data.py channels: accel_x, accel_y, accel_z,
 * gyro_z (Hub node) - this driver actually reads gyro x/y/z and accel x/y/z
 * (6 of the 9 DOF; the BNO080 also does onboard sensor fusion for orientation
 * quaternions if you want roll/pitch/yaw later, not wired up here since the
 * current channel set only asks for raw accel + yaw rate).
 *
 * IMPORTANT - this is the most involved sensor in this set. The BNO080 does
 * not expose simple "read a register, get accel" access like the MLX90614 -
 * it speaks CEVA's SH-2 protocol over an SHTP (Sensor Hub Transport
 * Protocol) framing layer on I2C. You have to explicitly "enable" each
 * report type (accelerometer, gyroscope) with a Set Feature command before
 * the sensor starts pushing data, then parse incoming SHTP packets for
 * matching report IDs.
 *
 * This driver implements that plumbing, but the fixed-point scaling
 * constants (Q points) for converting raw sensor values to physical units
 * are filled in from the SH-2 reference manual and are the single thing
 * most worth double-checking against the datasheet during bring-up, since
 * a wrong Q point silently produces values that are off by a power of 2
 * rather than an obvious garbage read.
 *
 * TODO before wiring: confirm I2C address (0x4A with ADDR pin low, 0x4B with
 * it pulled high - check which your breakout board defaults to), confirm
 * which I2C peripheral/pins in CubeMX, and wire the INT and RST pins too -
 * RST needs a brief low pulse at power-up to bring the sensor out of reset
 * (bno080_init() below expects a HAL_GPIO reset already done by the caller,
 * before calling init, since reset timing/pin choice is board-specific).
 */

#ifndef IMU_BNO080_H
#define IMU_BNO080_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    I2C_HandleTypeDef *hi2c;    /* TODO: which I2C peripheral/instance in CubeMX */
    uint8_t             i2c_addr_7bit;  /* TODO: verify 0x4A vs 0x4B against your breakout's ADDR pin strap */
    uint8_t             seq_command;
    uint8_t             seq_control;

    float   accel_x_g, accel_y_g, accel_z_g;
    float   gyro_x_dps, gyro_y_dps, gyro_z_dps;
    bool    has_accel, has_gyro;
} bno080_t;

/**
 * Call after the RST pin has already been pulsed low then released (that
 * GPIO toggle is board-specific, so it's the caller's job, not this
 * driver's). Sends the Set Feature commands enabling accelerometer and
 * gyroscope reports. Returns true if both were sent successfully - does
 * NOT guarantee the sensor is actually producing data yet, only that the
 * I2C writes succeeded; check has_accel/has_gyro after a few
 * bno080_poll() calls to confirm data is actually arriving.
 */
bool bno080_init(bno080_t *imu, I2C_HandleTypeDef *hi2c, uint8_t i2c_addr_7bit);

/**
 * Call periodically (faster than the 500 ms telemetry period - the BNO080
 * can be configured to report at 50-100 Hz). Reads one pending SHTP packet
 * if available and updates accel_x_g/etc. Non-blocking-ish: a single I2C
 * transaction per call, returns quickly if nothing is pending.
 */
void bno080_poll(bno080_t *imu);

#ifdef __cplusplus
}
#endif

#endif /* IMU_BNO080_H */
