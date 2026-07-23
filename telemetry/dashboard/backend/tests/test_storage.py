"""
Unit tests for the storage layer, using tempfile.mkdtemp() as the mock
SD card path -- exactly per the validation task's requirement to exercise
file creation, append, and rotation without any real hardware.
"""
import os
import shutil
import tempfile
import threading

import pytest

from app.storage import SessionManager, make_session_filename


@pytest.fixture()
def tmp_storage():
    path = tempfile.mkdtemp(prefix="sd_mock_")
    yield path
    shutil.rmtree(path, ignore_errors=True)


def test_filename_is_timestamp_based_and_unique():
    a = make_session_filename()
    b = make_session_filename()
    assert a != b
    assert a.startswith("session_")
    assert a.endswith(".db")


def test_start_session_creates_file_on_mock_sd_path(tmp_storage):
    mgr = SessionManager(tmp_storage)
    info = mgr.start_session(name="practice_run")
    assert os.path.isfile(info.path)
    assert info.path.startswith(tmp_storage)
    mgr.shutdown()


def test_append_and_read_back(tmp_storage):
    mgr = SessionManager(tmp_storage)
    mgr.start_session()
    mgr.append_telemetry({"can_id": "0x100", "seq": 1, "payload": {"rpm": 4200}})
    mgr.append_telemetry({"can_id": "0x101", "seq": 2, "payload": {"speed_kph": 61.5}})
    mgr.flush_active()

    session_id = mgr.active_session_id
    rows = mgr.read_session(session_id)
    assert len(rows) == 2
    assert rows[0]["can_id"] == "0x100"
    assert rows[0]["payload"] == {"rpm": 4200}
    assert rows[1]["payload"] == {"speed_kph": 61.5}
    mgr.shutdown()


def test_rotation_does_not_clobber_previous_session(tmp_storage):
    mgr = SessionManager(tmp_storage)

    first = mgr.start_session(name="run_a")
    mgr.append_telemetry({"seq": 1, "payload": {"marker": "run_a"}})
    mgr.flush_active()

    second = mgr.start_session(name="run_b")
    mgr.append_telemetry({"seq": 1, "payload": {"marker": "run_b"}})
    mgr.flush_active()

    assert first.session_id != second.session_id
    assert os.path.isfile(first.path)
    assert os.path.isfile(second.path)

    # previous session's data must still be intact and readable
    old_rows = mgr.read_session(first.session_id)
    assert len(old_rows) == 1
    assert old_rows[0]["payload"] == {"marker": "run_a"}

    new_rows = mgr.read_session(second.session_id)
    assert len(new_rows) == 1
    assert new_rows[0]["payload"] == {"marker": "run_b"}

    mgr.shutdown()


def test_append_without_flush_then_explicit_flush_persists(tmp_storage):
    mgr = SessionManager(tmp_storage)
    mgr.start_session()
    for i in range(50):
        mgr.append_telemetry({"seq": i, "payload": {"i": i}})
    # not flushed yet -- but read via the active in-memory connection still
    # sees everything (same connection), which is the correct behavior for
    # the read-back endpoint hitting the active session.
    session_id = mgr.active_session_id
    assert len(mgr.read_session(session_id)) == 50
    mgr.flush_active()
    assert len(mgr.read_session(session_id)) == 50
    mgr.shutdown()


def test_concurrent_appends_from_multiple_threads_no_dropped_writes(tmp_storage):
    mgr = SessionManager(tmp_storage)
    mgr.start_session()

    n_threads = 8
    writes_per_thread = 200
    errors = []

    def worker(thread_idx):
        try:
            for i in range(writes_per_thread):
                mgr.append_telemetry(
                    {"seq": i, "payload": {"thread": thread_idx, "i": i}}
                )
        except Exception as exc:  # pragma: no cover - failure path
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(t,)) for t in range(n_threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    mgr.flush_active()

    assert not errors
    session_id = mgr.active_session_id
    total = mgr.session_row_count(session_id)
    assert total == n_threads * writes_per_thread

    # sanity: every thread's full sequence made it in, nothing dropped
    rows = mgr.read_session(session_id, limit=total)
    counts = {}
    for row in rows:
        counts[row["payload"]["thread"]] = counts.get(row["payload"]["thread"], 0) + 1
    assert counts == {t: writes_per_thread for t in range(n_threads)}

    mgr.shutdown()


def test_reading_nonexistent_session_raises(tmp_storage):
    mgr = SessionManager(tmp_storage)
    with pytest.raises(FileNotFoundError):
        mgr.read_session("session_20200101_000000_deadbeef")
    mgr.shutdown()
