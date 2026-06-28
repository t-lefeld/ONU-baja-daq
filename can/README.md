# CAN Bus

## Topology

- **Baud rate:** 500kbps
- **Physical layer:** Ethernet CAT5e twisted pair (differential pair used as CAN H/L)
- **Connectors:** Deutsch DT series throughout
- **Termination:** 120Ω at each end of the bus (firewall node and farthest corner node)

## Node IDs

| Node | CAN ID (base) | Location |
|------|--------------|----------|
| Front | 0x010 | Front suspension corner |
| Rear | 0x020 | Rear suspension corner |
| eCVT | 0x030 | CVT assembly |
| Firewall | 0x000 | Firewall (aggregator, no sensors) |

## DBC File

`baja.dbc` defines all message IDs, signal names, bit positions, scaling factors, and units. Parse with `cantools`:

```python
import cantools
db = cantools.database.load_file('baja.dbc')
msg = db.get_message_by_name('FrontSuspension')
decoded = msg.decode(raw_bytes)
```

## Wiring Notes

- Ethernet twisted pair used for CAN H/L — do not use solid-core wire
- Deutsch DT connectors: pin A = CAN H, pin B = CAN L, pin C = GND reference
- Keep stub lengths short at each node tap — max 30cm recommended
- Shield drain wire tied to chassis GND at firewall end only (single-point ground)

## Reliability Features

- Sequence numbering on all messages — detect dropped frames at the dashboard
- Watchdog timer on each node — CAN TX halted and error frame sent if node hangs
- SD log on firewall captures raw CAN frames as backup regardless of LoRa link status
