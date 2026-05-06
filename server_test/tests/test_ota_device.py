"""T2-series: OTA device endpoint integration tests (Server Phase 5 / S5-8).

All tests hit FastAPI directly via INTERNAL_URL with X-SSL-Client-Verify injected.
DB setup and assertions require TEST_DB_DSN.
Writing firmware binaries requires FIRMWARE_DIR (absolute path accessible from
the test runner, same machine as the server or mounted remotely).

T2-1   No active campaign → "No update available" body
T2-2   Active campaign, device eligible → metadata token matches regex; V/L/H correct
T2-2a  Out-of-slot device → W.<positive> multiple of slot_len_sec
T2-2b  NULL rollout_start → W field absent (or W.0)
T2-2c  Malformed id (non-digit, wrong length, empty) → 400
T2-2d  Missing id query param → 400
T2-3   Slot determinism: 20 device_ids; W value == zlib.crc32 formula for each
T2-4   Chunked download reassembles to SHA-256 from metadata
T2-4a  Out-of-slot GET /get_firmware → 429 with Retry-After header
T2-5   Per-chunk CRC-32/MPEG-2 appended correctly
T2-6   Resumable: non-contiguous offsets; reassembled image SHA-256 matches
T2-7   offset + length > file_size → 416
T2-8   length=0 clamped to 1; length=1024 clamped to 512
T2-9   No active campaign → GET /get_firmware 404
T2-10  Monotone: once eligible, always eligible (absent pause/cancel)
"""
from __future__ import annotations

import hashlib
import re
import zlib
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path

import asyncpg
import pytest
import pytest_asyncio

from lib.crc32 import crc32_mpeg2
from lib.mock_device import MockDevice, OtaMetadata

# Metadata token regex from Arch §3.2
_META_RE = re.compile(r"V\.(\d+):L\.(\d+):H\.([0-9a-f]{64})(?::W\.(\d+))?")

TEST_REGION = 999
_SLOT_LEN = 43200   # 12 h; must match server default settings.slot_len_sec
_NUM_SLOTS = 20     # rollout_window_days=10

# ── Deterministic 2 KB test firmware binary ───────────────────────────────────
_FW_SIZE = 2048
_FW_DATA = bytes(i % 256 for i in range(_FW_SIZE))
_FW_SHA256 = hashlib.sha256(_FW_DATA).hexdigest()


# ── Slot helpers ──────────────────────────────────────────────────────────────

def _dev_slot(device_id: str, num_slots: int = _NUM_SLOTS) -> int:
    return zlib.crc32(device_id.encode("ascii")) % num_slots


def _rollout_start_for_now_slot(desired_now_slot: int) -> datetime:
    """Return a rollout_start such that now_slot == desired_now_slot."""
    return datetime.now(tz=UTC) - timedelta(seconds=desired_now_slot * _SLOT_LEN)


def _find_station_with_slot(target_op, *, exclude_stations: set[int] | None = None) -> int:
    """Return station in TEST_REGION satisfying target_op(dev_slot) == True."""
    exclude = exclude_stations or set()
    for s in range(1000):
        if s in exclude:
            continue
        did = f"{TEST_REGION:03d}{s:03d}"
        if target_op(_dev_slot(did)):
            return s
    raise RuntimeError("no matching station found in TEST_REGION[0..999]")


def _make_dev(station: int, internal_url: str) -> MockDevice:
    return MockDevice(
        region=TEST_REGION,
        station=station,
        base_url=internal_url,
        inject_verify_header=True,
    )


# ── Campaign DB helpers ───────────────────────────────────────────────────────

async def _insert_campaign(
    conn: asyncpg.Connection,
    *,
    firmware_path: str,
    version: int,
    rollout_start: datetime | None,
    rollout_window_days: int = 10,
    target_cohort_ids: list[str] | None = None,
) -> int:
    row = await conn.fetchrow(
        """
        INSERT INTO ota_campaigns
            (version, firmware_sha256, firmware_size, firmware_file_path,
             rollout_start, rollout_window_days, slot_len_sec,
             target_cohort_ids, status)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8::TEXT[], 'in_progress')
        RETURNING id
        """,
        version,
        _FW_SHA256,
        _FW_SIZE,
        firmware_path,
        rollout_start,
        rollout_window_days,
        _SLOT_LEN,
        target_cohort_ids,
    )
    return row["id"]


async def _purge_campaign(conn: asyncpg.Connection, campaign_id: int) -> None:
    await conn.execute(
        "DELETE FROM download_completions WHERE campaign_id = $1", campaign_id
    )
    await conn.execute("DELETE FROM ota_campaigns WHERE id = $1", campaign_id)


# ── campaign fixture (whole-fleet, rollout_start far past → all devices eligible) ─

