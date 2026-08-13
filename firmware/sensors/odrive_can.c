/**
 * odrive_can.c - see odrive_can.h for the cmd_id / node_id TODOs, especially
 * the unverified Get_Temperature command ID.
 */

#include "odrive_can.h"
#include <string.h>

#define CMD_GET_ENCODER_ESTIMATES   0x09u
#define CMD_GET_IQ                  0x14u
#define CMD_GET_TEMPERATURE         0x15u   /* TODO: UNVERIFIED - confirm against your firmware's can_simple.dbc */
#define CMD_GET_BUS_VOLTAGE_CURRENT 0x17u

void odrive_can_init(odrive_can_t *o, uint8_t axis_node_id)
{
    memset(o, 0, sizeof(*o));
    o->axis_node_id = axis_node_id;
}

static float bytes_to_float_le(const uint8_t *b)
{
    uint32_t bits = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

bool odrive_can_handle_frame(odrive_can_t *o, uint32_t can_id, const uint8_t data[8], uint8_t dlc)
{
    uint8_t node_id = (uint8_t)(can_id >> 5);
    uint8_t cmd_id  = (uint8_t)(can_id & 0x1Fu);

    if (node_id != o->axis_node_id)
    {
        return false;   /* not the ODrive - let the caller's normal CAN path handle it */
    }

    switch (cmd_id)
    {
        case CMD_GET_ENCODER_ESTIMATES:
            if (dlc >= 8u)
            {
                /* bytes 0-3 = Pos_Estimate (turns, unused here), bytes 4-7 = Vel_Estimate (turns/s) */
                o->velocity_turns_s = bytes_to_float_le(&data[4]);
                o->has_velocity = true;
            }
            return true;

        case CMD_GET_IQ:
            if (dlc >= 8u)
            {
                /* bytes 0-3 = Iq_Setpoint (unused), bytes 4-7 = Iq_Measured (A) */
                o->current_a = bytes_to_float_le(&data[4]);
                o->has_current = true;
            }
            return true;

        case CMD_GET_BUS_VOLTAGE_CURRENT:
            if (dlc >= 8u)
            {
                o->bus_voltage_v = bytes_to_float_le(&data[0]);
                o->bus_current_a = bytes_to_float_le(&data[4]);
                o->has_bus = true;
            }
            return true;

        case CMD_GET_TEMPERATURE:
            /* TODO: verify payload order too, not just the cmd_id - this
             * assumes [FET_Temperature, Motor_Temperature] as consecutive
             * floats, matching the pattern every other Get_* message here
             * uses, but this one specifically was not cross-checked. */
            if (dlc >= 8u)
            {
                o->fet_temp_degc   = bytes_to_float_le(&data[0]);
                o->motor_temp_degc = bytes_to_float_le(&data[4]);
                o->has_temp = true;
            }
            return true;

        default:
            return true;   /* still an ODrive frame, just a cmd_id this driver doesn't use */
    }
}

float odrive_velocity_rpm(const odrive_can_t *o)
{
    return o->velocity_turns_s * 60.0f;
}

float odrive_brake_resistor_w(const odrive_can_t *o)
{
    if (!o->has_bus || o->bus_current_a >= 0.0f)
    {
        return 0.0f;   /* not regenerating, or no reading yet */
    }
    return o->bus_voltage_v * (-o->bus_current_a);
}
