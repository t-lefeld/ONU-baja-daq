/**
 * odrive_can.h - Decodes ODrive S1's native CAN Simple protocol messages and
 * repackages the values into plain floats for the telemetry frame, per your
 * choice to keep motor data in the same TLM frame format as everything else
 * rather than relaying ODrive's raw CAN traffic separately.
 *
 * Maps to simulation/vehicle_data.py channels: motor_current, motor_velocity,
 * motor_temp, bus_voltage, brake_resistor_w (Motor/E-CVT node).
 *
 * The ODrive S1 is itself already a CAN node on the same bus - it is NOT
 * behind a Bluepill like the wheel/suspension/pressure sensors, so this
 * driver just needs to listen for the ODrive's own CAN Simple messages on
 * whichever bus the hub is already receiving from, and does not own a CAN
 * peripheral itself. Call odrive_can_handle_frame() from the hub's existing
 * CAN receive path for every frame that arrives, and it filters for the
 * ODrive's node ID internally.
 *
 * ODrive CAN Simple message IDs are (axis_node_id << 5) | cmd_id - the
 * cmd_id constants below are from the ODrive CAN protocol reference
 * (docs.odriverobotics.com/can-protocol) for firmware 0.6.x:
 *   Get_Encoder_Estimates      0x09  ->  Pos_Estimate (float), Vel_Estimate (float, turns/s)
 *   Get_Iq                     0x14  ->  Iq_Setpoint (float), Iq_Measured (float, A)
 *   Get_Bus_Voltage_Current    0x17  ->  Bus_Voltage (float), Bus_Current (float)
 *   Get_Temperature            TODO - VERIFY against your exact ODrive firmware's
 *                              can_simple.dbc (shipped with the firmware release,
 *                              in the ODrive repo's Firmware/ folder) - the cmd_id
 *                              and payload order below (FET_Temperature,
 *                              Motor_Temperature, both float) are a best guess
 *                              from the docs and NOT independently confirmed the
 *                              way the other three IDs above are.
 *
 * There is no dedicated "brake resistor power" CAN message in the standard
 * ODrive protocol. brake_resistor_w below is an ESTIMATE: bus_voltage times
 * however much bus current is flowing back INTO the bus during regen
 * braking (negative Bus_Current), on the assumption that's roughly what the
 * brake resistor is burning off. This is a rough proxy, not a measurement -
 * good enough for a dashboard glance, not for verifying resistor sizing.
 *
 * TODO before wiring: confirm the ODrive's configured axis node ID (set via
 * odrivetool or the GUI, whatever you configured it as - this driver needs
 * to know it to filter the right CAN IDs), and confirm the CAN bus bit rate
 * matches between the ODrive's config and this project's bxCAN init (both
 * need to agree, typically 250kbps or 500kbps - check what the rest of this
 * project's CAN nodes already use in the .ioc file and match it).
 */

#ifndef ODRIVE_CAN_H
#define ODRIVE_CAN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t axis_node_id;   /* TODO: must match the ODrive's actual configured CAN node ID */

    float velocity_turns_s;
    float current_a;
    float bus_voltage_v;
    float bus_current_a;
    float fet_temp_degc;
    float motor_temp_degc;

    bool has_velocity, has_current, has_bus, has_temp;
} odrive_can_t;

void odrive_can_init(odrive_can_t *o, uint8_t axis_node_id);

/**
 * Call for every frame the hub's CAN peripheral receives (from the existing
 * HAL_CAN_RxFifo0MsgPendingCallback path or wherever this project already
 * pulls frames off the bus). Returns true if this frame belonged to the
 * configured ODrive node and was decoded; false means it was some other
 * node's frame (e.g. one of the Front/Rear Bluepills) and should be handled
 * by the existing telemetry_hub.c path exactly as it is today - this driver
 * only needs to intercept ODrive-specific frames, nothing else changes.
 */
bool odrive_can_handle_frame(odrive_can_t *o, uint32_t can_id, const uint8_t data[8], uint8_t dlc);

/** Wheel-speed-style RPM, converted from the raw turns/s the ODrive reports. */
float odrive_velocity_rpm(const odrive_can_t *o);

/** See the header comment above - an estimate, not a direct measurement. */
float odrive_brake_resistor_w(const odrive_can_t *o);

#ifdef __cplusplus
}
#endif

#endif /* ODRIVE_CAN_H */
