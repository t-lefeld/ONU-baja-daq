"use strict";

/**
 * telemetry-core.js - shared backend contract for every themed dashboard.
 *
 * Every skin under static/themes/ includes only this file to talk to the
 * server; none of them parse WebSocket JSON themselves. Message shapes
 * (meta/status/history/frame) are handled once, here. That is what lets five
 * visually unrelated frontends stay honest about the same data: a themed page
 * supplies render callbacks and gets fed identical, correctly-ordered events,
 * so a bug in one skin's WebSocket handling can't exist because there isn't
 * one - only a render callback can be wrong, and that only affects looks.
 *
 * Backend contract (from telemetry/server.py + telemetry/pump.py):
 *   meta    { type:"meta", source, proto_version, node_count, frame_period_ms,
 *             nodes:[{node_id, label, channels:[{name, unit}]}] }
 *   status  { type:"status", source, link, detail, uptime_s, frames,
 *             last_frame_age_s, crc_errors, clients, error? }
 *   history { type:"history", frames:[frame, ...] }   // sent once on connect
 *   frame   { type:"frame", frame:{ seq, t_ms,
 *             nodes:[{node_id, stale, fault, loss, channels:[{value, unit}]}] } }
 */
window.TelemetryCore = (function () {
  const HISTORY = 240; // samples kept per channel client-side (~2 min at 2 Hz)

  function fmt(v) {
    const a = Math.abs(v);
    if (a >= 1000) return v.toFixed(0);
    if (a >= 100) return v.toFixed(1);
    if (a >= 1) return v.toFixed(2);
    return v.toFixed(3);
  }

  /**
   * Plain autoscaled line, no fill. `color` is any CSS color string - pass the
   * theme's own accent so the sparkline always matches the skin instead of a
   * value hardcoded in shared code.
   */
  function drawSpark(canvas, data, color) {
    if (!canvas) return;

    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (!w || !h) return;

    if (canvas.width !== w * dpr || canvas.height !== h * dpr) {
      canvas.width = w * dpr;
      canvas.height = h * dpr;
    }

    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    if (data.length < 2) return;

    let lo = Infinity, hi = -Infinity;
    for (const v of data) {
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    // A dead-flat channel would divide by zero and vanish. Give it a band so
    // it draws through the middle - constant, which is the honest picture,
    // rather than missing.
    if (hi - lo < 1e-9) { lo -= 0.5; hi += 0.5; }
    const pad = (hi - lo) * 0.1;
    lo -= pad; hi += pad;

    ctx.beginPath();
    data.forEach((v, i) => {
      const x = (i / (data.length - 1)) * w;
      const y = h - ((v - lo) / (hi - lo)) * h;
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.strokeStyle = color || "#0b62d0";
    ctx.lineWidth = 1.25;
    ctx.stroke();
  }

  /** Running frames/sec estimate from arrival timestamps, identical everywhere. */
  function makeRateTracker(maxSamples = 20) {
    const recent = [];
    return {
      hit() {
        recent.push(performance.now());
        if (recent.length > maxSamples) recent.shift();
      },
      hz() {
        if (recent.length < 3) return null;
        const span = (recent[recent.length - 1] - recent[0]) / 1000;
        return span > 0 ? (recent.length - 1) / span : null;
      },
    };
  }

  /**
   * connect(callbacks) opens the WebSocket and reconnects with backoff.
   * callbacks (all optional): onMeta(meta), onFrame(frame, redraw),
   * onStatus(status), onOpen(), onClose().
   *
   * `redraw` on onFrame is false while replaying the on-connect history
   * backlog (except the last one) - repainting on every backfilled frame is
   * wasted work the user never sees.
   */
  function connect(callbacks) {
    const cb = callbacks || {};
    let retry = 500;

    function open() {
      const scheme = location.protocol === "https:" ? "wss:" : "ws:";
      const ws = new WebSocket(`${scheme}//${location.host}/ws`);

      ws.onopen = () => {
        retry = 500;
        cb.onOpen && cb.onOpen();
      };

      ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.type === "meta") {
          cb.onMeta && cb.onMeta(msg);
        } else if (msg.type === "status") {
          cb.onStatus && cb.onStatus(msg);
        } else if (msg.type === "history") {
          msg.frames.forEach((f, i) =>
            cb.onFrame && cb.onFrame(f, i === msg.frames.length - 1)
          );
        } else if (msg.type === "frame") {
          cb.onFrame && cb.onFrame(msg.frame, true);
        }
      };

      ws.onclose = () => {
        cb.onClose && cb.onClose();
        setTimeout(open, retry);
        retry = Math.min(retry * 2, 5000); // back off, do not spam the console
      };

      ws.onerror = () => ws.close();
    }

    open();
  }

  return { HISTORY, fmt, drawSpark, makeRateTracker, connect };
})();
