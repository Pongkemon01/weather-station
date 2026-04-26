import os
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI

from app.config import settings
from app.db import pool
from app.routers import admin, ota, weather


@asynccontextmanager
async def lifespan(app: FastAPI):
    _validate_firmware_dir()
    await pool.init_pool()
    yield
    await pool.close_pool()


def _validate_firmware_dir() -> None:
    """Refuse to start if FIRMWARE_DIR is not set, not absolute, or not writable."""
    if not settings.firmware_dir:
        raise RuntimeError("FIRMWARE_DIR must be set in iot.env")
    resolved = Path(settings.firmware_dir).resolve(strict=False)
    if not resolved.is_absolute():
        raise RuntimeError(f"FIRMWARE_DIR must be an absolute path; got {settings.firmware_dir!r}")
    if not resolved.is_dir() or not os.access(resolved, os.W_OK):
        raise RuntimeError(f"FIRMWARE_DIR {resolved} must be a writable directory")


app = FastAPI(title="IoT Weather Station Server", lifespan=lifespan)

app.include_router(weather.router)
app.include_router(ota.router)
app.include_router(admin.router)


@app.get("/health")
async def health():
    return {"status": "ok"}
