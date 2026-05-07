"""OTA device-facing endpoints — metadata poll and firmware chunk download.

Both routes live under /api/v1/weather/ (Arch §10 Q-S1 Option B) so they share
the same Nginx location block and rate-limit zone as the ingest endpoint.
"""
from __future__ import annotations

import re
from pathlib import Path

import aiofiles
import asyncpg
from fastapi import APIRouter, Depends, HTTPException, Query, Response
from fastapi.responses import HTMLResponse

from app.auth.mtls import mtls_required
from app.db.queries import record_chunk_download
from app.deps import get_db
from app.metrics import ota_chunks_served_total
from app.ota.campaign import compute_wait, get_active_campaign_for_device
from app.ota.crc32 import crc32_mpeg2

router = APIRouter(prefix="/api/v1/weather", tags=["ota"])

_ID_RE = re.compile(r"^\d{6}$")
_MAX_CHUNK = 512


def _parse_device_id(id: str | None) -> str:  # noqa: A002
    if id is None:
        raise HTTPException(status_code=400, detail="missing id")
    if not _ID_RE.match(id):
        raise HTTPException(status_code=400, detail="id must be exactly 6 decimal digits")
    return id


@router.get("/")
async def ota_metadata(
    id: str | None = Query(default=None),  # noqa: A002
    conn: asyncpg.Connection = Depends(get_db),
    _: None = Depends(mtls_required),
) -> HTMLResponse:
    """Return OTA metadata token for the device, or 'No update available'.

    Token format: V.<version>:L.<size_bytes>:H.<sha256hex>[:W.<wait_seconds>]
    The W field is omitted when the device is already in its rollout slot.
    """
    device_id = _parse_device_id(id)
    campaign = await get_active_campaign_for_device(conn, device_id)
    if campaign is None:
        return HTMLResponse(content="<html><body>No update available</body></html>")

    wait = compute_wait(device_id, campaign)
    token = f"V.{campaign.version}:L.{campaign.firmware_size}:H.{campaign.firmware_sha256}"
    if wait > 0:
        token += f":W.{wait}"
    return HTMLResponse(content=f"<html><body>{token}</body></html>")


@router.get("/get_firmware")
async def get_firmware(
    offset: int = Query(...),
    length: int = Query(...),
    id: str | None = Query(default=None),  # noqa: A002
    conn: asyncpg.Connection = Depends(get_db),
    _: None = Depends(mtls_required),
) -> Response:
    """Serve a firmware chunk with a 4-byte little-endian CRC-32/MPEG-2 appended.

    length is clamped to [1, 512]. Errors:
      400 — malformed or missing id
      404 — no active campaign for device
      416 — offset + clamped_length > firmware_size
      429 — device not yet in its rollout slot (Retry-After header set)
    """
    device_id = _parse_device_id(id)
    length = max(1, min(length, _MAX_CHUNK))

    campaign = await get_active_campaign_for_device(conn, device_id)
    if campaign is None:
        raise HTTPException(status_code=404, detail="no active campaign")

    wait = compute_wait(device_id, campaign)
    if wait > 0:
        raise HTTPException(
            status_code=429,
            detail="not yet in rollout slot",
            headers={"Retry-After": str(wait)},
        )

    if offset < 0 or offset + length > campaign.firmware_size:
        raise HTTPException(status_code=416, detail="range not satisfiable")

    chunk = await _read_chunk(Path(campaign.firmware_file_path), offset, length)
    crc = crc32_mpeg2(chunk)
    body = chunk + crc.to_bytes(4, "little")

    await record_chunk_download(conn, campaign.id, device_id, offset // _MAX_CHUNK)
    ota_chunks_served_total.inc()

    return Response(content=body, media_type="application/octet-stream")


async def _read_chunk(path: Path, offset: int, length: int) -> bytes:
    """Read bytes [offset, offset+length) from path without blocking the event loop."""
    async with aiofiles.open(path, "rb") as f:
        await f.seek(offset)
        return await f.read(length)
