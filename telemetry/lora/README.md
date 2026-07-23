# LoRa Telemetry

**Frequency:** 915MHz ISM band  
**TX module:** E32-900T30D (Firewall node, vehicle)  
**RX module:** E22-900T22U (pit) — USB-interface LoRa dongle, no onboard MCU

## Link Architecture

```
[Firewall Node] ──LoRa 915MHz──► [E22-900T22U USB Receiver]
                                       │
                                       ▼
                              [Browser Dashboard]
                              Web Serial API
                                       │
                                  optional
                                       ▼
                          [FastAPI/SQLite backend]
                          POST /telemetry, persistent log
```

The receiver plugs directly into the pit laptop's USB port and enumerates as a
serial device — there's no WiFi AP or intermediate microcontroller in this
path. See `docs/build-log/04_web_dashboard_pivot.md` and
`docs/build-log/05_receiver_and_scope_corrections.md` for how this replaced
the earlier ESP32/WiFi/PyQtGraph design.

## Frequency Hopping

5-channel hopping across 915MHz band for interference resilience at competition:

| Channel | Frequency |
|---------|-----------|
| 0 | 915.0 MHz |
| 1 | 915.5 MHz |
| 2 | 916.0 MHz |
| 3 | 916.5 MHz |
| 4 | 917.0 MHz |

Hop pattern is pseudo-random, seeded at both ends with the same value at powerup. TX and RX stay synchronized via sequence numbers — if RX detects a gap, it re-syncs on the next valid packet header.

## Packet Format

```
[ 0xDA 0xDA | Node ID (1B) | Seq (2B) | Payload length (1B) | Payload (nB) | CRC16 (2B) ]
```

- `0xDADA`: sync word — receiver scans for this before attempting decode
- Sequence: rolling uint16, increments per packet
- CRC16: computed over Node ID + Seq + Length + Payload

## Reliability Features

- Sequence numbering — receiver detects and logs dropped packets
- CRC16 on every packet — corrupt packets discarded, not forwarded to dashboard
- Watchdog on TX side — if CAN bus goes silent, firewall sends a heartbeat packet so RX knows the link is alive
- SD log on firewall is independent of LoRa — data is never lost even if the link drops
