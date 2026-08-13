/**
 * ecvt_control.h - PI control loop for the electronically-controlled CVT
 * (E-CVT), with IMU-based hill feedforward.
 *
 * PLANT MODEL THIS ASSUMES: an actuator motor (the ODrive S1 on the
 * "Motor/E-CVT" node - see firmware/sensors/odrive_can.h) moves the CVT's
 * secondary sheave to change ratio. This module does NOT drive that motor
 * directly; it produces a velocity setpoint in turns/s that the caller hands
 * to the ODrive in velocity control mode (odrive_can_t already speaks in
 * turns/s, so the units line up with no conversion). The plant being
 * regulated is engine RPM: change the ratio, and for a given road speed the
 * engine RPM moves. This is the standard Baja E-CVT strategy - hold engine
 * RPM in the powerband, let road speed follow.
 *
 * OUTPUT SIGN CONVENTION (pick one and be consistent - this is it):
 *   output > 0  ->  actuator moves the CVT toward a LOWER ratio (downshift),
 *                   which raises engine RPM for a given road speed.
 *   output < 0  ->  actuator moves toward a HIGHER ratio (upshift), which
 *                   lowers engine RPM for a given road speed.
 *   output == 0 ->  hold the current ratio.
 * This is a design choice, not a measurement - if your actuator's physical
 * wiring/gearing makes positive turns/s do the opposite, either flip the
 * sign in the caller before handing the command to odrive_can, or negate
 * kp/ki/ff_gain here. Get this right on the bench (command a known small
 * positive value, confirm the sheave moves the expected direction) before
 * ever running this closed-loop with the engine running.
 *
 * ASSUMED PLANT: this assumes a RATE-controlled (velocity) actuator, where
 * commanding 0 holds position. Some Baja E-CVT builds instead use a
 * POSITION-controlled actuator (command an absolute ratio/sheave position).
 * If yours is a position actuator, this module still gets you most of the
 * way there (the PI math and feedforward are the same), but you'd want the
 * output integrated into a position setpoint by the caller, or restructured
 * to output position directly - worth deciding once the actual actuator
 * hardware is chosen, noted here so it isn't a silent assumption.
 *
 * CONTROL STRUCTURE:
 *
 *   target_rpm ----> (+)
 *                     |  error
 *                     v
 *              [ PI controller ] ----> (+) ----> [clamp] ----> output_turns_s
 *                     ^                 ^
 *                     |                 |
 *              measured_rpm      [ IMU feedforward ]
 *                                       ^
 *                                       |
 *                            accel_x_g, gyro_z_dps, wheel_speed_mph
 *
 * See firmware/control/README.md for the full ASCII block diagram, what each
 * tuning constant does, starting values, and the on-vehicle tuning procedure.
 * Read that before touching kp/ki/ff_gain in ecvt_control_config_t.
 *
 * IMU FEEDFORWARD - THE CORE PROBLEM AND HOW THIS SOLVES IT:
 * A 3-axis accelerometer bolted to the chassis cannot tell "I'm tilted
 * nose-up on a hill" apart from "I'm on flat ground and slowing down" - both
 * read as a negative-going shift on the longitudinal (X) axis relative to
 * steady level cruise, because an accelerometer measures specific force
 * (gravity's reaction plus true acceleration), not tilt angle directly. The
 * standard fix, used here: subtract the vehicle's TRUE longitudinal
 * acceleration - estimated from the rate of change of measured wheel speed,
 * which has nothing to do with gravity - from the raw accel_x_g reading.
 * What's left over is (approximately) the gravity component due to grade:
 *
 *     accel_x_g_measured  ~=  sin(grade_angle) + a_true_g
 *     grade_g             =   accel_x_g_measured - a_true_g
 *     grade_pct           ~=  100 * grade_g          (small-angle approx,
 *                                                      see caveats below)
 *
 * ASSUMPTIONS AND FAILURE MODES OF THAT SUBTRACTION - read this before
 * trusting the feedforward on the vehicle:
 *   - Wheel slip: a_true_g comes from driven-wheel (or whichever wheel_speed
 *     input you feed in) speed, not true chassis speed. Wheelspin off the
 *     line or a locked wheel under hard braking makes a_true_g wrong, which
 *     makes grade_g wrong, which makes the feedforward push the wrong way at
 *     exactly the moment traction is already marginal. Not detected or
 *     compensated here - a real implementation should compare against GPS
 *     ground speed or cross-check multiple wheels before trusting this
 *     during a slip event. Flagged as a known gap, not solved.
 *   - Hard cornering: lateral maneuvers couple into the longitudinal axis
 *     through any IMU mounting misalignment and through the vehicle's own
 *     roll/pitch coupling during a corner. This module's mitigation is
 *     crude but cheap: while |gyro_z_dps| exceeds
 *     cfg->max_yaw_rate_dps_for_grade, the grade estimate is FROZEN (held at
 *     its last value, not updated) rather than trusted fresh - grade doesn't
 *     change meaningfully over a few seconds of cornering, so holding is a
 *     reasonable approximation, but it is an approximation.
 *   - Small-angle approximation: percent grade is technically
 *     100*tan(theta), this code uses 100*sin(theta) because that's what the
 *     accelerometer actually measures. They agree closely at moderate grades
 *     (at 15% grade / 8.5 deg, sin and tan differ by about 1%; at 30% grade /
 *     16.7 deg, about 4%) and diverge more steeply beyond that. Fine for a
 *     feedforward nudge, not a precision grade sensor.
 *   - Sensor mounting orientation: see cfg->invert_accel_x above. The sign
 *     convention this file assumes (positive accel_x = nose-up or
 *     accelerating forward, i.e. the "pressed back into your seat" reaction
 *     force convention) must be verified against your actual BNO080 mounting
 *     during bring-up - see firmware/sensors/imu_bno080.h for the driver
 *     this reads from.
 *   - This is a FEEDFORWARD, not a ground-truth grade sensor. It exists to
 *     make the loop respond faster, not to replace the PI feedback term -
 *     the PI term still closes the loop on whatever the feedforward gets
 *     wrong.
 *
 * SAFETY / DEGRADED OPERATION:
 * A control loop acting on stale sensor data on a moving vehicle is a real
 * hazard, so every input has an explicit validity flag and this module
 * degrades deliberately rather than silently using old numbers - see the
 * per-input handling in ecvt_control_step()'s doc comment below and in
 * ecvt_control.c. Short version: lose the RPM feedback and the output is
 * forced to a safe zero (hold ratio, stop trying to close the loop blind);
 * lose the IMU or wheel-speed input and only the feedforward is disabled
 * (the PI feedback path keeps working on whatever RPM feedback it still
 * has) - losing "anticipation" is a performance hit, losing "correctness of
 * the feedback loop itself" is a safety issue, and those two failures are
 * handled differently on purpose.
 *
 * HAL-FREE BY DESIGN: no stm32*_hal.h include anywhere in this module, and
 * no wall-clock reads - every call takes dt_s explicitly from the caller.
 * This is what makes tools/test_ecvt_control.c able to run this on a PC
 * with a simulated clock (see tools/hal_shim/ for the same pattern used by
 * the rest of this repo's host tests). Do NOT hardcode an assumed loop
 * period anywhere in here; the caller may run this at 100 Hz on one board
 * and 20 Hz on another, and a hardcoded dt is exactly the kind of mistake
 * that mistunes a controller silently instead of loudly.
 *
 * FLOATING POINT NOTE: this file uses `float` throughout for the control
 * math. That's a deliberate trade - control loops are much easier to reason
 * about (and to keep numerically well-behaved across a wide range of RPM
 * error and dt) in floating point than in fixed-point. The catch: the
 * STM32F103 (Bluepill) has no hardware FPU, so every multiply/divide here
 * becomes a software-emulated libgcc call on that part - fine at a control
 * loop's typical rate (tens to low hundreds of Hz), but don't call
 * ecvt_control_step() from a tight ISR or a high-rate CAN-receive path on an
 * F103. If the E-CVT node ends up on an FPU-equipped part (the Nucleo hub is
 * an L4, which does have one) this is a non-issue either way.
 *
 * C99, no dynamic allocation, no libc dependency beyond <math.h>'s fabsf.
 */

