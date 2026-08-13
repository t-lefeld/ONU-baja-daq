# Field data

Real captured telemetry from the actual rig lands here - not synthetic data
(that's `simulation/`), and not scratch bench-test output.

## What goes here

- `.TLM` logs pulled off the hub's SD card after a real test session
  (`LOG0001.TLM`, `LOG0002.TLM`, ...) - see `BRINGUP_GUIDE.md` Part 5/6 for
  how those get written.
- `.csv` exports from the ground station app (`python run.py --csv` in
  `pc_app`) if you want an engineering-units copy alongside the raw log.

## Naming

Suggest `YYYY-MM-DD_description/` subfolders per session, e.g.:

```
field_data/
  2026-08-15_first_track_test/
    LOG0001.TLM
    LOG0001.csv
    notes.md
```

A short `notes.md` per session (conditions, what was being tested, anything
that broke) makes these logs far more useful later than the raw data alone.

## Replaying a log

From `pc_app`:

```
python run.py --replay ../field_data/2026-08-15_first_track_test/LOG0001.TLM
```

This runs the log back through the exact same decoder the live app uses -
whatever the dashboards showed live, replay reproduces bit-for-bit (see the
comment in `firmware/nucleo_hub/telemetry_hub.c` on why the SD log and the
LoRa link get identical bytes).

## Before committing to GitHub

`.gitignore` excludes `*.tlm` and `*.csv` everywhere else in this repo (bench
test scratch output), but has an explicit exception carving out
`field_data/**/*.tlm` and `field_data/**/*.csv` so real session captures
placed here *do* get committed. If a particular log is huge and you'd rather
not commit it, delete or move it before `git add` - the exception makes
everything under here trackable by default, not mandatory.
