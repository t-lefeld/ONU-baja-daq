"""
Pit-side telemetry logging backend.

Receives telemetry packets over HTTP (POST /telemetry) forwarded by the
browser dashboard, which itself reads them over USB serial from a LoRa
USB receiver (E22-900T22U) that receives CAN bus data over LoRa from the
firewall node's STM32 on the car. Appends them to a per-session SQLite log
file. Storage location is fully configurable (see app/config.py) so this
can be validated against a plain folder now and pointed at a real SD card
mount later with zero code changes.
"""
from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from typing import Any, Optional

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

from app.config import Settings, get_settings
from app.storage import SessionManager


class TelemetryPacket(BaseModel):
    """Generic envelope for one incoming telemetry packet.

    `payload` is intentionally an open dict: the exact CAN signal layout
    (RPM, wheel speed, brake pressure, etc.) coming off the STM32 isn't
    fixed yet, so we store whatever fields arrive rather than hardcoding a
    schema. Known/likely metadata fields are pulled out to real columns so
    they're queryable/sortable; adjust the payload shape once the dashboard's
    exact CAN decode format is finalized.
    """

    device_ts: Optional[float] = Field(
        default=None, description="Timestamp attached upstream (receiver/STM32), if provided"
    )
    can_id: Optional[str] = Field(default=None, description="CAN arbitration id, e.g. hex string")
    seq: Optional[int] = Field(default=None, description="Packet sequence number, if provided")
    rssi: Optional[float] = Field(default=None, description="LoRa received signal strength, if provided")
    payload: dict[str, Any] = Field(default_factory=dict, description="Arbitrary decoded signal fields")


class StartSessionRequest(BaseModel):
    name: Optional[str] = None


def create_app(storage_path: Optional[str] = None, flush_interval_seconds: Optional[float] = None) -> FastAPI:
    """App factory. Pass storage_path explicitly in tests for isolation;
    in production, leave it None and it's read from LOG_STORAGE_PATH env var.
    """
    settings = get_settings()
    if storage_path is not None:
        settings = Settings(storage_path=storage_path, flush_interval_seconds=settings.flush_interval_seconds)
    if flush_interval_seconds is not None:
        settings = Settings(storage_path=settings.storage_path, flush_interval_seconds=flush_interval_seconds)

    manager = SessionManager(settings.storage_path)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        stop_event = asyncio.Event()

        async def periodic_flush():
            while not stop_event.is_set():
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=settings.flush_interval_seconds)
                except asyncio.TimeoutError:
                    pass
                manager.flush_active()

        task = asyncio.create_task(periodic_flush())
        try:
            yield
        finally:
            stop_event.set()
            await task
            manager.shutdown()

    app = FastAPI(title="Baja Pit Telemetry Logger", lifespan=lifespan)
    # The dashboard is a local HTML file (file:// origin) or served from an
    # arbitrary pit laptop, so there's no fixed origin to allow-list. This
    # service is meant to run on localhost/pit LAN only, never exposed to
    # the internet, so permissive CORS is an acceptable trade-off here.
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )
    app.state.settings = settings
    app.state.manager = manager

    @app.get("/health")
    def health():
        return {"status": "ok", "storage_path": settings.storage_path, "active_session": manager.active_session_id}

    @app.post("/sessions/start")
    def start_session(body: StartSessionRequest = StartSessionRequest()):
        info = manager.start_session(body.name)
        return {"session_id": info.session_id, "filename": info.filename}

    @app.post("/sessions/stop")
    def stop_session():
        info = manager.stop_session()
        if info is None:
            raise HTTPException(status_code=409, detail="no active session")
        return {"session_id": info.session_id, "filename": info.filename}

    @app.get("/sessions")
    def list_sessions():
        return {"sessions": manager.list_sessions()}

    @app.get("/sessions/{session_id}")
    def get_session(session_id: str):
        try:
            count = manager.session_row_count(session_id)
        except FileNotFoundError:
            raise HTTPException(status_code=404, detail="session not found")
        except ValueError:
            raise HTTPException(status_code=400, detail="invalid session id")
        return {
            "session_id": session_id,
            "row_count": count,
            "active": session_id == manager.active_session_id,
        }

    @app.get("/sessions/{session_id}/telemetry")
    def read_telemetry(session_id: str, limit: int = 1000, offset: int = 0):
        try:
            rows = manager.read_session(session_id, limit=limit, offset=offset)
        except FileNotFoundError:
            raise HTTPException(status_code=404, detail="session not found")
        except ValueError:
            raise HTTPException(status_code=400, detail="invalid session id")
        return {"session_id": session_id, "count": len(rows), "rows": rows}

    @app.post("/telemetry")
    def post_telemetry(packet: TelemetryPacket):
        try:
            row_id = manager.append_telemetry(packet.model_dump())
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc))
        return {"id": row_id}

    return app


# Default app instance for `uvicorn app.main:app`, reads LOG_STORAGE_PATH
# from the environment (defaults to ./data/sd_mock).
app = create_app()
