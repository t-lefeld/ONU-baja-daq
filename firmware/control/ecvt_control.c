/**
 * ecvt_control.c - see ecvt_control.h for the control structure, sign
 * conventions, and the full reasoning behind the grade-estimation and
 * safety-degradation choices made here. Comments in this file focus on the
 * "how", the header has the "why".
 */

#include "ecvt_control.h"
#include <string.h>
#include <math.h>

/* mph/s that corresponds to 1 g of longitudinal acceleration. Pure unit
 * conversion (1 g = 32.174 ft/s^2 = 21.937 mph/s) - NOT a tuning constant,
 * do not "tune" this. Matches simulation/vehicle_data.py's MPH_PER_G_PER_S
 * so the two stay consistent in spirit, though this file does not include
 * that module or depend on it. */
#define ECVT_MPH_PER_G_PER_S 21.937f

static float clampf(float v, float lo, float hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

void ecvt_control_init(ecvt_control_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

/**
 * Updates st->grade_estimate_pct (or deliberately leaves it held, see
 * comments inline) and returns the feedforward contribution in turns/s,
 * already deadbanded. Also sets *sensor_lost to true if the IMU or
 * wheel-speed input has been missing for longer than cfg->stale_timeout_s -
 * the caller uses that to fold into st->degraded.
 *
 * This is a static helper, not part of the public API - the grade math is
 * only meaningful bundled with the staleness/cornering gating around it, so
 * there is no standalone "just compute grade" entry point. Read
 * st->grade_estimate_pct after calling ecvt_control_step() if you want the
 * value for telemetry.
 */
static float grade_feedforward_step(const ecvt_control_config_t *cfg,
                                     ecvt_control_state_t *st,
                                     float accel_x_g, bool imu_valid,
                                     float gyro_z_dps,
                                     float wheel_speed_mph, bool wheel_speed_valid,
                                     float dt_s,
                                     bool *sensor_lost)
{
    /* Staleness bookkeeping is independent of whether THIS call's flags are
     * valid - a single missed frame should not instantly declare a sensor
     * "lost", but stale_timeout_s of consecutive misses should. */
    st->imu_stale_timer_s   = imu_valid        ? 0.0f : (st->imu_stale_timer_s + dt_s);
    st->wheel_stale_timer_s = wheel_speed_valid ? 0.0f : (st->wheel_stale_timer_s + dt_s);

    *sensor_lost = (st->imu_stale_timer_s   > cfg->stale_timeout_s) ||
                   (st->wheel_stale_timer_s > cfg->stale_timeout_s);

    bool cornering = fabsf(gyro_z_dps) > cfg->max_yaw_rate_dps_for_grade;

    /* Only recompute the grade estimate when we have a fresh accel reading,
     * a fresh wheel-speed reading, a previous wheel-speed sample to
     * differentiate against, and we are not mid-corner. Otherwise HOLD the
     * last estimate rather than update it with a bad number - see header
     * for why holding (not zeroing) is correct for the transient cases. */
    if (imu_valid && wheel_speed_valid && !cornering && st->has_prev_wheel_speed)
    {
        float ax_g = cfg->invert_accel_x ? -accel_x_g : accel_x_g;

        /* True longitudinal acceleration, estimated from the wheel-speed
         * derivative - this has nothing to do with gravity/tilt, which is
         * exactly why subtracting it from the raw accel reading isolates
         * the grade component. See header for the wheel-slip caveat: if
         * this wheel is spinning or locked, this derivative (and therefore
         * the grade estimate) is wrong. */
        float a_true_g = (wheel_speed_mph - st->prev_wheel_speed_mph) / dt_s / ECVT_MPH_PER_G_PER_S;

        float grade_g = ax_g - a_true_g;

        /* percent grade ~= 100 * sin(angle), which is what the accelerometer
         * actually measures (see header for the sin-vs-tan small-angle
         * caveat). */
        st->grade_estimate_pct = 100.0f * grade_g;
    }

    if (wheel_speed_valid)
    {
        st->prev_wheel_speed_mph = wheel_speed_mph;
        st->has_prev_wheel_speed = true;
    }

    if (*sensor_lost)
    {
        /* Genuine, sustained sensor loss: feedforward-off. We keep
         * grade_estimate_pct at its last value for telemetry/debugging
         * visibility (so a dashboard can show "last known grade: 12%,
         * stale"), but we do NOT use it to command the actuator once it's
         * old enough to no longer be trustworthy. */
        return 0.0f;
    }

    float g = st->grade_estimate_pct;
    if (g > -cfg->grade_deadband_pct && g < cfg->grade_deadband_pct)
    {
        return 0.0f;
    }

    return cfg->ff_gain_turns_s_per_pct_grade * g;
}

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
                         float dt_s)
{
    /* A caller bug (dt<=0) would divide-by-zero in the grade derivative and
     * run the integrator backward. Fail safe rather than compute garbage:
     * return the last known-good output and flag degraded. This should
     * never happen in normal operation (dt_s comes from the caller's own
     * tick source), so treat repeated occurrences of this as a bug to fix
     * in the caller, not something to tune around here. */
    if (dt_s <= 0.0f)
    {
        st->degraded = true;
        return st->last_output_turns_s;
    }

    bool ff_sensor_lost = false;
    float ff = grade_feedforward_step(cfg, st, accel_x_g, imu_valid, gyro_z_dps,
                                       wheel_speed_mph, wheel_speed_valid, dt_s,
                                       &ff_sensor_lost);

    /* RPM feedback staleness - handled separately from (and more strictly
     * than) the feedforward inputs above, because losing this one breaks
     * the feedback loop itself, not just the "anticipate before it happens"
     * bonus on top of it. */
    st->rpm_stale_timer_s = rpm_valid ? 0.0f : (st->rpm_stale_timer_s + dt_s);
    bool rpm_lost = (!rpm_valid) || (st->rpm_stale_timer_s > cfg->stale_timeout_s);

    if (rpm_lost)
    {
        /* Safety fallback: command ZERO actuator velocity, not "hold the
         * last nonzero command." The output of this module is a RATE
         * (turns/s) to a velocity-controlled actuator, not a position -
         * continuing to command whatever nonzero rate was last computed
         * while blind to RPM feedback would keep shifting the CVT ratio
         * with no closed-loop correction at all, which is the actual
         * hazard on a moving vehicle. Commanding 0 turns/s freezes the
         * ratio actuator wherever it currently sits: a safe, inspectable
         * state, and the one a driver/teammate can act on (get off the
         * throttle, coast, diagnose) rather than an actuator that keeps
         * quietly shifting on stale information.
         *
         * The integrator is intentionally left untouched here (frozen, not
         * zeroed) so that if the RPM feed returns a moment later, control
         * resumes from roughly where it left off instead of re-winding a
         * fresh integral from zero - a brief CAN glitch shouldn't cost you
         * your accumulated trim. */
        st->degraded = true;
        st->last_output_turns_s = 0.0f;
        return 0.0f;
    }

    float error = target_rpm - measured_rpm;
    float unclamped = cfg->kp * error + st->integrator + ff;
    float output = clampf(unclamped, cfg->output_min_turns_s, cfg->output_max_turns_s);

    /* Anti-windup, two layers used together deliberately:
     *
     * 1) Hard clamp on the integrator's own contribution
     *    (cfg->integrator_limit). This bounds how much authority the I-term
     *    can ever have, independent of whatever the P-term and feedforward
     *    are doing - protects against a bad kp/ki pairing driving the
     *    integrator to an extreme value even before the combined output
     *    saturates.
     *
     * 2) Conditional integration: once the UNCLAMPED combined output has
     *    exceeded the actuator's range, and the current error would push it
     *    further into that same saturation, stop accumulating error into
     *    the integrator this step. This is the standard fix for classical
     *    integrator windup (the I-term racing far past what the actuator
     *    can use, then taking a long time to unwind once the error
     *    reverses).
     *
     * Chosen over back-calculation (feeding the saturation error back
     * through an extra tracking-time-constant gain) because it needs one
     * fewer tuning constant and is easier to reason about for a first base
     * implementation - see README for a note on revisiting this if
     * conditional integration proves too coarse once tuned on the real
     * actuator. */
    bool saturated_high = unclamped > cfg->output_max_turns_s;
    bool saturated_low  = unclamped < cfg->output_min_turns_s;
    bool would_worsen_saturation = (saturated_high && error > 0.0f) ||
                                    (saturated_low  && error < 0.0f);

    if (!would_worsen_saturation)
    {
        st->integrator += cfg->ki * error * dt_s;
        st->integrator = clampf(st->integrator, -cfg->integrator_limit, cfg->integrator_limit);
    }

    st->degraded = ff_sensor_lost;
    st->last_output_turns_s = output;
    return output;
}
