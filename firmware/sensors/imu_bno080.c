/**
 * imu_bno080.c - minimal SH-2/SHTP driver: enables accelerometer + gyroscope
 * reports and parses them. See imu_bno080.h for the Q-point/address TODOs
 * to verify against the datasheet during bring-up.
 *
 * Not implemented (out of scope for the current channel set, but the SHTP
 * plumbing here is the same either way if you add these later): rotation
 * vector / quaternion output, magnetometer, DFU, calibration commands.
 */

#include "imu_bno080.h"
#include <string.h>

#define SHTP_CHAN_COMMAND   0u
#define SHTP_CHAN_CONTROL   2u
#define SHTP_CHAN_REPORTS   3u

#define SH2_SET_FEATURE_CMD 0xFDu
#define SH2_REPORT_ACCEL    0x01u
#define SH2_REPORT_GYRO     0x02u
#define SH2_BASE_TIMESTAMP  0xFBu

#define I2C_TIMEOUT_MS      20u
#define RX_BUF_LEN          32u   /* TODO: enlarge if you enable additional/batched report types later */

/* Q points from the SH-2 reference manual - accelerometer reports in units
 * of 2^-8 m/s^2, gyroscope in units of 2^-9 rad/s. VERIFY against your
 * BNO080 firmware version's datasheet before trusting a bring-up reading
 * that looks "close but not quite right" - a Q-point mismatch looks exactly
 * like that (values off by a clean power of 2). */
#define ACCEL_Q_POINT   8
#define GYRO_Q_POINT    9
#define MPS2_PER_G      9.80665f
#define RAD_PER_DEG     0.0174533f

static float q_to_float(int16_t raw, int q_point)
{
    return (float)raw / (float)(1 << q_point);
}

static bool shtp_write(bno080_t *imu, uint8_t channel, const uint8_t *payload, uint16_t len, uint8_t *seq)
{
    uint8_t buf[4 + 32];   /* control-channel commands here are all well under 32 bytes */
    if (len > sizeof(buf) - 4u) return false;

    uint16_t total = (uint16_t)(len + 4u);
    buf[0] = (uint8_t)(total & 0xFFu);
    buf[1] = (uint8_t)((total >> 8) & 0x7Fu);   /* top bit = continuation, unused for these short writes */
    buf[2] = channel;
    buf[3] = (*seq)++;
    memcpy(&buf[4], payload, len);

    HAL_StatusTypeDef st = HAL_I2C_Master_Transmit(
        imu->hi2c, (uint16_t)(imu->i2c_addr_7bit << 1),
        buf, total, I2C_TIMEOUT_MS
    );
    return st == HAL_OK;
}

static bool send_set_feature(bno080_t *imu, uint8_t feature_report_id, uint32_t interval_us)
{
    uint8_t payload[17] = {0};
    payload[0] = SH2_SET_FEATURE_CMD;
    payload[1] = feature_report_id;
    payload[2] = 0u;                       /* feature flags */
    payload[3] = 0u; payload[4] = 0u;      /* change sensitivity */
    payload[5]  = (uint8_t)( interval_us        & 0xFFu);
    payload[6]  = (uint8_t)((interval_us >> 8)  & 0xFFu);
    payload[7]  = (uint8_t)((interval_us >> 16) & 0xFFu);
    payload[8]  = (uint8_t)((interval_us >> 24) & 0xFFu);
    /* bytes 9-16: batch interval + sensor-specific config, left at 0 */

    return shtp_write(imu, SHTP_CHAN_CONTROL, payload, sizeof(payload), &imu->seq_control);
}

bool bno080_init(bno080_t *imu, I2C_HandleTypeDef *hi2c, uint8_t i2c_addr_7bit)
{
    memset(imu, 0, sizeof(*imu));
    imu->hi2c           = hi2c;
    imu->i2c_addr_7bit    = i2c_addr_7bit;

    /* 20000 us = 20 ms = 50 Hz report rate - comfortably faster than the
     * 500 ms telemetry frame period, plenty of headroom to average/smooth
     * if the raw rate turns out noisier than expected. */
    bool ok_accel = send_set_feature(imu, SH2_REPORT_ACCEL, 20000u);
    bool ok_gyro  = send_set_feature(imu, SH2_REPORT_GYRO, 20000u);

    return ok_accel && ok_gyro;
}

static void parse_report(bno080_t *imu, const uint8_t *r, uint16_t remaining, uint16_t *consumed)
{
    uint8_t report_id = r[0];

    if (report_id == SH2_BASE_TIMESTAMP)
    {
        *consumed = 5u;   /* reportID + 4-byte timestamp delta */
        return;
    }

    if ((report_id == SH2_REPORT_ACCEL || report_id == SH2_REPORT_GYRO) && remaining >= 10u)
    {
        int16_t x = (int16_t)((uint16_t)r[4] | ((uint16_t)r[5] << 8));
        int16_t y = (int16_t)((uint16_t)r[6] | ((uint16_t)r[7] << 8));
        int16_t z = (int16_t)((uint16_t)r[8] | ((uint16_t)r[9] << 8));

        if (report_id == SH2_REPORT_ACCEL)
        {
            imu->accel_x_g = q_to_float(x, ACCEL_Q_POINT) / MPS2_PER_G;
            imu->accel_y_g = q_to_float(y, ACCEL_Q_POINT) / MPS2_PER_G;
            imu->accel_z_g = q_to_float(z, ACCEL_Q_POINT) / MPS2_PER_G;
            imu->has_accel = true;
        }
        else
        {
            imu->gyro_x_dps = q_to_float(x, GYRO_Q_POINT) / RAD_PER_DEG;
            imu->gyro_y_dps = q_to_float(y, GYRO_Q_POINT) / RAD_PER_DEG;
            imu->gyro_z_dps = q_to_float(z, GYRO_Q_POINT) / RAD_PER_DEG;
            imu->has_gyro = true;
        }

        *consumed = 10u;
        return;
    }

    /* Unrecognized or truncated report - bail out rather than mis-parse the
     * rest of the packet. Only relevant if you enable additional report
     * types beyond accel/gyro later. */
    *consumed = remaining;
}

void bno080_poll(bno080_t *imu)
{
    uint8_t buf[RX_BUF_LEN] = {0};

    if (HAL_I2C_Master_Receive(imu->hi2c, (uint16_t)(imu->i2c_addr_7bit << 1),
                                buf, RX_BUF_LEN, I2C_TIMEOUT_MS) != HAL_OK)
    {
        return;
    }

    uint16_t length = (uint16_t)(buf[0] | ((buf[1] & 0x7Fu) << 8));
    if (length <= 4u || length > RX_BUF_LEN)
    {
        return;   /* empty packet, or bigger than our fixed read buffer (see RX_BUF_LEN TODO) */
    }

    uint8_t channel = buf[2];
    if (channel != SHTP_CHAN_REPORTS)
    {
        return;   /* advertisements/acks on other channels - nothing to parse for our purposes */
    }

    uint16_t payload_len = (uint16_t)(length - 4u);
    const uint8_t *p = &buf[4];
    uint16_t offset = 0u;

    while (offset < payload_len)
    {
        uint16_t consumed = 0u;
        parse_report(imu, &p[offset], (uint16_t)(payload_len - offset), &consumed);
        if (consumed == 0u) break;
        offset = (uint16_t)(offset + consumed);
    }
}
