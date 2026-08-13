/**
 * test_ecvt_control.c - Host test for firmware/control/ecvt_control.c.
 *
 * Plain C, no HAL, builds and runs directly on the host with gcc/clang (see
 * tools/test_ecvt_control.py for the one-line build+run, matching the
 * pattern tools/test_firmware.py uses for the node/hub firmware).
 *
 * ecvt_control.c has no plant of its own - it just reacts to whatever RPM/
 * IMU/wheel-speed values it's handed each step - so this file supplies a
 * small first-order plant model to stand in for "engine + CVT ratio +
 * load", enough to prove three things a real vehicle would need proven
 * before this is trusted with an actual engine:
 *
 *   1. Flat ground: the PI loop converges to target_rpm and the integrator
 *      does not wind up past its configured limit.
 *   2. A step onto a 15% grade: the IMU feedforward term responds in the
 *      very same control step the grade appears in accel_x_g - i.e. before
 *      the lagged plant has let measured_rpm sag at all - while the PI
 *      feedback term is still near where it was at the prior steady state.
 *      That ordering (feedforward first, feedback catches the remainder)
 *      is the entire point of this module.
 *   3. Sensor dropout degrades the way the header promises: losing IMU/
 *      wheel-speed input disables ONLY the feedforward (feedback keeps
 *      controlling RPM); losing RPM feedback itself forces a safe zero
 *      output and freezes (does not zero) the integrator so a brief glitch
 *      doesn't cost accumulated trim.
 */

#include "ecvt_control.h"
#include <stdio.h>
#include <math.h>

static int checks, failures;

static void ok(const char *label, int cond)
{
    checks++;
    if (!cond)
    {
        failures++;
        printf("  FAIL %s\n", label);
    }
}

static void near(const char *label, float got, float want, float tol)
{
    checks++;
    if (fabsf(got - want) > tol)
    {
        failures++;
        printf("  FAIL %s: got %.4f want %.4f (tol %.4f)\n", label, (double)got, (double)want, (double)tol);
    }
}

/* ------------------------------------------------------------------ */
/* Toy plant: first-order lag from actuator output to engine RPM.       */
/* rpm_ss(output) = base_rpm + k_plant*output - load_rpm                */
/* rpm slews toward rpm_ss with time constant tau_s.                    */
/* load_rpm stands in for how much a grade (or any load increase) pulls */
/* the steady-state RPM down for a given CVT ratio, before the ratio    */
/* actuator has had a chance to compensate.                             */
/* ------------------------------------------------------------------ */
typedef struct {
    float rpm;
    float base_rpm;
    float k_plant;
    float load_rpm;
    float tau_s;
} plant_t;

static void plant_step(plant_t *p, float output_turns_s, float dt_s)
{
    float rpm_ss = p->base_rpm + p->k_plant * output_turns_s - p->load_rpm;
    p->rpm += (rpm_ss - p->rpm) * (dt_s / p->tau_s);
}

/* Shared config for all three scenarios - see firmware/control/README.md
 * for what each of these means and how to actually tune them on a vehicle.
 * These are made-up-but-plausible numbers sized to this toy plant, not
 * vehicle-tuned values. */
static ecvt_control_config_t make_cfg(void)
{
    ecvt_control_config_t cfg;
    cfg.kp = 0.0020f;
    cfg.ki = 0.0008f;
    cfg.integrator_limit = 2.2f;
    cfg.output_min_turns_s = -3.0f;
    cfg.output_max_turns_s = 3.0f;
    cfg.ff_gain_turns_s_per_pct_grade = 0.05f;
    cfg.grade_deadband_pct = 1.5f;
    cfg.max_yaw_rate_dps_for_grade = 25.0f;
    cfg.invert_accel_x = false;
    cfg.stale_timeout_s = 0.30f;
    return cfg;
}

