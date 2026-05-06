"""T1-series: ingest path integration tests (Server Phase 3 / S3-8).

All tests use region=999 (TEST_REGION) to isolate from real device data.
Primary mode: INTERNAL_URL + injected X-SSL-Client-Verify header.
DB assertions require TEST_DB_DSN; skipped gracefully when absent.

T1-1  Happy path: single chunk → 200 {"status":"ok"}, float fields in DB ±1 LSB
T1-2  Max batch: 28 chunks → 200, all 28 rows in weather_records
T1-3  Idempotency: same payload twice → second response {"status":"duplicate"}
T1-4  Field boundary extremes: -256.0 and +255.9921875 survive round-trip
T1-5  Malformed header (count mismatch) → 400
T1-6  First-seen device auto-upsert → new devices row with last_seen
T1-7  Idempotency key format: "{rrr}{sss}:{iso_datetime}" in ingest_log
T1-8  Missing X-SSL-Client-Verify header → 403
T1-9  Y2K epoch 0 timestamp → stored as 2000-01-01T00:00:00+00:00
"""
from __future__ import annotations

import struct
import time
from datetime import UTC, datetime, timedelta

import pytest

from lib.mock_device import MockDevice
from lib.packed import Sample, encode, make_sample

_LSB = 1 / 128
_Y2K = datetime(2000, 1, 1, tzinfo=UTC)

# Unique base timestamp per test-run so repeated runs don't hit idempotency cache.
_RUN_TS = int(time.time()) - int(_Y2K.timestamp())


def _ts(offset: int = 0) -> int:
    """Y2K epoch seconds, unique per run + offset."""
    return _RUN_TS + offset


# ── T1-1: Happy path ──────────────────────────────────────────────────────────
async def test_t1_1_happy_path_response(dev):
    sample = make_sample(ts=_ts(1))
    result = await dev.upload([sample])
    assert result == {"status": "ok"}


async def test_t1_1_happy_path_db_float_fields(dev, db_cleanup):
    sample = make_sample(ts=_ts(2))
    await dev.upload([sample])

    row = await db_cleanup.fetchrow(
        """
        SELECT wr.temperature, wr.humidity, wr.pressure, wr.light_par,
               wr.rainfall, wr.dew_point, wr.bus_value
        FROM weather_records wr
        JOIN devices d ON d.id = wr.device_id
        WHERE d.region_id = $1 AND d.station_id = $2
        ORDER BY wr.time DESC LIMIT 1
        """,
        dev.region, dev.station,
    )
    assert row is not None, "no weather_records row found after upload"
    assert abs(row["temperature"] - sample.temperature) <= _LSB
    assert abs(row["humidity"]    - sample.humidity)    <= _LSB
    assert abs(row["pressure"]    - sample.pressure)    <= _LSB
    assert row["light_par"] == sample.light_par
    assert abs(row["rainfall"]    - sample.rainfall)    <= _LSB
    assert abs(row["dew_point"]   - sample.dew_point)   <= _LSB
    assert abs(row["bus_value"]   - sample.bus_value)   <= _LSB


# ── T1-2: Max batch (28 records) ─────────────────────────────────────────────
async def test_t1_2_max_batch_response(dev):
    samples = [make_sample(ts=_ts(10 + i)) for i in range(28)]
    payload = encode(dev.region, dev.station, samples)
    assert len(payload) == 509   # 5 + 28×18
    result = await dev.upload(samples)
    assert result == {"status": "ok"}


async def test_t1_2_max_batch_row_count(dev, db_cleanup):
    samples = [make_sample(ts=_ts(50 + i)) for i in range(28)]
    await dev.upload(samples)

    count = await db_cleanup.fetchval(
        """
        SELECT COUNT(*) FROM weather_records wr
        JOIN devices d ON d.id = wr.device_id
        WHERE d.region_id = $1 AND d.station_id = $2
        """,
        dev.region, dev.station,
    )
    assert count == 28


# ── T1-3: Idempotency ────────────────────────────────────────────────────────
async def test_t1_3_idempotency(dev, db_cleanup):
    sample = make_sample(ts=_ts(100))
    r1 = await dev.upload([sample])
    r2 = await dev.upload([sample])          # same payload, same ts
    assert r1 == {"status": "ok"}
    assert r2 == {"status": "duplicate"}

    count = await db_cleanup.fetchval(
        """
        SELECT COUNT(*) FROM weather_records wr
        JOIN devices d ON d.id = wr.device_id
        WHERE d.region_id = $1 AND d.station_id = $2
        """,
        dev.region, dev.station,
    )
    assert count == 1, "duplicate payload must not insert a second row"


# ── T1-4: Field boundary extremes ────────────────────────────────────────────
async def test_t1_4_field_boundaries_response(dev):
    sample = make_sample(ts=_ts(200), temperature=-256.0, humidity=255.9921875)
    result = await dev.upload([sample])
    assert result == {"status": "ok"}


