/**
 * cvt_temp_mlx90614.c - see cvt_temp_mlx90614.h for the sensor placement TODO.
 */

#include "cvt_temp_mlx90614.h"

#define MLX90614_I2C_ADDR_7BIT   0x5Au
#define MLX90614_REG_TOBJ1       0x07u
#define I2C_TIMEOUT_MS           50u

void cvt_temp_init(cvt_temp_sensor_t *s, I2C_HandleTypeDef *hi2c)
{
    s->hi2c = hi2c;
}

bool cvt_temp_read(cvt_temp_sensor_t *s, float *out_degc)
{
    uint8_t buf[3]; /* low byte, high byte, PEC - PEC is read but not checked */

    /* HAL's DevAddress is the 7-bit address pre-shifted left by 1 - do not
     * pass the raw 0x5A, HAL_I2C expects (addr << 1). */
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(
        s->hi2c,
        (uint16_t)(MLX90614_I2C_ADDR_7BIT << 1),
        MLX90614_REG_TOBJ1,
        I2C_MEMADD_SIZE_8BIT,
        buf, sizeof(buf),
        I2C_TIMEOUT_MS
    );

    if (st != HAL_OK)
    {
        return false;
    }

    uint16_t raw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

    /* Top bit is an error flag on some MLX90614 variants - if set, the
     * reading is not valid (e.g. thermocouple input open on -BAA variants,
     * not relevant to the standard TO-can package used here, but cheap to
     * guard against). */
    if (raw & 0x8000u)
    {
        return false;
    }

    float kelvin = (float)raw * 0.02f;
    *out_degc = kelvin - 273.15f;
    return true;
}