@pytest_asyncio.fixture
async def campaign(db, firmware_dir):
    """Insert an in_progress campaign; write 2 KB test binary to firmware_dir.

    rollout_start is set far in the past so now_slot = num_slots-1 and every
    device is in-slot (W=0). Cleans up DB rows and the firmware file after the test.
    """
    version = 90001  # unlikely to collide with real campaigns
    fw_path = firmware_dir / f"v{version}.bin"
    fw_path.write_bytes(_FW_DATA)

    rollout_start = datetime.now(tz=UTC) - timedelta(days=30)  # far past
    cid = await _insert_campaign(
        db,
        firmware_path=str(fw_path),
        version=version,
        rollout_start=rollout_start,
    )

    yield cid, fw_path

    await _purge_campaign(db, cid)
    fw_path.unlink(missing_ok=True)


# ── T2-1: No active campaign ──────────────────────────────────────────────────

async def test_t2_1_no_campaign(dev):
    """When no in_progress campaign exists, body must NOT match the metadata regex."""
    meta = await dev.ota_poll()
    assert meta is None, "expected no metadata token when no campaign active"


# ── T2-2: Active campaign, device in current slot ─────────────────────────────

async def test_t2_2_metadata_token(dev, campaign):
    """Metadata token matches regex; V, L, H values match the test firmware."""
    cid, _ = campaign
    meta = await dev.ota_poll()
    assert meta is not None, "expected a metadata token for in-progress campaign"
    assert meta.version == 90001
    assert meta.size == _FW_SIZE
    assert meta.sha256 == _FW_SHA256
    assert meta.wait_seconds == 0


# ── T2-2a: Out-of-slot device ─────────────────────────────────────────────────

async def test_t2_2a_out_of_slot_w_field(db, firmware_dir, internal_url):
    """Out-of-slot device receives W.<positive> that is a multiple of slot_len_sec."""
    version = 90002
    fw_path = firmware_dir / f"v{version}.bin"
    fw_path.write_bytes(_FW_DATA)

    # rollout_start = now → now_slot = 0; find device with dev_slot > 0
    rollout_start = datetime.now(tz=UTC)
    station = _find_station_with_slot(lambda s: s > 0)
    device_id = f"{TEST_REGION:03d}{station:03d}"
    ds = _dev_slot(device_id)

    cid = await _insert_campaign(
        db,
        firmware_path=str(fw_path),
        version=version,
        rollout_start=rollout_start,
    )
    try:
        async with _make_dev(station, internal_url) as dev:
            meta = await dev.ota_poll()
        assert meta is not None
        assert meta.wait_seconds > 0, "expected W > 0 for out-of-slot device"
        assert meta.wait_seconds % _SLOT_LEN == 0, (
            f"W={meta.wait_seconds} not a multiple of {_SLOT_LEN}"
        )
        expected_w = ds * _SLOT_LEN  # now_slot=0, so (ds - 0) * slot_len
        assert meta.wait_seconds == expected_w
    finally:
        await _purge_campaign(db, cid)
        fw_path.unlink(missing_ok=True)


# ── T2-2b: NULL rollout_start → W absent or W.0 ──────────────────────────────

async def test_t2_2b_null_rollout_start(db, firmware_dir, internal_url):
    """Campaign with NULL rollout_start → W field absent (device immediately eligible)."""
    version = 90003
    fw_path = firmware_dir / f"v{version}.bin"
    fw_path.write_bytes(_FW_DATA)

    cid = await _insert_campaign(
        db,
        firmware_path=str(fw_path),
        version=version,
        rollout_start=None,
    )
    try:
        async with _make_dev(1, internal_url) as dev:
            meta = await dev.ota_poll()
        assert meta is not None
        assert meta.wait_seconds == 0, "NULL rollout_start must yield W.0 or no W field"
    finally:
        await _purge_campaign(db, cid)
        fw_path.unlink(missing_ok=True)


# ── T2-2c/2d: Malformed / missing id ─────────────────────────────────────────

@pytest.mark.parametrize(
    "params,expected_status",
    [
        ({"id": "abc123"}, 400),   # non-digit
        ({"id": "12345"},  400),   # 5 chars
        ({"id": ""},       400),   # empty string
        ({},               400),   # missing entirely
    ],
    ids=["non-digit", "5-chars", "empty", "missing"],
)
async def test_t2_2c_2d_bad_id(dev, params, expected_status, campaign):
    """Malformed or missing id → 400."""
    cid, _ = campaign
    r = await dev._client.get(
        f"{dev._base}/api/v1/weather/", params=params
    )
    assert r.status_code == expected_status, f"params={params!r}: got {r.status_code}"


# ── T2-3: Slot determinism ───────────────────────────────────────────────────

