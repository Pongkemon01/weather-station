"""Weather data ingestion router — POST /api/v1/weather/upload."""
from __future__ import annotations

import asyncpg
from fastapi import APIRouter, Depends, HTTPException, Request

from app.auth.mtls import mtls_required
from app.db.queries import insert_ingest_log, insert_weather_records, upsert_device
from app.deps import get_db
from app.ota.parser import parse_upload

router = APIRouter(prefix="/api/v1/weather", tags=["weather"])


@router.post("/upload")
async def upload(
    request: Request,
    conn: asyncpg.Connection = Depends(get_db),
    _: None = Depends(mtls_required),
) -> dict:
    """Ingest a binary Weather_Data_Packed_t batch from a device.

    Expects raw bytes: 5-byte header (region_id u16, station_id u16, count u8)
    followed by count × 18-byte Weather_Data_Packed_t records.
    Returns {"status": "ok"} or {"status": "duplicate"}.
    """
    payload = await request.body()
    try:
        region, station, records = parse_upload(payload)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    if not records:
        raise HTTPException(status_code=400, detail="payload contains no records")

    idempotency_key = f"{region:03d}{station:03d}:{records[0]['time'].isoformat()}"

    async with conn.transaction():
        device_id = await upsert_device(conn, region, station)
        is_new = await insert_ingest_log(conn, idempotency_key)
        if not is_new:
            return {"status": "duplicate"}
        await insert_weather_records(conn, device_id, records)

    return {"status": "ok"}
