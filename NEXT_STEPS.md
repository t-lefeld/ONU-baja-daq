# Run it, then push to GitHub

Two parts. Part 1 takes about ten minutes and needs no hardware. Part 2 is the
GitHub push to your existing repo.

---

## ⚠ One thing already handled — don't undo it

Your repo was about to commit **`pc_app/.browser-profile/`** — 1044 files of
Edge profile data that `run.py` creates automatically, including Chromium's
`Login Data` and `Vpn Tokens` databases. App-generated, so almost certainly
empty of anything real, but a browser profile has no business in a public repo.

`.gitignore` now excludes it, plus `pc_app/build/` and `pc_app/dist/`. Staged
file count went from **1579 to 525**. Nothing for you to do — just don't remove
those lines, and Step 5 below verifies it before you push.

---

# Part 1 — Get the fake data running

## Step 1: Install the two dependencies

```
cd C:\Users\lefel\OneDrive\Desktop\can-lora-telemetry\pc_app
pip install -r requirements.txt
```

That's `aiohttp` and `pyserial`. **PySide6 is no longer needed** — the native
window it powered is retired, so if you had it installed you can leave it or
remove it, nothing uses it.

## Step 2: Run the test suite

```
cd C:\Users\lefel\OneDrive\Desktop\can-lora-telemetry
python tools\run_all_tests.py
```

**Expect 14 suites, all `PASS`, exit code 0.** If anything is red, stop and
tell me which one — I fixed the two you hit last time (a `.exe` naming bug in
my v2 node test runner, and the Qt test that could only ever pass by not
running), so a failure now would be something new.

## Step 3: Start the simulator

```
cd simulation
python run_sim.py
```

A browser opens to the app. That's it — the whole UI is one page now.

## Step 4: Walk the seven tabs

Press `1`–`7`, or click. What to check on each:

| Key | Tab | What should be true |
|---|---|---|
| `1` | **Live** | Every channel updating twice a second, sparkline drawing under each, min/max/avg filling in. No `--` left after a few seconds. |
| `2` | **Gauges** | 22 dials with needles sweeping through a useful part of their arc — not pinned near zero. |
| `3` | **Schematic** | Car outline with values in callout boxes at the corners. Wheel speeds and suspension at all four wheels, CVT and motor toward the rear. |
| `4` | **Charts** | Every channel a scrolling trace on a shared time axis. Try the window selector — 60s / 2m / 5m. |
| `5` | **Map** | A track shape drawing itself, an orange arrow at the car, a scale bar bottom-right. Give it 30 seconds to build a recognizable loop. |
| `6` | **Session** | Table of min / max / avg / current for all 24 channels with alarm thresholds. |
| `7` | **Logs** | Empty-state message explaining how to populate `field_data/`. That's correct — you have no real logs yet. |

Also try: **`F`** freezes the display (data keeps arriving behind it),
**Export CSV** downloads the whole session, **Reset session** clears stats.

**The one thing worth confirming:** the header shows `WIRE v2·80B`. That means
every number on screen was encoded to real v2 wire bytes and decoded back —
the same path real hardware will use — rather than floats handed straight to
the browser. If it says `raw-json`, the codec import failed and the startup log
will say why.

To let teammates watch from their phones on the same wifi:

```
python run_sim.py --host 0.0.0.0
```

Then give them `http://<your-machine-ip>:8766/`.

## Step 5 (optional): The real-hardware path

Nothing about v1 changed, so your working rig still runs:

```
cd pc_app
python run.py --port COM9
```

This now opens the browser dashboards instead of the old native window. It
speaks **v1** by default — the 3-node placeholder channels your firmware
actually sends today. The full 24-channel app is the simulator until the v2
firmware exists.

Once you have a real v2 log to replay:

```
python run.py --proto v2 --replay ../field_data/<folder>/LOG0001.TLM
```

---

# Part 2 — Push to your GitHub repo

## Step 1: Git identity (skip if already set)

```
git config --global user.name "Tate Lefeld"
git config --global user.email "t-lefeld@onu.edu"
```

## Step 2: Stage and check *before* committing

Un-committing a large or sensitive file later means rewriting history, so
verify first.

```
cd C:\Users\lefel\OneDrive\Desktop\can-lora-telemetry
git add -A
git status --short
```

Two checks that matter:

```
git ls-files --cached | findstr browser-profile
```
→ **must print nothing.** If it prints anything, run `git reset` and tell me.

```
git ls-files --cached | find /c /v ""
```
→ should be around **525**, not 1579.

## Step 3: Commit

```
git commit -m "Baja SAE CAN-to-LoRa telemetry system

Working v1 pipeline (3 CAN nodes -> LoRa -> SD -> ground station), verified
end to end on hardware. Plus the v2 multi-frame protocol, real sensor
drivers, an E-CVT control loop, and a tabbed telemetry app - all tested at
the library level, not yet running on hardware.

14 test suites passing."
```

## Step 4: Connect your existing repo and push

Your repo already exists, so just point at it. Replace the URL with yours:

```
git remote add origin https://github.com/<your-username>/<your-repo>.git
git branch -M main
git push -u origin main
```

**If the remote already has commits** (a README GitHub created for you, say),
that push will be rejected. Fix it with:

```
git pull --rebase origin main
git push -u origin main
```

If it asks for a password: GitHub stopped accepting account passwords over
HTTPS. You need a **personal access token** (github.com → Settings → Developer
settings → Personal access tokens → Fine-grained tokens), or install GitHub
Desktop / `gh` CLI, which handle auth for you.

## Step 5: Verify on github.com

- `README.md` renders on the front page
- **No `.browser-profile` folder anywhere**
- `firmware/`, `protocol/`, `simulation/`, `pc_app/`, `tools/` all present
- File count looks like a code repo, not a thousand cache files

---

## About OneDrive

Your project lives inside OneDrive. Git and OneDrive both aggressively manage
the same files, and OneDrive syncing `.git` mid-operation is a known way to
corrupt a repository.

It usually works. But if you start seeing odd git errors about objects or
locks, move the project to a non-synced path like
`C:\Users\lefel\dev\baja-daq-telemetry` — once it is on GitHub, that is your
real backup anyway.

---

## Filling in field_data/ with real captures

The Logs tab reads `field_data/`, which is empty on purpose — it is for real
captures only, not synthetic data. Once you are recording:

1. Run a session with the SD card in the hub.
2. Pull the card, copy `LOG0001.TLM` into `field_data/YYYY-MM-DD_description/`.
3. Add a short `notes.md`: conditions, what you were testing, what broke. Six
   months from now that is the difference between a useful log and an
   anonymous blob.
4. `git add`, commit, push.

`.gitignore` excludes `*.tlm` and `*.csv` everywhere else but has an explicit
exception for `field_data/`, so real captures get committed while bench
scratch files don't.

---

## What is NOT ready

Pushing this does not mean everything works:

- **v1 works on hardware** — 9 placeholder channels, verified end to end.
- **v2 is tested but runs nowhere.** Protocol, Python decoder, DBC, sensor
  drivers, E-CVT controller all pass tests in isolation. No v2 byte has
  crossed a real wire.
- **The v2 hub firmware is unfinished** — `telemetry_hub_v2.h` exists,
  `telemetry_hub_v2.c` was never written. Main blocker for v2 on hardware.
- **Sensor pins are unassigned** — every driver in `firmware/sensors/` has
  `TODO:` markers where a real pin or calibration constant goes.
- **Bench stages 3 and 4** (SD card, fault recovery) were never completed.

`PROJECT_STATUS.md` has the full breakdown.