async def test_t2_3_slot_determinism(db, firmware_dir, internal_url):
    """W value for every device_id matches zlib.crc32 formula exactly."""
    version = 90004
    fw_path = firmware_dir / f"v{version}.bin"
    fw_path.write_bytes(_FW_DATA)

    # rollout_start = now → now_slot = 0
    rollout_start = datetime.now(tz=UTC)
    cid = await _insert_campaign(
        db,
        firmware_path=str(fw_path),
        version=version,
        rollout_start=rollout_start,
    )
    try:
        for station in range(20):
            device_id = f"{TEST_REGION:03d}{station:03d}"
            ds = _dev_slot(device_id)  # now_slot=0, so wait = ds * slot_len
            async with _make_dev(station, internal_url) as dev:
                meta = await dev.ota_poll()
            assert meta is not None
            expected_w = ds * _SLOT_LEN
            assert meta.wait_seconds == expected_w, (
                f"device_id={device_id}: expected W={expected_w}, got W={meta.wait_seconds}"
            )
    finally:
        await _purge_campaign(db, cid)
        fw_path.unlink(missing_ok=True)


# ── T2-4: Chunked download ────────────────────────────────────────────────────

async def test_t2_4_chunked_download(dev, campaign):
    """Full chunked download reassembles to SHA-256 from metadata."""
    cid, _ = campaign
    meta = await dev.ota_poll()
    assert meta is not None
    image = await dev.ota_download_all(meta.size, meta.sha256)
    assert image == _FW_DATA


# ── T2-4a: Out-of-slot GET /get_firmware → 429 ───────────────────────────────

async def test_t2_4a_out_of_slot_get_firmware(db, firmware_dir, internal_url):
    """Out-of-slot device gets 429 with Retry-After header from /get_firmware."""
    version = 90005
    fw_path = firmware_dir / f"v{version}.bin"
    fw_path.write_bytes(_FW_DATA)

    rollout_start = datetime.now(tz=UTC)
    station = _find_station_with_slot(lambda s: s > 0)
    cid = await _insert_campaign(
        db,
        firmware_path=str(fw_path),
        version=version,
        rollout_start=rollout_start,
    )
    try:
        async with _make_dev(station, internal_url) as dev:
            r = await dev._client.get(
                f"{dev._base}/api/v1/weather/get_firmware",
                params={"offset": 0, "length": 512, "id": dev.device_id},
            )
        assert r.status_code == 429
        assert "Retry-After" in r.headers
        retry_after = int(r.headers["Retry-After"])
        assert retry_after > 0
    finally:
        await _purge_campaign(db, cid)
        fw_path.unlink(missing_ok=True)


# ── T2-5: Per-chunk CRC ───────────────────────────────────────────────────────

async def test_t2_5_per_chunk_crc(dev, campaign):
    """Each chunk response ends with CRC-32/MPEG-2 of the preceding bytes."""
    cid, _ = campaign
    # Download first 3 chunks and verify CRC on each
    for chunk_offset in (0, 512, 1024):
        r = await dev._client.get(
            f"{dev._base}/api/v1/weather/get_firmware",
            params={"offset": chunk_offset, "length": 512, "id": dev.device_id},
        )
        r.raise_for_status()
        body = r.content
        assert len(body) >= 4
        chunk_data, crc_bytes = body[:-4], body[-4:]
        expected_crc = crc32_mpeg2(chunk_data)
        got_crc = int.from_bytes(crc_bytes, "little")
        assert got_crc == expected_crc, (
            f"offset={chunk_offset}: CRC mismatch {got_crc:#010x} != {expected_crc:#010x}"
        )


# ── T2-6: Resumable non-contiguous reads ─────────────────────────────────────

async def test_t2_6_resumable_reads(dev, campaign):
    """Non-contiguous chunk requests; reassembled image matches SHA-256."""
    cid, _ = campaign
    offsets = [0, 512, 1024, 1536]  # all chunks of 2 KB binary
    buf = bytearray(_FW_SIZE)
    for offset in offsets:
        length = min(512, _FW_SIZE - offset)
        r = await dev._client.get(
            f"{dev._base}/api/v1/weather/get_firmware",
            params={"offset": offset, "length": length, "id": dev.device_id},
        )
        r.raise_for_status()
        chunk_data = r.content[:-4]
        buf[offset: offset + len(chunk_data)] = chunk_data
    assert hashlib.sha256(bytes(buf)).hexdigest() == _FW_SHA256


# ── T2-7: Boundary (416) ─────────────────────────────────────────────────────

async def test_t2_7_boundary_416(dev, campaign):
    """offset + length > firmware_size → 416."""
    cid, _ = campaign
    r = await dev._client.get(
        f"{dev._base}/api/v1/weather/get_firmware",
        params={"offset": _FW_SIZE - 10, "length": 512, "id": dev.device_id},
    )
    assert r.status_code == 416


