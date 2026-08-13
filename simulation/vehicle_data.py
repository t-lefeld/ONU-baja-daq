"""
vehicle_data.py - Realistic synthetic data for the FULL sensor suite, not the
placeholder 3-node protocol the firmware currently speaks.

This is intentionally decoupled from protocol/telemetry_proto.c and
pc_app/telemetry/proto.py. Those define the actual CAN/LoRa wire format and
are proven working end-to-end on real hardware - nothing here touches them.
This module exists purely so the themed dashboards under pc_app/static/themes
can be judged against data shaped like what the finished car will actually
produce, before every sensor is wired up.

Sensor -> node mapping (see simulation/README.md for the full writeup):

  Hub   (node 0) - GPS (u-blox NEO-M8N) + 9DOF IMU (BNO080)
  Front (node 1) - wheel speed x2 (Littelfuse 55075), suspension pot x2
                    (Bourns 53AAA-B28-B15L), brake pressure (Anfield T200/T201)
  Rear  (node 2) - wheel speed x2, suspension pot x2, CVT temp (MLX90614)
  Motor (node 3) - E-CVT: ODrive S1 + D5065 motor telemetry, brake resistor

Every channel is a deterministic function of elapsed seconds, same philosophy
as sim_read_channels() in the real firmware's can_node.c: reproducible, no
randomness, so two runs (or two teammates' laptops) show the same story.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

LAP_PERIOD_S = 60.0          # one simulated lap of the course
MPH_PER_G_PER_S = 21.937     # mph/s that equals 1 g of longitudinal accel

# Fixed near Ada, Ohio (Ohio Northern's home turf) - arbitrary but plausible.
TRACK_CENTER_LAT = 40.7660
TRACK_CENTER_LON = -83.8220
TRACK_RADIUS_DEG = 0.00090   # roughly a 100 m loop


@dataclass
class Channel:
    name: str
    unit: str


NODES: list[tuple[str, list[Channel]]] = [
    ("Hub - GPS/IMU", [
        Channel("gps_lat", "deg"),
        Channel("gps_lon", "deg"),
        Channel("gps_speed", "mph"),
        Channel("gps_heading", "deg"),
        Channel("gps_sats", "count"),
        Channel("accel_x", "g"),
        Channel("accel_y", "g"),
        Channel("accel_z", "g"),
        Channel("gyro_z", "deg/s"),
    ]),
    ("Front", [
        Channel("wheel_speed_fl", "mph"),
        Channel("wheel_speed_fr", "mph"),
        Channel("suspension_fl", "mm"),
        Channel("suspension_fr", "mm"),
        Channel("brake_pressure_f", "psi"),
    ]),
    ("Rear", [
        Channel("wheel_speed_rl", "mph"),
        Channel("wheel_speed_rr", "mph"),
        Channel("suspension_rl", "mm"),
        Channel("suspension_rr", "mm"),
        Channel("cvt_temp", "degC"),
    ]),
    ("E-CVT / Motor", [
        Channel("motor_current", "A"),
        Channel("motor_velocity", "rpm"),
        Channel("motor_temp", "degC"),
        Channel("bus_voltage", "V"),
        Channel("brake_resistor_w", "W"),
    ]),
]

NODE_COUNT = len(NODES)
FRAME_PERIOD_MS = 500  # 2 Hz, same cadence as the real protocol


def _bump(t: float, amp: float) -> float:
    """Deterministic high-frequency texture standing in for track chatter."""
    return amp * (math.sin(37.0 * t) * 0.6 + math.sin(91.3 * t) * 0.4)


def _speed_mph(t: float) -> float:
    phase = 2.0 * math.pi * t / LAP_PERIOD_S - math.pi / 2.0
    return 22.0 + 20.0 * math.sin(phase)


def _lon_accel_g(t: float) -> float:
    """d(speed)/dt, converted to g - positive = accelerating, negative = braking."""
    phase = 2.0 * math.pi * t / LAP_PERIOD_S - math.pi / 2.0
    d_speed_mph_dt = 20.0 * (2.0 * math.pi / LAP_PERIOD_S) * math.cos(phase)
    return d_speed_mph_dt / MPH_PER_G_PER_S


def _lat_accel_g(t: float) -> float:
    return 0.8 * math.sin(2.0 * math.pi * t / LAP_PERIOD_S * 2.0)


def _warmup(t: float, start: float, end: float, tau: float) -> float:
    """Rises from `start` toward `end` with time constant `tau` seconds."""
    return start + (end - start) * (1.0 - math.exp(-t / tau))


def sample_frame(seq: int, t_elapsed: float) -> dict:
    """
    One frame at `t_elapsed` seconds since the sim started. `t_elapsed` drives
    the cyclic (lap-based) channels via t_elapsed % LAP_PERIOD_S, but thermal
    channels use raw t_elapsed so they warm up over the whole session instead
    of resetting every lap.
    """
    t = t_elapsed % LAP_PERIOD_S

    speed = max(0.0, _speed_mph(t))
    lon_g = _lon_accel_g(t)
    lat_g = _lat_accel_g(t)
    heading = (t_elapsed / LAP_PERIOD_S * 360.0) % 360.0

    # ---- Hub: GPS + IMU ----
    gps_lat = TRACK_CENTER_LAT + TRACK_RADIUS_DEG * math.sin(2 * math.pi * t / LAP_PERIOD_S)
    gps_lon = TRACK_CENTER_LON + TRACK_RADIUS_DEG * math.cos(2 * math.pi * t / LAP_PERIOD_S)
    gps_sats = 9 + round(math.sin(t_elapsed / 13.0))
    accel_z = 1.0 + _bump(t_elapsed, 0.06)
    gyro_z = lat_g * 60.0

    hub_vals = [gps_lat, gps_lon, speed, heading, gps_sats, lat_g, lon_g, accel_z, gyro_z]

    # ---- Front ----
    corner_offset = lat_g * 1.5
    wheel_fl = max(0.0, speed + corner_offset)
    wheel_fr = max(0.0, speed - corner_offset)
    lean = lat_g * 8.0
    susp_fl = min(50.0, max(0.0, 25.0 - lean + _bump(t_elapsed, 4.0)))
    susp_fr = min(50.0, max(0.0, 25.0 + lean + _bump(t_elapsed + 0.5, 4.0)))
    brake_f = min(1000.0, max(0.0, -lon_g * 600.0) + 5.0)

    front_vals = [wheel_fl, wheel_fr, susp_fl, susp_fr, brake_f]

    # ---- Rear ----
    wheel_rl = max(0.0, speed + corner_offset * 0.7)
    wheel_rr = max(0.0, speed - corner_offset * 0.7)
    susp_rl = min(50.0, max(0.0, 25.0 - lean * 0.8 + _bump(t_elapsed + 1.0, 4.0)))
    susp_rr = min(50.0, max(0.0, 25.0 + lean * 0.8 + _bump(t_elapsed + 1.5, 4.0)))
    cvt_temp = _warmup(t_elapsed, 35.0, 108.0, 180.0) + _bump(t_elapsed, 1.5)

    rear_vals = [wheel_rl, wheel_rr, susp_rl, susp_rr, cvt_temp]

    # ---- E-CVT / Motor ----
    motor_current = min(60.0, max(0.0, 5.0 + 40.0 * abs(lon_g)))
    motor_velocity = min(6000.0, max(0.0, speed * 130.0))
    motor_temp = _warmup(t_elapsed, 30.0, 92.0, 240.0) + 0.3 * motor_current
    bus_voltage = 55.0 - 4.0 * (motor_current / 60.0) + _bump(t_elapsed, 0.15)
    brake_resistor_w = min(50.0, max(0.0, -lon_g * 45.0))

    motor_vals = [motor_current, motor_velocity, motor_temp, bus_voltage, brake_resistor_w]

    all_vals = [hub_vals, front_vals, rear_vals, motor_vals]

    nodes = []
    for node_id, ((label, chans), vals) in enumerate(zip(NODES, all_vals)):
        nodes.append({
            "node_id": node_id,
            "stale": False,
            "fault": False,
            "loss": 0,
            "channels": [
                {"value": round(v, 4), "unit": c.unit}
                for c, v in zip(chans, vals)
            ],
        })

    return {
        "seq": seq & 0xFFFF,
        "t_ms": int(t_elapsed * 1000),
        "nodes": nodes,
    }


def meta() -> dict:
    return {
        "type": "meta",
        "source": "vehicle-sim",
        "proto_version": 0,
        "node_count": NODE_COUNT,
        "frame_period_ms": FRAME_PERIOD_MS,
        "nodes": [
            {
                "node_id": node_id,
                "label": label,
                "channels": [{"name": c.name, "unit": c.unit} for c in chans],
            }
            for node_id, (label, chans) in enumerate(NODES)
        ],
    }
