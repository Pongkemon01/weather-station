import os
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request, Response
from prometheus_client import CONTENT_TYPE_LATEST, REGISTRY, generate_latest
from prometheus_fastapi_instrumentator import Instrumentator

from app.config import settings
from app.db import pool
from app.logging_config import configure_logging
from app.routers import admin, ota, ui, weather

configure_logging()


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

# Single worker (gunicorn -w 1): standard in-process registry is sufficient.
# /metrics is safe: gunicorn binds 127.0.0.1:8000; Nginx never proxies this path.
Instrumentator().instrument(app)


@app.get("/metrics", include_in_schema=False)
async def metrics_endpoint(request: Request) -> Response:
    if request.client.host not in ("127.0.0.1", "::1"):
        return Response(status_code=403)
    return Response(generate_latest(REGISTRY), media_type=CONTENT_TYPE_LATEST)


app.include_router(weather.router)
app.include_router(ota.router)
app.include_router(admin.router)
app.include_router(ui.router)


@app.get("/health")
async def health():
    return {"status": "ok"}