# ── T2-8: Length clamping ────────────────────────────────────────────────────

async def test_t2_8_length_clamp(dev, campaign):
    """length=0 clamped to 1; length=1024 clamped to 512."""
    cid, _ = campaign

    # length=0 → clamped to 1 → returns 1 byte of data + 4 CRC = 5 bytes
    r = await dev._client.get(
        f"{dev._base}/api/v1/weather/get_firmware",
        params={"offset": 0, "length": 0, "id": dev.device_id},
    )
    r.raise_for_status()
    assert len(r.content) == 5, f"expected 5 bytes (1 data + 4 CRC), got {len(r.content)}"

    # length=1024 → clamped to 512 → returns 512 bytes of data + 4 CRC = 516 bytes
    r = await dev._client.get(
        f"{dev._base}/api/v1/weather/get_firmware",
        params={"offset": 0, "length": 1024, "id": dev.device_id},
    )
    r.raise_for_status()
    assert len(r.content) == 516, f"expected 516 bytes (512 data + 4 CRC), got {len(r.content)}"


# ── T2-9: No campaign → GET /get_firmware 404 ─────────────────────────────────

async def test_t2_9_no_campaign_404(dev):
    """GET /get_firmware with no active campaign → 404."""
    r = await dev._client.get(
        f"{dev._base}/api/v1/weather/get_firmware",
        params={"offset": 0, "length": 512, "id": dev.device_id},
    )
    assert r.status_code == 404


# ── T2-10: Monotone retry ────────────────────────────────────────────────────

async def test_t2_10_monotone_retry(db, firmware_dir, internal_url):
    """Once eligible, always eligible: advancing now_slot past dev_slot keeps W=0."""
    version = 90006
    fw_path = firmware_dir / f"v{version}.bin"
    fw_path.write_bytes(_FW_DATA)

    # Find a device whose dev_slot is between 2 and num_slots-2 for maneuverability
    station = _find_station_with_slot(lambda s: 2 <= s <= _NUM_SLOTS - 2)
    device_id = f"{TEST_REGION:03d}{station:03d}"
    ds = _dev_slot(device_id)

    # Phase A: now_slot = ds - 1 → device is out-of-slot, W should be > 0
    cid = await _insert_campaign(
        db,
        firmware_path=str(fw_path),
        version=version,
        rollout_start=_rollout_start_for_now_slot(ds - 1),
    )
    try:
        async with _make_dev(station, internal_url) as dev:
            meta = await dev.ota_poll()
        assert meta is not None
        assert meta.wait_seconds > 0, f"expected W>0 at now_slot={ds-1}, dev_slot={ds}"

        # Phase B: now_slot = ds → device just becomes eligible, W should be 0
        await db.execute(
            "UPDATE ota_campaigns SET rollout_start = $1 WHERE id = $2",
            _rollout_start_for_now_slot(ds),
            cid,
        )
        async with _make_dev(station, internal_url) as dev:
            meta = await dev.ota_poll()
        assert meta is not None
        assert meta.wait_seconds == 0, f"expected W=0 at now_slot={ds}, dev_slot={ds}"

        # Phase C: now_slot = ds + 1 → still eligible (monotone)
        await db.execute(
            "UPDATE ota_campaigns SET rollout_start = $1 WHERE id = $2",
            _rollout_start_for_now_slot(ds + 1),
            cid,
        )
        async with _make_dev(station, internal_url) as dev:
            meta = await dev.ota_poll()
        assert meta is not None
        assert meta.wait_seconds == 0, (
            f"eligibility revoked at now_slot={ds+1} — monotone violated"
        )
    finally:
        await _purge_campaign(db, cid)
        fw_path.unlink(missing_ok=True)


# ── T2: download_completions tracking ────────────────────────────────────────

async def test_t2_completions_tracking(dev, db, campaign):
    """Every served chunk is recorded in download_completions (idempotent)."""
    cid, _ = campaign
    meta = await dev.ota_poll()
    assert meta is not None

    await dev.ota_download_all(meta.size, meta.sha256)

    expected_chunks = (meta.size + 511) // 512
    count = await db.fetchval(
        "SELECT COUNT(*) FROM download_completions WHERE campaign_id=$1 AND device_id=$2",
        cid,
        dev.device_id,
    )
    assert count == expected_chunks, (
        f"expected {expected_chunks} completions, got {count}"
    )

    # Second full download must not increase count (ON CONFLICT DO NOTHING)
    await dev.ota_download_all(meta.size, meta.sha256)
    count2 = await db.fetchval(
        "SELECT COUNT(*) FROM download_completions WHERE campaign_id=$1 AND device_id=$2",
        cid,
        dev.device_id,
    )
    assert count2 == count, "download_completions count grew on re-download (not idempotent)"