/* ------------------------------------------------------------------ */
/* Scenario 1: flat ground steady state                                 */
/* ------------------------------------------------------------------ */
static void test_flat_ground_convergence(ecvt_control_config_t *cfg,
                                          ecvt_control_state_t *st,
                                          plant_t *plant)
{
    printf("-- scenario 1: flat ground steady state\n");

    const float target_rpm = 3800.0f;
    const float dt = 0.02f;   /* 50 Hz - arbitrary, this module doesn't care */

    float output = 0.0f;
    for (int i = 0; i < 1000; i++)   /* 20 simulated seconds - several plant time constants plus integrator settling */
    {
        output = ecvt_control_step(cfg, st,
                                    target_rpm, plant->rpm, true,
                                    /*accel_x_g=*/0.0f, /*gyro_z_dps=*/0.0f, true,
                                    /*wheel_speed_mph=*/25.0f, true,
                                    dt);
        plant_step(plant, output, dt);
    }

    printf("  after 20s: rpm=%.1f target=%.1f output=%.3f integrator=%.3f grade_est=%.2f%%\n",
           (double)plant->rpm, (double)target_rpm, (double)output,
           (double)st->integrator, (double)st->grade_estimate_pct);

    near("flat ground: rpm converges to target", plant->rpm, target_rpm, 10.0f);
    ok("flat ground: integrator within configured limit",
       fabsf(st->integrator) <= cfg->integrator_limit + 1e-4f);
    near("flat ground: grade estimate ~0 (within deadband)", st->grade_estimate_pct, 0.0f, 2.0f);
    ok("flat ground: feedforward not engaged (grade under deadband)",
       fabsf(st->grade_estimate_pct) < cfg->grade_deadband_pct);
    ok("flat ground: not degraded", !st->degraded);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: step onto a 15% grade                                    */
/* ------------------------------------------------------------------ */
static void test_grade_step_feedforward_leads(ecvt_control_config_t *cfg,
                                               ecvt_control_state_t *st,
                                               plant_t *plant)
{
    printf("-- scenario 2: step onto a 15%% grade\n");

    const float target_rpm = 3800.0f;
    const float dt = 0.02f;

    /* One more steady-cruise step immediately before the hill starts, purely
     * to get a clean "before" output with the disturbance not yet applied
     * (scenario 1 already converged the plant/integrator to steady state -
     * this just samples that state cleanly). */
    float output_before_hill = ecvt_control_step(cfg, st,
                                                  target_rpm, plant->rpm, true,
                                                  0.0f, 0.0f, true,
                                                  25.0f, true,
                                                  dt);
    plant_step(plant, output_before_hill, dt);

    /* The hill begins: accel_x_g jumps to reflect a 15% grade (~0.15 g)
     * instantaneously (a real chassis pitch happens over less than one
     * control step at 50 Hz), while wheel speed - and therefore the plant's
     * RPM - has not moved yet. The load also steps up, which is what will
     * pull rpm_ss down over the plant's time constant; the feedforward's
     * job is to have already started compensating before that lag catches
     * up with measured_rpm. */
    plant->load_rpm = 900.0f;   /* engine now has to work harder to hold the same ratio */
    const float accel_x_g_on_grade = 0.15f;   /* ~15% grade, sin(theta) approx, see header */

    float output_step1 = ecvt_control_step(cfg, st,
                                            target_rpm, plant->rpm, true,
                                            accel_x_g_on_grade, /*gyro_z_dps=*/0.0f, true,
                                            /*wheel_speed_mph=*/25.0f, true,   /* still 25 mph - hasn't sagged yet */
                                            dt);

    float ff_contribution = cfg->ff_gain_turns_s_per_pct_grade * st->grade_estimate_pct;

    /* Because measured_rpm (and therefore error) has NOT changed between
     * output_before_hill and output_step1 - the plant hasn't had a chance
     * to sag yet - and the integrator entering this call is the same value
     * in both cases, the entire jump in output between these two calls can
     * only be the feedforward term. This is the direct proof that
     * feedforward acts before feedback has anything to react to. */
    float output_jump = output_step1 - output_before_hill;

    printf("  before hill: output=%.3f | step 1 of hill: grade_est=%.2f%% ff=%.3f "
           "output=%.3f (jump=%.3f)\n",
           (double)output_before_hill, (double)st->grade_estimate_pct,
           (double)ff_contribution, (double)output_step1, (double)output_jump);

    near("grade step: grade estimate reads ~15%% immediately", st->grade_estimate_pct, 15.0f, 1.0f);
    ok("grade step: feedforward term is a meaningful, non-trivial push",
       ff_contribution > 0.3f);
    near("grade step: the ENTIRE output jump on step 1 is attributable to feedforward "
         "(feedback hasn't reacted - error/integrator unchanged from the prior step)",
         output_jump, ff_contribution, 0.02f);

    plant_step(plant, output_step1, dt);

    /* Now run it out and confirm the loop actually recovers RPM near target
     * once the plant catches up - feedforward gets it in the right
     * neighborhood immediately, PI closes whatever gap is left. */
    float output = output_step1;
    for (int i = 0; i < 1500; i++)   /* 30 more simulated seconds */
    {
        output = ecvt_control_step(cfg, st,
                                    target_rpm, plant->rpm, true,
                                    accel_x_g_on_grade, 0.0f, true,
                                    25.0f, true,
                                    dt);
        plant_step(plant, output, dt);
    }

    printf("  30s after hill start: rpm=%.1f target=%.1f output=%.3f\n",
           (double)plant->rpm, (double)target_rpm, (double)output);

    near("grade step: rpm recovers back to target after the plant catches up",
         plant->rpm, target_rpm, 15.0f);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: sensor dropout                                           */
/* ------------------------------------------------------------------ */
static void test_sensor_dropout_degrades_safely(ecvt_control_config_t *cfg,
                                                 ecvt_control_state_t *st,
                                                 plant_t *plant)
{
    printf("-- scenario 3: sensor dropout\n");

    const float target_rpm = 3800.0f;
    const float dt = 0.02f;

    /* --- 3a: IMU + wheel speed drop out for longer than stale_timeout_s.
     * Feedback must keep working; feedforward must disable itself. --- */
    float output = 0.0f;
    int dropout_steps = (int)(0.60f / dt);   /* 0.6s of dropout, well past the 0.3s timeout */
    for (int i = 0; i < dropout_steps; i++)
    {
        output = ecvt_control_step(cfg, st,
                                    target_rpm, plant->rpm, true,       /* RPM feedback still fine */
                                    0.0f, 0.0f, false,                  /* IMU: no fresh data */
                                    0.0f, false,                        /* wheel speed: no fresh data */
                                    dt);
        plant_step(plant, output, dt);
    }

    printf("  after %.2fs IMU+wheel dropout: degraded=%d output=%.3f rpm=%.1f\n",
           (double)(dropout_steps * dt), st->degraded, (double)output, (double)plant->rpm);

    ok("dropout: module reports degraded once past the stale timeout", st->degraded);
    ok("dropout: feedback still produced a live (nonzero-capable) output",
       output != 0.0f || fabsf(target_rpm - plant->rpm) < 1.0f);

    /* Prove feedback is still genuinely closing the loop while blind to the
     * IMU: push the target up and confirm the output responds. */
    float output_before_push = output;
    float new_target = target_rpm + 400.0f;
    for (int i = 0; i < 25; i++)
    {
        output = ecvt_control_step(cfg, st,
                                    new_target, plant->rpm, true,
                                    0.0f, 0.0f, false,
                                    0.0f, false,
                                    dt);
        plant_step(plant, output, dt);
    }
    ok("dropout: feedback (kp*error) still reacts to a new target while IMU is dark",
       output > output_before_push);

    /* --- 3b: RPM feedback itself drops out. Must force a safe zero output
     * and freeze (not zero) the integrator. --- */
    float integrator_before = st->integrator;
    float out_rpm_lost = ecvt_control_step(cfg, st,
                                            new_target, plant->rpm, false,   /* RPM: no fresh data */
                                            0.0f, 0.0f, false,
                                            0.0f, false,
                                            dt);

    printf("  RPM feedback lost: output=%.3f degraded=%d integrator %.4f -> %.4f\n",
           (double)out_rpm_lost, st->degraded, (double)integrator_before, (double)st->integrator);

    near("rpm lost: output forced to safe zero", out_rpm_lost, 0.0f, 1e-6f);
    ok("rpm lost: degraded flag set", st->degraded);
    near("rpm lost: integrator frozen, not zeroed", st->integrator, integrator_before, 1e-6f);

    /* --- 3c: dt_s <= 0 guard: must return last output, not divide by zero. --- */
    float last = st->last_output_turns_s;
    float out_bad_dt = ecvt_control_step(cfg, st,
                                          new_target, plant->rpm, true,
                                          0.0f, 0.0f, true,
                                          25.0f, true,
                                          0.0f);   /* dt_s = 0 */
    near("dt<=0 guard: returns last known-good output unchanged", out_bad_dt, last, 1e-6f);
    ok("dt<=0 guard: flags degraded", st->degraded);
}

int main(void)
{
    printf("ecvt_control host test\n\n");

    ecvt_control_config_t cfg = make_cfg();
    ecvt_control_state_t st;
    ecvt_control_init(&st);

    plant_t plant;
    plant.rpm = 3000.0f;
    plant.base_rpm = 3000.0f;
    plant.k_plant = 600.0f;
    plant.load_rpm = 0.0f;
    plant.tau_s = 1.5f;

    test_flat_ground_convergence(&cfg, &st, &plant);
    test_grade_step_feedforward_leads(&cfg, &st, &plant);
    test_sensor_dropout_degrades_safely(&cfg, &st, &plant);

    printf("\n  %d/%d checks passed\n", checks - failures, checks);
    if (failures)
    {
        printf("  %d FAILURE(S)\n", failures);
        return 1;
    }
    return 0;
}
