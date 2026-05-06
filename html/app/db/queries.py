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


# ---------------------------------------------------------------------------
# Phase 5: OTA chunk-download tracking
# ---------------------------------------------------------------------------

async def record_chunk_download(
    conn: asyncpg.Connection, campaign_id: int, device_id: str, chunk_index: int
) -> None:
    """Record a successfully served firmware chunk (idempotent)."""
    await conn.execute(
        """
        INSERT INTO download_completions (campaign_id, device_id, chunk_index)
        VALUES ($1, $2, $3)
        ON CONFLICT DO NOTHING
        """,
        campaign_id,
        device_id,
        chunk_index,
    )


# ---------------------------------------------------------------------------
# Phase 6: admin user lookup
# ---------------------------------------------------------------------------

async def get_admin_user(conn: asyncpg.Connection, username: str):
    """Return admin_users row by username, or None if not found."""
    return await conn.fetchrow(
        "SELECT id, username, password_hash, role FROM admin_users WHERE username = $1",
        username,
    )



# ---------------------------------------------------------------------------
# Phase 7: OTA campaign management
# ---------------------------------------------------------------------------

async def get_max_firmware_version(conn: asyncpg.Connection) -> int:
    """Return the maximum firmware version in ota_campaigns, or 0."""
    row = await conn.fetchrow("SELECT MAX(version) AS v FROM ota_campaigns")
    return row["v"] or 0


async def insert_campaign(
    conn: asyncpg.Connection,
    *,
    version: int,
    firmware_sha256: str,
    firmware_size: int,
    firmware_file_path: str,
    slot_len_sec: int,
) -> int:
    """Insert a new draft campaign and return its id."""
    row = await conn.fetchrow(
        """
        INSERT INTO ota_campaigns
            (version, firmware_sha256, firmware_size, firmware_file_path, slot_len_sec)
        VALUES ($1, $2, $3, $4, $5)
        RETURNING id
        """,
        version, firmware_sha256, firmware_size, firmware_file_path, slot_len_sec,
    )
    return row["id"]


async def get_campaign(conn: asyncpg.Connection, campaign_id: int):
    """Return the ota_campaigns row for campaign_id, or None."""
    return await conn.fetchrow("SELECT * FROM ota_campaigns WHERE id = $1", campaign_id)


async def list_campaigns_by_status(conn: asyncpg.Connection, *statuses: str):
    """Return campaigns matching any of the given statuses, ordered version DESC."""
    return await conn.fetch(
        "SELECT * FROM ota_campaigns WHERE status = ANY($1::text[]) ORDER BY version DESC",
        list(statuses),
    )


async def list_terminal_campaigns_ordered(conn: asyncpg.Connection):
    """Return id/version/firmware_file_path for terminal campaigns, version DESC."""
    return await conn.fetch(
        """
        SELECT id, version, firmware_file_path
        FROM ota_campaigns
        WHERE status IN ('completed', 'cancelled')
        ORDER BY version DESC
        """,
    )


async def set_campaign_in_progress(
    conn: asyncpg.Connection,
    campaign_id: int,
    *,
    rollout_window_days: int,
    slot_len_sec: int,
    target_cohort_ids,
) -> None:
    await conn.execute(
        """
        UPDATE ota_campaigns
        SET status = 'in_progress',
            rollout_start = now(),
            rollout_window_days = $2,
            slot_len_sec = $3,
            target_cohort_ids = $4,
            updated_at = now()
        WHERE id = $1
        """,
        campaign_id, rollout_window_days, slot_len_sec, target_cohort_ids,
    )


async def set_campaign_paused(conn: asyncpg.Connection, campaign_id: int) -> None:
    await conn.execute(
        "UPDATE ota_campaigns SET status = 'paused', updated_at = now() WHERE id = $1",
        campaign_id,
    )


async def set_campaign_resumed(conn: asyncpg.Connection, campaign_id: int) -> None:
    await conn.execute(
        "UPDATE ota_campaigns SET status = 'in_progress', updated_at = now() WHERE id = $1",
        campaign_id,
    )


async def set_campaign_cancelled(
    conn: asyncpg.Connection, campaign_id: int, success_rate: float
) -> None:
    await conn.execute(
        """
        UPDATE ota_campaigns
        SET status = 'cancelled', success_rate = $2, updated_at = now()
        WHERE id = $1
        """,
        campaign_id, success_rate,
    )


async def compute_campaign_success_rate(
    conn: asyncpg.Connection, campaign_id: int, firmware_size: int
) -> float:
    """Fraction of started devices that downloaded all chunks (0.0 if none started)."""
    total_chunks = (firmware_size + 511) // 512
    row = await conn.fetchrow(
        """
        SELECT
            COUNT(*) FILTER (WHERE chunk_count >= $2) AS completed,
            COUNT(*)                                   AS total
        FROM (
            SELECT device_id, COUNT(DISTINCT chunk_index) AS chunk_count
            FROM download_completions
            WHERE campaign_id = $1
            GROUP BY device_id
        ) sub
        """,
        campaign_id, total_chunks,
    )
    total = row["total"] or 0
    completed = row["completed"] or 0
    return float(completed) / float(total) if total > 0 else 0.0


async def count_completed_devices(
    conn: asyncpg.Connection, campaign_id: int, firmware_size: int
) -> int:
    """Count devices that downloaded all (firmware_size+511)//512 chunks."""
    total_chunks = (firmware_size + 511) // 512
    row = await conn.fetchrow(
        """
        SELECT COUNT(*) AS cnt FROM (
            SELECT device_id
            FROM download_completions
            WHERE campaign_id = $1
            GROUP BY device_id
            HAVING COUNT(DISTINCT chunk_index) >= $2
        ) sub
        """,
        campaign_id, total_chunks,
    )
    return row["cnt"] or 0


async def count_eligible_devices(
    conn: asyncpg.Connection, target_cohort_ids
) -> int:
    """Count registered devices that match the cohort filter (NULL = whole fleet)."""
    if target_cohort_ids is None:
        row = await conn.fetchrow("SELECT COUNT(*) AS cnt FROM devices")
    else:
        row = await conn.fetchrow(
            """
            SELECT COUNT(*) AS cnt FROM devices
            WHERE lpad(region_id::text, 3, '0') || lpad(station_id::text, 3, '0')
                  = ANY($1::text[])
            """,
            target_cohort_ids,
        )
    return row["cnt"] or 0