#ifndef ECVT_CONTROL_H
#define ECVT_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Every constant here is a tuning value with a TODO - none of these are safe
 * to trust untuned on the actual vehicle. See firmware/control/README.md for
 * a starting value and reasoning for each, and the recommended on-vehicle
 * tuning order (PI first with feedforward disabled, then feedforward).
 */
typedef struct {
    /* ---- PI feedback gains (error is in RPM, output is in turns/s) ---- */
    float kp;   /* turns/s of output per RPM of error. TODO: tune on vehicle, see README. */
    float ki;   /* turns/s of output per (RPM * second) of accumulated error. TODO: tune, start at 0.0 and add only once kp alone is stable. */

    /* ---- anti-windup ---- */
    float integrator_limit;   /* symmetric clamp on the integrator's own contribution, in turns/s (same units as output). TODO: tune, a reasonable starting point is well under output_max so the I-term alone can never saturate the actuator. */

    /* ---- actuator range - MUST match the real ODrive vel_limit / mechanical sheave travel, not a guess ---- */
    float output_min_turns_s;   /* most negative (fastest upshift) command this module will ever return. TODO: set from ODrive config + mechanical limits. */
    float output_max_turns_s;   /* most positive (fastest downshift) command. TODO: same. */

    /* ---- IMU hill feedforward ---- */
    float ff_gain_turns_s_per_pct_grade;   /* how hard to preemptively downshift per percent of grade. TODO: tune, see README. */
    float grade_deadband_pct;              /* |grade| estimates below this are treated as 0 - covers sensor noise, flat-ground camber, and imperfect accel/wheel-speed cancellation on flat ground. TODO: tune, start around 1.5-2.0. */
    float max_yaw_rate_dps_for_grade;      /* while turn rate exceeds this, freeze (don't update) the grade estimate - see header note on cornering. TODO: tune, start around 20-25 deg/s. */
    bool  invert_accel_x;                  /* flip the sign of accel_x_g before use - set this from bench testing, see header note on mounting orientation. */

    /* ---- safety / staleness ---- */
    float stale_timeout_s;   /* how long any one input (RPM, IMU, wheel speed) may go without a fresh sample before this module treats it as lost and degrades - see .c for exactly what "degrades" means per input. TODO: tune to a few missed update periods for whatever rate actually feeds this loop, NOT the 2 Hz telemetry downlink rate - this should run off a faster local feed (CAN/UART poll), not the ground-station link. */
} ecvt_control_config_t;

