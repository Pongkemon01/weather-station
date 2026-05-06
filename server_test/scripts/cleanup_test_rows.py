#!/usr/bin/env python3
"""Remove all test rows from the database (region_id=999, version>=900000000).

Run manually after a test session if autocleanup was skipped:
    python server_test/scripts/cleanup_test_rows.py

Reads TEST_DB_DSN from .env or environment.
"""
from __future__ import annotations

import asyncio
import os
import sys
from pathlib import Path

import asyncpg
from dotenv import load_dotenv

load_dotenv(Path(__file__).parent.parent / ".env", override=False)

TEST_REGION = 999
TEST_VERSION_THRESHOLD = 900_000_000


async def main() -> None:
    dsn = os.getenv("TEST_DB_DSN", "")
    if not dsn:
        print("ERROR: TEST_DB_DSN not set", file=sys.stderr)
        sys.exit(1)

    conn = await asyncpg.connect(dsn)
    try:
        # Weather records for test devices
        n = await conn.fetchval(
            """
            SELECT COUNT(*) FROM weather_records
            WHERE device_id IN (SELECT id FROM devices WHERE region_id = $1)
            """,
            TEST_REGION,
        )
        await conn.execute(
            """
            DELETE FROM weather_records
            WHERE device_id IN (SELECT id FROM devices WHERE region_id = $1)
            """,
            TEST_REGION,
        )
        print(f"Deleted {n} weather_records rows (region={TEST_REGION})")

        # Ingest log entries
        n = await conn.fetchval(
            "SELECT COUNT(*) FROM ingest_log WHERE idempotency_key LIKE $1",
            f"{TEST_REGION:03d}%",
        )
        await conn.execute(
            "DELETE FROM ingest_log WHERE idempotency_key LIKE $1",
            f"{TEST_REGION:03d}%",
        )
        print(f"Deleted {n} ingest_log rows")

        # Test device rows
        n = await conn.fetchval(
            "SELECT COUNT(*) FROM devices WHERE region_id = $1",
            TEST_REGION,
        )
        await conn.execute("DELETE FROM devices WHERE region_id = $1", TEST_REGION)
        print(f"Deleted {n} devices rows")

        # OTA test campaigns
        n = await conn.fetchval(
            "SELECT COUNT(*) FROM ota_campaigns WHERE version >= $1",
            TEST_VERSION_THRESHOLD,
        )
        if n:
            await conn.execute(
                "DELETE FROM ota_campaigns WHERE version >= $1",
                TEST_VERSION_THRESHOLD,
            )
            print(f"Deleted {n} ota_campaigns rows (version>={TEST_VERSION_THRESHOLD})")

        print("Done.")
    finally:
        await conn.close()


if __name__ == "__main__":
    asyncio.run(main())
