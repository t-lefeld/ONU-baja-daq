# E-CVT control loop - base code

`ecvt_control.h` / `ecvt_control.c` - a PI control loop that holds engine RPM
at a target by commanding the CVT's ratio actuator (an ODrive S1, see
`firmware/sensors/odrive_can.h`), with an IMU-based feedforward term that
anticipates a hill before RPM actually sags.

**This is base/scaffold code, not vehicle-tuned.** Every gain and limit in
`ecvt_control_config_t` has a `TODO` and needs real numbers from your actual
actuator, engine, and CVT before this runs anything. Nothing here has touched
a real ODrive, engine, or CVT - see "What this does NOT handle yet" at the
bottom before building on it.

## Where this fits

```
firmware/sensors/imu_bno080.h  --------> accel_x_g, gyro_z_dps
firmware/sensors/wheel_encoder.h ------> wheel_speed_mph
(engine RPM sensor - not in this repo yet) -> measured_rpm
                                              |
                                              v
                                    ecvt_control_step()
                                              |
                                              v  output_turns_s
                                firmware/sensors/odrive_can.h -> ODrive S1
                                                                  (CVT ratio
                                                                   actuator)
```

This module is HAL-free and knows nothing about CAN, I2C, or any peripheral -
it is pure math that takes plain floats in and returns a plain float out. The
caller (whatever runs on the Motor/E-CVT node) is responsible for reading the
real sensors, calling `ecvt_control_step()` at whatever rate the main loop
allows, and sending the returned `turns_s` value as the ODrive's velocity
setpoint.

## Control structure

```
                         +----------------------------+
  target_rpm ----------->|                            |
                         |   error = target - measured |
  measured_rpm ---------->|                            |
                         +--------------+---------------+
                                        |
                                        v  error (RPM)
                         +----------------------------+
                         |     PI:  kp*error + integ    |
                         |  (integ += ki*error*dt,      |
                         |   clamped + conditionally    |
                         |   frozen when saturated -    |
                         |   see anti-windup below)     |
                         +--------------+---------------+
                                        |
                                        |  feedback (turns/s)
                                        v
   accel_x_g   -----+                (+)<----- feedforward (turns/s)
   gyro_z_dps  -----+---> [ grade estimator ]------+  = ff_gain * grade_pct
   wheel_speed_mph -+     (see below)                    (0 if |grade| <
                                        |                  deadband, or if
                                        v                  IMU/wheel input
                              grade_estimate_pct           is stale)
                                        |
                                        v
                              +-------------------+
                              |  clamp to          |
                              |  [output_min,       |
                              |   output_max]       |
                              +----------+----------+
                                         |
                                         v
                              output_turns_s -> ODrive velocity setpoint
```

**Grade estimator**, expanded:

```
  accel_x_g (raw, from IMU) ------------------------(+)---> grade_g = accel_x_g - a_true_g
                                                       ^
  wheel_speed_mph --> d(speed)/dt --> a_true_g --------+
                       (needs a previous sample,
                        divides by dt_s)

  grade_pct = 100 * grade_g          (small-angle sin(theta) approximation)
```

An accelerometer's X axis reads gravity's component along the vehicle's
longitudinal axis (which shows up when the vehicle pitches on a hill) added
to the vehicle's true forward/backward acceleration - it cannot tell those
two apart on its own. This module estimates the true acceleration part
independently, from how fast wheel speed is actually changing, and subtracts
it out. What's left over is (approximately) the gravity/tilt term, i.e. the
grade. Full reasoning, including the failure modes (wheel slip, hard
cornering, IMU mounting sign) is in the header comment at the top of
`ecvt_control.h` - read it before trusting this on a hill.

## What every tuning constant does, and a starting value

None of these have been run against a real engine/CVT/actuator. Treat the
"starting value" column as "a number that won't immediately do something
obviously wrong on the bench," not as a tuned value.

