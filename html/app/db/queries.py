"""Parameterized SQL queries — no ORM; asyncpg directly."""
from __future__ import annotations

import asyncpg


# ---------------------------------------------------------------------------
# Phase 3: device upsert, idempotency, weather ingestion
# ---------------------------------------------------------------------------

async def upsert_device(
    conn: asyncpg.Connection, region_id: int, station_id: int
) -> int:
    """Upsert device row, refresh last_seen, return devices.id."""
    row = await conn.fetchrow(
        """
        INSERT INTO devices (region_id, station_id, last_seen)
        VALUES ($1, $2, now())
        ON CONFLICT (region_id, station_id) DO UPDATE SET last_seen = now()
        RETURNING id
        """,
        region_id,
        station_id,
    )
    return row["id"]


async def insert_ingest_log(conn: asyncpg.Connection, key: str) -> bool:
    """Insert idempotency key. Returns True if new, False if duplicate."""
    result = await conn.execute(
        "INSERT INTO ingest_log (idempotency_key) VALUES ($1) ON CONFLICT DO NOTHING",
        key,
    )
    return result == "INSERT 0 1"


async def insert_weather_records(
    conn: asyncpg.Connection, device_id: int, records: list[dict]
) -> None:
    """Bulk-insert decoded weather records for a device."""
    await conn.executemany(
        """
        INSERT INTO weather_records
            (time, device_id, temperature, humidity, pressure,
             light_par, rainfall, dew_point, bus_value)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
        """,
        [
            (
                r["time"],
                device_id,
                r["temperature"],
                r["humidity"],
                r["pressure"],
                r["light_par"],
                r["rainfall"],
                r["dew_point"],
                r["bus_value"],
            )
            for r in records
        ],
    )


# Phase 5: get_active_campaign_for_device (stub — implemented in Phase 5)
# Phase 7: get_max_firmware_version, list_campaigns_by_status, etc. (Phase 7)
