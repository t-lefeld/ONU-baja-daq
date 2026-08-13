# Vehicle simulator (full sensor suite)

This is a **synthetic data generator** for the sensor suite the finished car
will actually carry - not the 3-node placeholder protocol
(`protocol/telemetry_proto.c`) the firmware currently speaks. It exists so
your team can judge dashboard UI styles against data shaped like the real
thing, before every sensor is physically wired up.

**Nothing here touches the firmware, CAN bus, LoRa link, or SD logging.**
Those are proven working end-to-end on real hardware (see `BRINGUP_GUIDE.md`)
and this folder does not modify `firmware/`, `protocol/`, or `pc_app/`. It
reads static files from `pc_app/static/` (the themed dashboard HTML/CSS/JS)
to render against, and imports one module read-only:
`pc_app/telemetry/proto_v2.py`, the pure-Python v2 wire codec. Every frame
`server.py` sends is encoded to real v2 bytes and decoded straight back
before going out over the WebSocket, so the dashboards render data that
actually crossed the wire format real hardware will use - not just
vehicle_data.py's floats reformatted as JSON. See the docstring at the top
of `server.py` and `quantization_report.py` for details.

## Sensor -> node mapping

| Node | Label | Channels | Real sensor |
|---|---|---|---|
| 0 | Hub - GPS/IMU | gps_lat, gps_lon, gps_speed, gps_heading, gps_sats | u-blox NEO-M8N GPS w/ U.FL + external active antenna |
| 0 | Hub - GPS/IMU | accel_x, accel_y, accel_z, gyro_z | BNO080 9DOF IMU |
| 1 | Front | wheel_speed_fl, wheel_speed_fr | Littelfuse 55075 gear tooth sensors |
| 1 | Front | suspension_fl, suspension_fr | Bourns 53AAA-B28-B15L rotary pot on bellcrank |
| 1 | Front | brake_pressure_f | Anfield T200/T201 pressure transducer |
| 2 | Rear | wheel_speed_rl, wheel_speed_rr | Littelfuse 55075 gear tooth sensors |
| 2 | Rear | suspension_rl, suspension_rr | Bourns 53AAA-B28-B15L rotary pot on bellcrank |
| 2 | Rear | cvt_temp | MLX90614 IR temperature sensor |
| 3 | E-CVT / Motor | motor_current, motor_velocity, motor_temp | ODrive S1 controlling an ODrive D5065 (built-in thermistor) |
| 3 | E-CVT / Motor | bus_voltage | Battery pack rail as seen by the ODrive |
| 3 | E-CVT / Motor | brake_resistor_w | 2 ohm / 50 W brake resistor dissipation during regen |

All 24 channels are deterministic functions of elapsed time (see
`vehicle_data.py`) - a simulated 60-second lap: accelerate, corner, brake,
repeat, with thermal channels (`cvt_temp`, `motor_temp`) warming up
continuously across the whole session instead of resetting each lap. No
randomness, so the same run looks the same on every machine.

## Running it

Needs `aiohttp` (already a `pc_app` dependency - `pip install aiohttp` if you
haven't run `pc_app` yet).

```
cd simulation
python run_sim.py
```

That opens a browser to the gallery landing page. Or go straight to a skin:

```
http://localhost:8766/static/themes/modern.html
http://localhost:8766/static/themes/orange.html
http://localhost:8766/static/themes/cyan.html
http://localhost:8766/static/themes/black.html
http://localhost:8766/static/themes/white.html
```

`--host 0.0.0.0` lets teammates pull it up on their own phones over the same
Wi-Fi to judge side by side. `--port` picks a different port if 8766 is busy
(it auto-falls-forward anyway).

## Why the dashboards "just work" against this

`pc_app/static/js/telemetry-core.js` builds every panel purely from the
`meta` message's `nodes: [{node_id, label, channels: [{name, unit}]}]` list,
and fills values purely from each `frame`'s matching `nodes[].channels[]`
array. Nothing in any theme file hardcodes a channel name, node count, or
node label. `simulation/server.py` speaks the identical
meta/status/history/frame WebSocket contract as the real
`pc_app/telemetry/server.py`, just backed by `vehicle_data.py` instead of a
serial/CAN source - so any dashboard built against the real backend renders
this one unmodified, and vice versa.

## When real sensors replace this

Once you're ready to wire real GPS/IMU/encoders/etc. into the actual
firmware, this stops being needed - the real pipeline should grow its own
node/channel definitions in `protocol/telemetry_proto.c` (mirroring how
`TLM_CHANNELS` works today) and the dashboards keep working unmodified either
way. See `DATA_VERIFICATION_CHECKLIST.md` in the repo root for how to confirm
that real data is genuinely live once that happens.