async def test_t1_4_field_boundaries_db(dev, db_cleanup):
    sample = make_sample(ts=_ts(201), temperature=-256.0, humidity=255.9921875)
    await dev.upload([sample])

    row = await db_cleanup.fetchrow(
        """
        SELECT wr.temperature, wr.humidity FROM weather_records wr
        JOIN devices d ON d.id = wr.device_id
        WHERE d.region_id = $1 AND d.station_id = $2
        ORDER BY wr.time DESC LIMIT 1
        """,
        dev.region, dev.station,
    )
    assert row is not None
    assert abs(row["temperature"] - (-256.0))      <= _LSB
    assert abs(row["humidity"]    - 255.9921875)   <= _LSB


# ── T1-5: Malformed header (count mismatch) ───────────────────────────────────
async def test_t1_5_count_mismatch_returns_400(dev):
    # Build 4 records but declare count=5 in header.
    payload = bytearray(encode(dev.region, dev.station, [make_sample(ts=_ts(300))] * 4))
    payload[4] = 5   # count byte is at header offset 4
    r = await dev.upload_raw(bytes(payload))
    assert r.status_code == 400


async def test_t1_5_no_rows_on_bad_payload(dev, db_cleanup):
    payload = bytearray(encode(dev.region, dev.station, [make_sample(ts=_ts(301))] * 4))
    payload[4] = 5
    await dev.upload_raw(bytes(payload))

    count = await db_cleanup.fetchval(
        """
        SELECT COUNT(*) FROM weather_records wr
        JOIN devices d ON d.id = wr.device_id
        WHERE d.region_id = $1 AND d.station_id = $2
        """,
        dev.region, dev.station,
    )
    assert count == 0


# ── T1-6: First-seen device auto-upsert ──────────────────────────────────────
async def test_t1_6_new_device_response(dev):
    """A never-seen (region, station) pair must be accepted and auto-created."""
    result = await dev.upload([make_sample(ts=_ts(400))])
    assert result == {"status": "ok"}


async def test_t1_6_new_device_db_row(dev, db_cleanup):
    before = datetime.now(UTC)
    await dev.upload([make_sample(ts=_ts(401))])

    row = await db_cleanup.fetchrow(
        "SELECT last_seen FROM devices WHERE region_id = $1 AND station_id = $2",
        dev.region, dev.station,
    )
    assert row is not None, "devices row must be auto-created on first upload"
    assert row["last_seen"] >= before, "last_seen must reflect upload time"


# ── T1-7: Idempotency key format ─────────────────────────────────────────────
async def test_t1_7_idempotency_key_format(dev, db_cleanup):
    """Key must be '{region:03d}{station:03d}:{first_sample_time.isoformat()}'."""
    ts = _ts(500)
    expected_time = _Y2K + timedelta(seconds=ts)
    await dev.upload([make_sample(ts=ts)])

    # The key the server stores:
    expected_key = f"{dev.region:03d}{dev.station:03d}:{expected_time.isoformat()}"

    key = await db_cleanup.fetchval(
        "SELECT idempotency_key FROM ingest_log WHERE idempotency_key = $1",
        expected_key,
    )
    assert key == expected_key, (
        f"ingest_log key mismatch: stored {key!r}, expected {expected_key!r}"
    )


# ── T1-8: Missing X-SSL-Client-Verify header → 403 ───────────────────────────
async def test_t1_8_missing_verify_header_returns_403(dev_no_cert):
    """FastAPI must reject requests without X-SSL-Client-Verify: SUCCESS."""
    r = await dev_no_cert.upload_raw(
        encode(dev_no_cert.region, dev_no_cert.station, [make_sample(ts=_ts(600))])
    )
    assert r.status_code == 403


# ── T1-9: Y2K epoch 0 timestamp ──────────────────────────────────────────────
async def test_t1_9_y2k_epoch_zero_response(dev):
    result = await dev.upload([make_sample(ts=0)])
    # Y2K epoch 0 = 2000-01-01T00:00:00Z; valid timestamp, must be accepted.
    assert result in ({"status": "ok"}, {"status": "duplicate"})


async def test_t1_9_y2k_epoch_zero_stored_correctly(dev, db_cleanup):
    # db_cleanup pre-purges stale rows before yielding, so ts=0 is safe to insert.
    await dev.upload([make_sample(ts=0)])

    row = await db_cleanup.fetchrow(
        """
        SELECT wr.time FROM weather_records wr
        JOIN devices d ON d.id = wr.device_id
        WHERE d.region_id = $1 AND d.station_id = $2
        ORDER BY wr.time ASC LIMIT 1
        """,
        dev.region, dev.station,
    )
    assert row is not None
    expected = datetime(2000, 1, 1, tzinfo=UTC)
    assert row["time"].replace(tzinfo=UTC) == expected