/**
 * Internal state carried between calls - one instance per E-CVT loop
 * instance (there is only ever one CVT per car, but keeping this as
 * caller-owned state rather than a global is what makes host testing
 * possible: tools/test_ecvt_control.c runs several independent scenarios
 * back to back with fresh state each time).
 *
 * grade_estimate_pct and degraded are safe/intended to read directly for
 * telemetry/dashboard purposes (same pattern as e.g. wheel_encoder_t's
 * cached speed_mph field) - everything else here is internal, do not set
 * directly.
 */
typedef struct {
    float integrator;              /* internal */
    float last_output_turns_s;     /* internal - last value returned by ecvt_control_step() */
    float grade_estimate_pct;      /* last computed or held road-grade estimate, +uphill. Safe to read for telemetry. */
    float prev_wheel_speed_mph;    /* internal */
    bool  has_prev_wheel_speed;    /* internal */
    float rpm_stale_timer_s;       /* internal */
    float imu_stale_timer_s;       /* internal */
    float wheel_stale_timer_s;     /* internal */
    bool  degraded;                /* true if the most recent ecvt_control_step() call had feedback and/or feedforward disabled due to bad/stale input. Safe to read for telemetry/fault lamps. */
} ecvt_control_state_t;

/**
 * Zero-initializes all state. Call once at startup, and also safe to call
 * any time you want a clean restart (e.g. after a manual-override period
 * where the driver was controlling the CVT directly and you don't want a
 * stale integrator to cause a jump when autonomy re-engages).
 */
void ecvt_control_init(ecvt_control_state_t *st);

/**
 * Run one control step. Call at whatever rate your board's main loop or
 * timer allows - there is no assumed rate, dt_s carries all timing
 * information explicitly (see header note on why a hardcoded dt is
 * dangerous). Returns the actuator velocity command in turns/s, per the sign
 * convention documented at the top of this file - hand it directly to
 * whatever calls the ODrive's velocity-setpoint CAN command.
 *
 * Parameters:
 *   target_rpm        - desired engine RPM (e.g. the engine's peak-torque
 *                        point). Caller's choice how this is derived - fixed
 *                        constant, throttle-position-scheduled, etc.
 *   measured_rpm       - current engine RPM from your tach source. NOTE:
 *                        this repo does not yet have an engine RPM driver
 *                        under firmware/sensors/ - this module only consumes
 *                        the value, wiring up the actual sensor (ignition
 *                        pickup, Hall sensor on the flywheel, etc.) is a
 *                        separate task.
 *   rpm_valid          - false if this call has no fresh RPM reading (sensor
 *                        fault, out-of-range value your caller already
 *                        rejected, etc.)
 *   accel_x_g          - BNO080 longitudinal accel, see firmware/sensors/
 *                        imu_bno080.h (accel_x_g field). Sign convention per
 *                        the header note above.
 *   gyro_z_dps         - BNO080 yaw rate, same driver (gyro_z_dps field).
 *                        Used only to gate the grade estimate during
 *                        cornering, see header note.
 *   imu_valid           - false if this call has no fresh IMU reading.
 *   wheel_speed_mph     - current wheel speed, see firmware/sensors/
 *                        wheel_encoder.h. Used both to derive true
 *                        longitudinal acceleration for the grade estimate.
 *                        Pick one wheel consistently (a non-driven wheel is
 *                        the better choice if you have one, since it's less
 *                        prone to wheelspin under power - see header note on
 *                        wheel slip).
 *   wheel_speed_valid   - false if this call has no fresh wheel-speed
 *                        reading.
 *   dt_s                - elapsed time in seconds since the previous call to
 *                        this function. Must be > 0; if the caller can't
 *                        supply a valid dt (e.g. first call ever, or a clock
 *                        glitch), this returns the safe last-known output
 *                        rather than doing undefined math.
 *
 * Returns: actuator velocity command in turns/s, already clamped to
 * [cfg->output_min_turns_s, cfg->output_max_turns_s] (or forced to 0.0 in
 * the RPM-lost safety case - see .c for why 0 and not "last output" is the
 * correct fallback here).
 */
float ecvt_control_step(const ecvt_control_config_t *cfg,
                         ecvt_control_state_t *st,
                         float target_rpm,
                         float measured_rpm,
                         bool  rpm_valid,
                         float accel_x_g,
                         float gyro_z_dps,
                         bool  imu_valid,
                         float wheel_speed_mph,
                         bool  wheel_speed_valid,
                         float dt_s);

#ifdef __cplusplus
}
#endif

#endif /* ECVT_CONTROL_H */
