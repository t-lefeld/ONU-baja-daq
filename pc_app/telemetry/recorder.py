"""CSV recorder. One row per received snapshot, engineering units."""

from __future__ import annotations

import csv
import math
from datetime import datetime, timezone
from pathlib import Path

from .protocols import active
from .proto import Frame


class CsvRecorder:
    """
    Writes a flat, wide CSV: one column per channel, one row per frame.

    Wide rather than long (timestamp, channel, value) because the point of this
    file is to be opened in Excel or pandas and plotted immediately, and a wide
    layout needs no pivoting. If you later want long format for a database,
    convert at that point rather than making the everyday case harder.

    Flushed every row. Telemetry sessions end by someone unplugging something,
    and a buffered CSV that loses its last few seconds is a bad trade for the
    negligible write cost at 2 Hz.
    """

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)

        self._fh = self.path.open("w", newline="", encoding="utf-8")
        self._writer = csv.writer(self._fh)
        self._rows = 0

        header = ["utc_iso", "host_epoch", "frame_seq", "hub_t_ms"]
        for node in range(active().NODE_COUNT):
            header += [f"n{node}_online", f"n{node}_loss"]
            for ch in active().CHANNELS[node]:
                header.append(f"n{node}_{ch.name}_{ch.unit}")

        self._writer.writerow(header)
        self._fh.flush()

    def write(self, frame: Frame) -> None:
        now = datetime.now(timezone.utc)
        row: list = [now.isoformat(timespec="milliseconds"), f"{now.timestamp():.3f}",
                     frame.seq, frame.t_ms]

        for rec in frame.nodes:
            row += [int(rec.online), rec.loss]
            defs = active().CHANNELS[rec.node_id]
            for d, raw in zip(defs, rec.raw):
                # Round to the precision the scale factor can actually express.
                # Printing 22.470000000000002 for a 0.01-scaled channel is noise.
                decimals = max(0, -int(round(math.log10(d.scale))))
                row.append(round(d.to_eng(raw), decimals))

        self._writer.writerow(row)
        self._fh.flush()
        self._rows += 1

    @property
    def rows(self) -> int:
        return self._rows

    def close(self) -> None:
        if not self._fh.closed:
            self._fh.close()
