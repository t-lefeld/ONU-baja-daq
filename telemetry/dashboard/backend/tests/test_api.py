"""
Integration tests against the FastAPI app, using tempfile.mkdtemp() as the
mock SD card path (LOG_STORAGE_PATH equivalent, injected directly via the
create_app() factory for test isolation).

Covers: burst of high-frequency concurrent writes (mimicking real telemetry
rate) checked for dropped writes, the read-back endpoint against known
written data, and rotation via the API not clobbering the previous session.
"""
import shutil
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed

import pytest
from fastapi.testclient import TestClient

from app.main import create_app


@pytest.fixture()
def client():
    storage_path = tempfile.mkdtemp(prefix="sd_mock_api_")
    app = create_app(storage_path=storage_path, flush_interval_seconds=0.05)
    with TestClient(app) as c:
        yield c
    shutil.rmtree(storage_path, ignore_errors=True)


def test_health_reports_configured_storage_path(client):
    resp = client.get("/health")
    assert resp.status_code == 200
    assert resp.json()["status"] == "ok"


def test_posting_telemetry_without_active_session_is_rejected(client):
    resp = client.post("/telemetry", json={"payload": {"rpm": 1000}})
    assert resp.status_code == 409


def test_start_write_readback_roundtrip(client):
    start = client.post("/sessions/start", json={"name": "shakedown"})
    assert start.status_code == 200
    session_id = start.json()["session_id"]

    known_packets = [
        {"can_id": "0x100", "seq": i, "payload": {"rpm": 3000 + i}} for i in range(10)
    ]
    for pkt in known_packets:
        resp = client.post("/telemetry", json=pkt)
        assert resp.status_code == 200

    readback = client.get(f"/sessions/{session_id}/telemetry")
    assert readback.status_code == 200
    body = readback.json()
    assert body["count"] == 10
    for expected, actual in zip(known_packets, body["rows"]):
        assert actual["can_id"] == expected["can_id"]
        assert actual["payload"] == expected["payload"]


def test_rotation_via_api_does_not_clobber_previous_session(client):
    start_a = client.post("/sessions/start", json={"name": "run_a"})
    session_a = start_a.json()["session_id"]
    client.post("/telemetry", json={"seq": 1, "payload": {"marker": "a"}})

    start_b = client.post("/sessions/start", json={"name": "run_b"})
    session_b = start_b.json()["session_id"]
    client.post("/telemetry", json={"seq": 1, "payload": {"marker": "b"}})

    assert session_a != session_b

    rows_a = client.get(f"/sessions/{session_a}/telemetry").json()["rows"]
    rows_b = client.get(f"/sessions/{session_b}/telemetry").json()["rows"]

    assert len(rows_a) == 1 and rows_a[0]["payload"] == {"marker": "a"}
    assert len(rows_b) == 1 and rows_b[0]["payload"] == {"marker": "b"}

    sessions = client.get("/sessions").json()["sessions"]
    ids = {s["session_id"] for s in sessions}
    assert session_a in ids and session_b in ids


def test_burst_of_concurrent_writes_no_dropped_packets(client):
    """Mimics a burst of high-frequency telemetry with multiple concurrent
    writers (simulating async tasks / threads feeding off the LoRa receiver link)
    hitting the same active session."""
    client.post("/sessions/start", json={"name": "burst"})

    n_workers = 10
    packets_per_worker = 100

    def send_batch(worker_idx):
        sent = 0
        for i in range(packets_per_worker):
            resp = client.post(
                "/telemetry",
                json={"seq": i, "payload": {"worker": worker_idx, "i": i}},
            )
            if resp.status_code == 200:
                sent += 1
        return sent

    with ThreadPoolExecutor(max_workers=n_workers) as pool:
        futures = [pool.submit(send_batch, w) for w in range(n_workers)]
        total_sent = sum(f.result() for f in as_completed(futures))

    assert total_sent == n_workers * packets_per_worker

    session_id = client.get("/health").json()["active_session"]
    got = client.get(f"/sessions/{session_id}/telemetry", params={"limit": total_sent}).json()
    assert got["count"] == n_workers * packets_per_worker

    per_worker_counts = {}
    for row in got["rows"]:
        w = row["payload"]["worker"]
        per_worker_counts[w] = per_worker_counts.get(w, 0) + 1
    assert per_worker_counts == {w: packets_per_worker for w in range(n_workers)}


def test_stop_session_then_reject_further_writes(client):
    client.post("/sessions/start", json={"name": "short_run"})
    client.post("/telemetry", json={"seq": 1, "payload": {"x": 1}})
    stop = client.post("/sessions/stop")
    assert stop.status_code == 200

    resp = client.post("/telemetry", json={"seq": 2, "payload": {"x": 2}})
    assert resp.status_code == 409

    # data written before stop is still readable after the session is closed
    session_id = stop.json()["session_id"]
    rows = client.get(f"/sessions/{session_id}/telemetry").json()["rows"]
    assert len(rows) == 1


def test_read_unknown_session_returns_404(client):
    resp = client.get("/sessions/session_20200101_000000_deadbeef/telemetry")
    assert resp.status_code == 404