| Constant | What it does | Starting value | Reasoning |
|---|---|---|---|
| `kp` | RPM error -> output turns/s, proportional term | small, e.g. start around 0.001-0.003 | Pick during the "raise kp until it oscillates" step below - there's no substitute for doing this on the actual actuator/engine, any number here is a placeholder. |
| `ki` | RPM error -> output turns/s, integral term (accumulates error*dt) | `0.0` initially | Always start integral gain at zero - see tuning procedure. Add it only once `kp` alone gives a stable, non-oscillating response with an acceptable (even if nonzero) steady-state error. |
| `integrator_limit` | Hard clamp on how much output authority the I-term alone can ever have | comfortably below `output_max` (e.g. half) | Needs to be large enough that the integrator can actually cancel realistic steady-state load (idle drag, moderate grade) without saturating, but small enough that a fault (jammed actuator, sensor stuck) can't have the integrator alone drive the actuator to its mechanical limit. This repo's host test (`tools/test_ecvt_control.c`) shows what happens when this is set too small relative to the disturbance: a permanent steady-state RPM error that no amount of waiting fixes, because the integrator hit its ceiling. |
| `output_min_turns_s` / `output_max_turns_s` | Actuator command clamp | match the ODrive's configured `vel_limit` and the sheave's mechanical travel, not a guess | Getting this wrong either leaves performance on the table (too conservative) or commands the actuator past a mechanical stop (too aggressive) - confirm both against the real hardware config, not just the ODrive's software limit, since a software limit doesn't know where the sheave actually runs out of travel. |
| `ff_gain_turns_s_per_pct_grade` | How hard to preemptively shift per percent of estimated grade | small, e.g. `0.03-0.08` | This is the one gain that's genuinely hard to guess even roughly without vehicle data, because it depends on how much ratio change is actually needed to hold RPM through a given grade on your specific engine/CVT/gearing. Start low, watch how far behind (or ahead) of ideal the RPM trace is during a hill, adjust. |
| `grade_deadband_pct` | Grade estimates below this magnitude are treated as 0 | `1.5-2.0` | Covers accelerometer noise, road camber, and the fact that the accel-minus-wheel-speed subtraction is never perfectly clean even on flat ground (both signals have their own noise/bias). Too small: feedforward chatters constantly on flat ground. Too large: real hills get ignored until they're well underway. |
| `max_yaw_rate_dps_for_grade` | While `|gyro_z_dps|` exceeds this, freeze (don't update) the grade estimate | `20-25 deg/s` | A rough gate against hard cornering fouling the longitudinal-accel assumption - see header. Watch actual gyro_z traces from real cornering on your course to pick a threshold that catches genuine hard turns without also gating out every gentle sweeper. |
| `invert_accel_x` | Flips the sign of `accel_x_g` before use | determine on the bench | Tilt the actual mounted IMU nose-up by hand and check the sign of `accel_x_g` matches the convention documented at the top of `ecvt_control.h` (positive = nose-up / "pressed back in your seat"). If it's backwards, set this rather than touching the math. |
| `stale_timeout_s` | How long any one input may go without a fresh sample before this module treats it as lost | a few missed update periods of whatever feeds this loop (NOT the 2 Hz telemetry downlink - see below) | This loop should run off a fast local feed (CAN frames straight off the bus, a dedicated tach interrupt, etc.), not the ground-station telemetry link. Set this relative to that feed's actual update rate. |

## Tuning procedure (on the actual vehicle, once wired up)

This is a standard PI tuning order, plus a feedforward step at the end. Do
it in this order - trying to tune feedforward before the PI loop itself is
stable just makes it impossible to tell which term is misbehaving.

1. **Set `ki = 0` and `ff_gain_turns_s_per_pct_grade = 0`.** You are tuning
   pure proportional control first.
2. **Raise `kp` from a small value until the RPM response starts to
   oscillate** around the target after a step change in `target_rpm` (or
   after a load change, like tipping into the throttle). Back it off to
   somewhere around 50-70% of the value where oscillation started - this is
   the classic "find the edge, then step back" proportional tuning approach.
   Expect a persistent small steady-state error at this stage; that's what
   the integral term is for next.
3. **Bring `ki` up from zero slowly**, watching for the steady-state error
   to shrink without introducing a new, slower oscillation (integral action
   overshooting and correcting in a slow cycle is the classic symptom of
   `ki` too high). Set `integrator_limit` while doing this - if you see the
   RPM overshoot significantly after a large disturbance and take a long
   time to come back, the integrator likely wound up past what the actuator
   could use; lower the limit or re-check the anti-windup logic is actually
   engaging (it should be, since it's built into `ecvt_control_step()`, but
   confirm `integrator_limit` isn't set so high it never matters).
4. **Only after 2-3 give a stable, acceptable feedback loop**, start raising
   `ff_gain_turns_s_per_pct_grade` from 0 on a real hill (or a ramp on the
   course). Watch the RPM trace through the transition: too little
   feedforward and RPM still sags before the PI term catches up (the
   original problem this module exists to fix); too much and RPM overshoots
   the target the moment the grade is detected, before the actuator/engine
   have had a chance to actually respond. Aim for "RPM dips less, and
   recovers faster, than with feedforward off" rather than a perfectly flat
   trace - a perfectly flat trace usually means the feedforward is doing
   more work than it should relative to feedback, which makes it more
   sensitive to the grade estimate being wrong (see the header's list of
   grade-estimate failure modes).
5. **Tune `grade_deadband_pct` and `max_yaw_rate_dps_for_grade` last**,
   using logged `grade_estimate_pct` from flat, cornering, and hill sections
   of a real lap (this field is meant to be read out for telemetry/logging -
   see `ecvt_control_state_t` in the header). Set the deadband just above
   the noise floor you see on flat ground, and the yaw-rate gate just above
   what you see on your hardest real corners.

## What this does NOT handle yet

Being upfront about this matters more than it looks like it does - this is
meant to be built on for months, and a false sense of completeness here is
worse than a known gap.

- **No engine RPM driver exists in this repo yet.** `measured_rpm` is a
  plain `float` parameter - wiring up an actual tach (ignition pickup, Hall
  sensor on the flywheel, etc.) under `firmware/sensors/` is a separate,
  not-yet-started task.
- **No wheel-slip detection.** The grade estimator's accel-minus-wheel-speed
  subtraction silently produces a wrong grade estimate during wheelspin or a
  locked wheel. A real fix needs either a non-driven wheel's speed (if the
  car has one to reference), a GPS ground-speed cross-check, or comparing
  multiple wheels for disagreement - none of that is implemented here.
- **The cornering gate is crude.** Freezing the grade estimate above a yaw-
  rate threshold is a cheap mitigation, not a real decoupling of lateral
  dynamics from the longitudinal-accel assumption. A sustained hard corner
  on a hill (banked or off-camber) will still confuse this.
- **No actuator-side confirmation.** This module trusts that commanding
  `output_turns_s` actually moves the CVT ratio as expected - it does not
  read back the ODrive's actual encoder position/velocity to confirm the
  actuator did what was asked (`odrive_can.h` exposes that data; nothing
  here consumes it yet). A jammed actuator or a lost CAN frame to the ODrive
  would not be detected by this module.
- **No engine-specific behavior.** There's no rev limiter interaction, no
  idle/off-throttle special-casing, no clutch engagement logic (Baja
  primaries typically have their own centrifugal clutch behavior at low RPM
  that this loop knows nothing about) - this is purely "hold RPM at target,"
  full stop.
- **Velocity-controlled actuator assumed.** If the real CVT actuator turns
  out to be better suited to position control (command an absolute ratio,
  not a rate), this module's output needs to be integrated into a position
  setpoint by the caller, or the module restructured - see the assumption
  called out at the top of `ecvt_control.h`.
- **Grade sign convention is unverified against real hardware.** Everything
  about `accel_x_g`'s sign (nose-up = positive) is a documented assumption,
  not a measurement from an actual mounted BNO080 on this chassis. Verify it
  on the bench (`invert_accel_x`) before trusting any hill behavior.
- **Untested on hardware, obviously.** `tools/test_ecvt_control.c` proves
  the control math does what it's supposed to do against a synthetic
  first-order plant model - it says nothing about real actuator dynamics,
  real sensor noise, or real CAN bus timing.

## Host test

```
python tools/test_ecvt_control.py
```

Builds `tools/test_ecvt_control.c` against the real `ecvt_control.c` with
gcc/clang and runs it - no HAL, no hardware. Covers: flat-ground convergence
without integrator windup, a step onto a 15% grade (and a direct check that
the entire output jump on the first control step of the hill is attributable
to the feedforward term, not feedback - feedback hasn't had a chance to see
anything change yet), and sensor dropout (IMU/wheel loss disables only
feedforward; RPM feedback loss forces a safe zero output and freezes rather
than zeroes the integrator).

Not yet added to `tools/run_all_tests.py` - see the comment at the top of
`tools/test_ecvt_control.py` for why that's left as a deliberate follow-up
rather than bundled in with code that hasn't run on real hardware yet.
