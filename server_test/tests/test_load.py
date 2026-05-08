"""T5-series: Soak, load, and failure-mode tests.

All tests are @pytest.mark.slow — run with:
  pytest server_test/tests/test_load.py -m slow

T5-1  Soak ingest   — 100 concurrent devices, configurable SOAK_DURATION_SEC
T5-2  OTA at scale  — 1000 device IDs poll; slot-0 cohort ≈ 50 ± 3·√50
T5-2a Slot advance  — advance rollout_start 1 slot; another ~50 become eligible
T5-2b Jitter dist   — CRC32-based first-chunk delays are spread, not bunched at 0
T5-3  Dup flood     — 50 identical concurrent uploads → exactly 1 DB row
T5-4  Net chaos     — disconnect mid-download; resume from saved offset; SHA-256 OK
T5-5  Power-loss    — partial FRAM-offset-tracked download; restart; image complete
T5-6  Fault inject  — 10 % chunk 500s; retry loop completes all 20 devices
"""
from __future__ import annotations

import asyncio
import hashlib
import os
import random
import statistics
import time
import zlib
from datetime import UTC, datetime, timedelta
from pathlib import Path

import asyncpg
import httpx
import pytest
import pytest_asyncio
from dotenv import load_dotenv

from lib.crc32 import crc32_mpeg2
from lib.mock_device import MockDevice
from lib.packed import make_sample

load_dotenv(Path(__file__).parent.parent / ".env", override=False)

INTERNAL_URL  = os.getenv("INTERNAL_URL", "")
TEST_DB_DSN   = os.getenv("TEST_DB_DSN", "")
FIRMWARE_DIR  = os.getenv("FIRMWARE_DIR", "")
SOAK_DURATION = int(os.getenv("SOAK_DURATION_SEC", "3600"))

TEST_REGION  = 999
TEST_REGION2 = 998
_SLOT_LEN    = 43200   # 12 h in seconds; must match server settings.slot_len_sec
_NUM_SLOTS   = 20      # rollout_window_days=10
_OTA_JITTER  = 1800    # seconds; mirrors firmware OTA_JITTER_MAX_SEC

_FW_SIZE   = 2048
_FW_DATA   = bytes(i % 256 for i in range(_FW_SIZE))
_FW_SHA256 = hashlib.sha256(_FW_DATA).hexdigest()

# Y2K epoch base for timestamp helpers
_Y2K_TS = int(datetime(2000, 1, 1, tzinfo=UTC).timestamp())
_RUN_TS  = int(time.time()) - _Y2K_TS


def _ts(offset: int = 0) -> int:
    return _RUN_TS + offset


# ── Utilities ─────────────────────────────────────────────────────────────────

def _need(name: str, val: str) -> str:
    if not val:
        pytest.skip(f"{name} not configured in .env")
    return val


def _dev_slot(device_id: str) -> int:
    return zlib.crc32(device_id.encode("ascii")) % _NUM_SLOTS


def _rollout_start_for_now_slot(desired: int) -> datetime:
    """Return rollout_start so that (now - start) // slot_len_sec == desired."""
    return datetime.now(UTC) - timedelta(seconds=desired * _SLOT_LEN + _SLOT_LEN // 2)


def _all_device_ids(num: int = 1000) -> list[tuple[int, int, str]]:
    """Return (region, station, device_id) across TEST_REGION and TEST_REGION2."""
    out = []
    for i in range(num):
        region  = TEST_REGION if i < 500 else TEST_REGION2
        station = (i % 500) + 1
        out.append((region, station, f"{region:03d}{station:03d}"))
    return out


async def _purge_region(conn: asyncpg.Connection, region: int) -> None:
    await conn.execute(
        "DELETE FROM weather_records"
        " WHERE device_id IN (SELECT id FROM devices WHERE region_id=$1)",
        region,
    )
    await conn.execute(
        "DELETE FROM ingest_log WHERE idempotency_key LIKE $1",
        f"{region:03d}%",
    )
    await conn.execute("DELETE FROM devices WHERE region_id=$1", region)


async def _make_campaign(
    conn: asyncpg.Connection,
    fw_dir: Path,
    rollout_start: datetime,
    rollout_window_days: int = 10,
) -> tuple[int, int]:
    version = await conn.fetchval(
        "SELECT COALESCE(MAX(version), 900000000) + 1"
        " FROM ota_campaigns WHERE version >= 900000000"
    )
    fw_path = fw_dir / f"v{version}.bin"
    fw_path.write_bytes(_FW_DATA)
    row = await conn.fetchrow(
        """
        INSERT INTO ota_campaigns
            (version, firmware_file_path, firmware_sha256, firmware_size, status,
             rollout_start, rollout_window_days)
        VALUES ($1,$2,$3,$4,'in_progress',$5,$6)
        RETURNING id, version
        """,
        version, str(fw_path), _FW_SHA256, _FW_SIZE, rollout_start, rollout_window_days,
    )
    return row["id"], row["version"]


async def _delete_campaign(
    conn: asyncpg.Connection, campaign_id: int, fw_dir: Path, version: int
) -> None:
    await conn.execute(
        "DELETE FROM download_completions WHERE campaign_id=$1", campaign_id
    )
    await conn.execute("DELETE FROM ota_campaigns WHERE id=$1", campaign_id)
    (fw_dir / f"v{version}.bin").unlink(missing_ok=True)


# ── Fixtures ──────────────────────────────────────────────────────────────────

@pytest_asyncio.fixture
async def load_db():
    _need("TEST_DB_DSN", TEST_DB_DSN)
    conn = await asyncpg.connect(TEST_DB_DSN)
    yield conn
    await conn.close()


@pytest.fixture
def load_fw_dir() -> Path:
    return Path(_need("FIRMWARE_DIR", FIRMWARE_DIR))


@pytest.fixture
def load_url() -> str:
    return _need("INTERNAL_URL", INTERNAL_URL)


@pytest_asyncio.fixture
async def campaign_at_slot0(load_db, load_fw_dir):
    """Active campaign with rollout_start set so now_slot == 0."""
    rollout_start = _rollout_start_for_now_slot(0)
    camp_id, version = await _make_campaign(load_db, load_fw_dir, rollout_start)
    yield load_db, camp_id, version, rollout_start
    await _delete_campaign(load_db, camp_id, load_fw_dir, version)


# ── T5-1: Soak ingest ─────────────────────────────────────────────────────────

@pytest.mark.slow
async def test_t5_1_soak_ingest(load_url):
    """100 concurrent devices upload every 60 s for SOAK_DURATION_SEC.
    Pass criteria: no 5xx responses; p95 latency < 500 ms.
    """
    NUM_DEVICES = 100
    INTERVAL    = 60.0

    latencies: list[float] = []
    server_errors: list[int] = []
    deadline = time.monotonic() + SOAK_DURATION

    async def device_loop(station: int) -> None:
        # Stagger initial upload over the first interval to avoid thundering herd.
        await asyncio.sleep(random.uniform(0, INTERVAL))
        sample = make_sample(ts=_ts(station))
        async with MockDevice(
            region=TEST_REGION, station=station,
            base_url=load_url, inject_verify_header=True,
        ) as dev:
            while time.monotonic() < deadline:
                t0 = time.monotonic()
                try:
                    await dev.upload([sample])
                except httpx.HTTPStatusError as exc:
                    if exc.response.status_code >= 500:
                        server_errors.append(exc.response.status_code)
                finally:
                    latencies.append((time.monotonic() - t0) * 1000)
                await asyncio.sleep(INTERVAL)

    await asyncio.gather(*(device_loop(s) for s in range(1, NUM_DEVICES + 1)))

    assert not server_errors, (
        f"{len(server_errors)} 5xx errors during soak: {server_errors[:10]}"
    )
    assert latencies, "No uploads completed during soak"
    p95 = statistics.quantiles(latencies, n=100)[94]
    assert p95 < 500, f"p95 latency {p95:.1f} ms exceeds 500 ms threshold"


# ── T5-2: OTA rollout at scale ─────────────────────────────────────────────────

@pytest.mark.slow
async def test_t5_2_ota_at_scale(campaign_at_slot0, load_url):
    """1000 device IDs poll simultaneously.
    With rollout_window_days=10 (20 slots), slot 0 cohort ≈ 50 devices.
    Tolerance: 29–71 (3·√50 ≈ 21 either side).
    """
    db, camp_id, version, _ = campaign_at_slot0
    devices = _all_device_ids(1000)
    sem = asyncio.Semaphore(50)  # cap concurrent HTTP connections to server

    async def poll_one(region: int, station: int) -> int:
        async with sem:
            async with MockDevice(
                region=region, station=station,
                base_url=load_url, inject_verify_header=True,
            ) as dev:
                meta = await dev.ota_poll()
                return meta.wait_seconds if (meta and meta.version == version) else -1

    results = await asyncio.gather(*(poll_one(r, s) for r, s, _ in devices))

    eligible = sum(1 for w in results if w == 0)
    not_yet  = sum(1 for w in results if w > 0)

    assert eligible + not_yet == 1000, (
        f"Some devices did not see the campaign: "
        f"eligible={eligible}, not_yet={not_yet}, missing={1000 - eligible - not_yet}"
    )
    assert 29 <= eligible <= 71, (
        f"Expected ~50 slot-0 eligible devices, got {eligible}"
    )


# ── T5-2a: Slot advancement ────────────────────────────────────────────────────

@pytest.mark.slow
async def test_t5_2a_slot_advancement(campaign_at_slot0, load_url):
    """Advance rollout_start 1 slot; ~50 more devices become eligible.
    Previously eligible devices remain eligible (monotone invariant).
    """
    db, camp_id, version, original_start = campaign_at_slot0
    devices = _all_device_ids(1000)
    sem = asyncio.Semaphore(50)

    async def poll_wait(region: int, station: int) -> int:
        async with sem:
            async with MockDevice(
                region=region, station=station,
                base_url=load_url, inject_verify_header=True,
            ) as dev:
                meta = await dev.ota_poll()
                return meta.wait_seconds if (meta and meta.version == version) else -1

    # Baseline: slot-0 eligible set
    baseline  = await asyncio.gather(*(poll_wait(r, s) for r, s, _ in devices))
    slot0_set = {devices[i][2] for i, w in enumerate(baseline) if w == 0}

    # Advance rollout_start so now_slot becomes 1
    new_start = _rollout_start_for_now_slot(1)
    try:
        await db.execute(
            "UPDATE ota_campaigns SET rollout_start=$1 WHERE id=$2",
            new_start, camp_id,
        )

        advanced = await asyncio.gather(*(poll_wait(r, s) for r, s, _ in devices))
        eligible_after = {devices[i][2] for i, w in enumerate(advanced) if w == 0}
    finally:
        await db.execute(
            "UPDATE ota_campaigns SET rollout_start=$1 WHERE id=$2",
            original_start, camp_id,
        )

    gained = len(eligible_after) - len(slot0_set)
    assert 29 <= gained <= 71, (
        f"Expected ~50 new eligible after 1-slot advance, gained {gained} "
        f"(slot0={len(slot0_set)}, slot1={len(eligible_after)})"
    )

    # Monotone: slot-0 devices must still be eligible at slot 1
    lost = slot0_set - eligible_after
    assert not lost, f"Monotone violated: {len(lost)} devices lost eligibility"


# ── T5-2b: Jitter distribution ─────────────────────────────────────────────────

@pytest.mark.slow
async def test_t5_2b_jitter_distribution(campaign_at_slot0, load_url):
    """CRC32-based first-chunk delays for in-slot devices should be spread
    uniformly across [0, OTA_JITTER_MAX_SEC), not bunched at 0.

    Distribution check: split into 5 equal buckets; no bucket may contain
    more than 3× the expected count (very loose — guards against all-zero).
    """
    db, camp_id, version, _ = campaign_at_slot0
    devices = _all_device_ids(1000)
    sem = asyncio.Semaphore(50)

    async def poll_one(region: int, station: int, dev_id: str) -> str | None:
        async with sem:
            async with MockDevice(
                region=region, station=station,
                base_url=load_url, inject_verify_header=True,
            ) as dev:
                meta = await dev.ota_poll()
                if meta and meta.version == version and meta.wait_seconds == 0:
                    return dev_id
                return None

    eligible_ids = [
        r for r in await asyncio.gather(*(poll_one(r, s, d) for r, s, d in devices))
        if r is not None
    ]
    assert len(eligible_ids) >= 20, (
        f"Too few in-slot devices for distribution check ({len(eligible_ids)})"
    )

    # Compute the jitter delay each device would sleep before its first chunk.
    delays = [crc32_mpeg2(dev_id.encode()) % _OTA_JITTER for dev_id in eligible_ids]

    bucket_size = _OTA_JITTER // 5
    buckets = [0] * 5
    for d in delays:
        buckets[min(d // bucket_size, 4)] += 1

    max_bucket       = max(buckets)
    expected_per_bucket = len(delays) / 5
    assert max_bucket <= 3 * expected_per_bucket, (
        f"Jitter distribution not uniform: buckets={buckets}, max={max_bucket}, "
        f"expected≈{expected_per_bucket:.1f}"
    )


# ── T5-3: Duplicate flood ─────────────────────────────────────────────────────

@pytest.mark.slow
async def test_t5_3_duplicate_flood(load_db, load_url):
    """50 concurrent identical uploads → exactly 1 weather_records row.
    Tests that DB-level idempotency holds under concurrency (no race inserts).
    """
    db      = load_db
    STATION = 201
    sample  = make_sample(ts=_ts(300))

    await _purge_region(db, TEST_REGION)

    async with MockDevice(
        region=TEST_REGION, station=STATION,
        base_url=load_url, inject_verify_header=True,
    ) as dev:
        # upload() raises only on 4xx/5xx; duplicates return 200 {"status":"duplicate"}
        await asyncio.gather(*(dev.upload([sample]) for _ in range(50)))

    row_count = await db.fetchval(
        """
        SELECT COUNT(*) FROM weather_records
        WHERE device_id = (
            SELECT id FROM devices
            WHERE region_id=$1 AND station_id=$2
        )
        """,
        TEST_REGION, STATION,
    )
    assert row_count == 1, (
        f"Expected exactly 1 row after 50 concurrent identical uploads, got {row_count}"
    )
    await _purge_region(db, TEST_REGION)


# ── T5-4: Network chaos — mid-download disconnect and resume ──────────────────

@pytest.mark.slow
async def test_t5_4_net_chaos_resume(campaign_at_slot0, load_url):
    """Download 3 chunks then close the client (simulate network loss).
    A new client resumes from the saved offset and reassembles the full image.
    SHA-256 of the reassembled image must match.
    """
    db, camp_id, version, _ = campaign_at_slot0

    # Find any slot-0 device
    in_slot = next(
        (r, s, d) for r, s, d in _all_device_ids() if _dev_slot(d) == 0
    )
    region, station, dev_id = in_slot

    CHUNK  = 512
    buf    = b""

    # Phase 1: download first 3 chunks, then "disconnect" (context manager exit)
    async with MockDevice(
        region=region, station=station,
        base_url=load_url, inject_verify_header=True,
    ) as dev:
        meta = await dev.ota_poll()
        assert meta and meta.version == version and meta.wait_seconds == 0, (
            f"Device {dev_id} not in slot 0 or wrong campaign version"
        )
        for _ in range(3):
            offset = len(buf)
            length = min(CHUNK, _FW_SIZE - offset)
            r = await dev._client.get(
                f"{load_url}/api/v1/weather/get_firmware",
                params={"offset": offset, "length": length, "id": dev_id},
            )
            r.raise_for_status()
            body = r.content
            chunk_data, crc_bytes = body[:-4], body[-4:]
            assert crc32_mpeg2(chunk_data) == int.from_bytes(crc_bytes, "little"), (
                f"CRC mismatch on chunk at offset {offset}"
            )
            buf += chunk_data
    # Client closed here — simulates network loss

    resume_offset = len(buf)

    # Phase 2: new client resumes from saved offset
    async with MockDevice(
        region=region, station=station,
        base_url=load_url, inject_verify_header=True,
    ) as dev2:
        while resume_offset < _FW_SIZE:
            length = min(CHUNK, _FW_SIZE - resume_offset)
            r = await dev2._client.get(
                f"{load_url}/api/v1/weather/get_firmware",
                params={"offset": resume_offset, "length": length, "id": dev_id},
            )
            r.raise_for_status()
            body = r.content
            chunk_data, crc_bytes = body[:-4], body[-4:]
            assert crc32_mpeg2(chunk_data) == int.from_bytes(crc_bytes, "little")
            buf += chunk_data
            resume_offset += len(chunk_data)

    assert hashlib.sha256(buf).hexdigest() == _FW_SHA256, "Reassembled image SHA-256 mismatch"


# ── T5-5: Power-loss simulation ───────────────────────────────────────────────

@pytest.mark.slow
async def test_t5_5_power_loss_resume(campaign_at_slot0, load_url):
    """Download STOP_AFTER chunks and record the FRAM offset.
    After simulated power-loss, a new client reads fram_offset and resumes.
    The reassembled image's SHA-256 must match.
    """
    db, camp_id, version, _ = campaign_at_slot0

    in_slot = next(
        (r, s, d) for r, s, d in _all_device_ids() if _dev_slot(d) == 0
    )
    region, station, dev_id = in_slot

    CHUNK       = 512
    STOP_AFTER  = 2   # chunks written before simulated power loss
    fram_offset = 0   # non-volatile offset tracking (FRAM analogue)
    buf         = b""

    # Phase 1: download STOP_AFTER chunks
    async with MockDevice(
        region=region, station=station,
        base_url=load_url, inject_verify_header=True,
    ) as dev:
        meta = await dev.ota_poll()
        assert meta and meta.version == version and meta.wait_seconds == 0

        for _ in range(STOP_AFTER):
            length = min(CHUNK, _FW_SIZE - fram_offset)
            r = await dev._client.get(
                f"{load_url}/api/v1/weather/get_firmware",
                params={"offset": fram_offset, "length": length, "id": dev_id},
            )
            r.raise_for_status()
            body = r.content
            chunk_data, crc_bytes = body[:-4], body[-4:]
            assert crc32_mpeg2(chunk_data) == int.from_bytes(crc_bytes, "little")
            buf += chunk_data
            fram_offset += len(chunk_data)
    # Power-loss here — fram_offset survives (non-volatile)

    # Phase 2: restart; resume from fram_offset
    async with MockDevice(
        region=region, station=station,
        base_url=load_url, inject_verify_header=True,
    ) as dev2:
        while fram_offset < _FW_SIZE:
            length = min(CHUNK, _FW_SIZE - fram_offset)
            r = await dev2._client.get(
                f"{load_url}/api/v1/weather/get_firmware",
                params={"offset": fram_offset, "length": length, "id": dev_id},
            )
            r.raise_for_status()
            body = r.content
            chunk_data, crc_bytes = body[:-4], body[-4:]
            assert crc32_mpeg2(chunk_data) == int.from_bytes(crc_bytes, "little")
            buf += chunk_data
            fram_offset += len(chunk_data)

    assert hashlib.sha256(buf).hexdigest() == _FW_SHA256, "Post-resume image SHA-256 mismatch"


# ── T5-6: Fault injection at scale ───────────────────────────────────────────

class _FaultyTransport(httpx.AsyncBaseTransport):
    """Wraps a real transport; injects HTTP 500 on `fault_path` at `fail_rate`."""

    def __init__(
        self,
        wrapped: httpx.AsyncBaseTransport,
        fail_rate: float,
        fault_path: str,
    ) -> None:
        self._wrapped   = wrapped
        self._fail_rate = fail_rate
        self._fault_path = fault_path

    async def handle_async_request(self, request: httpx.Request) -> httpx.Response:
        if self._fault_path in request.url.path and random.random() < self._fail_rate:
            return httpx.Response(500, content=b"injected fault", request=request)
        return await self._wrapped.handle_async_request(request)

    async def aclose(self) -> None:
        await self._wrapped.aclose()


@pytest.mark.slow
async def test_t5_6_fault_injection_retry(campaign_at_slot0, load_url):
    """20 in-slot devices download with 10 % chunk 500 failure rate.
    Each device retries failed chunks until the complete image is assembled.
    All devices complete successfully with matching SHA-256 (zero admin intervention).
    """
    db, camp_id, version, _ = campaign_at_slot0

    slot0_devs = [(r, s, d) for r, s, d in _all_device_ids() if _dev_slot(d) == 0][:20]
    assert len(slot0_devs) >= 5, (
        f"Need at least 5 slot-0 test devices, found {len(slot0_devs)}"
    )

    CHUNK             = 512
    FAIL_RATE         = 0.10
    MAX_CHUNK_RETRIES = 30

    async def download_with_retries(region: int, station: int, dev_id: str) -> bytes:
        transport = _FaultyTransport(
            httpx.AsyncHTTPTransport(), FAIL_RATE, "/get_firmware"
        )
        async with httpx.AsyncClient(
            transport=transport,
            headers={"X-SSL-Client-Verify": "SUCCESS"},
            timeout=30,
        ) as client:
            buf    = b""
            offset = 0
            while offset < _FW_SIZE:
                length = min(CHUNK, _FW_SIZE - offset)
                for attempt in range(MAX_CHUNK_RETRIES):
                    r = await client.get(
                        f"{load_url}/api/v1/weather/get_firmware",
                        params={"offset": offset, "length": length, "id": dev_id},
                    )
                    if r.status_code == 200:
                        body = r.content
                        chunk_data, crc_bytes = body[:-4], body[-4:]
                        assert crc32_mpeg2(chunk_data) == int.from_bytes(crc_bytes, "little"), (
                            f"CRC mismatch at offset {offset}"
                        )
                        buf += chunk_data
                        offset += len(chunk_data)
                        break
                    if attempt == MAX_CHUNK_RETRIES - 1:
                        raise AssertionError(
                            f"Device {dev_id} chunk at offset {offset} failed "
                            f"after {MAX_CHUNK_RETRIES} retries"
                        )
            return buf

    images = await asyncio.gather(
        *(download_with_retries(r, s, d) for r, s, d in slot0_devs)
    )

    for img, (_, _, dev_id) in zip(images, slot0_devs):
        assert hashlib.sha256(img).hexdigest() == _FW_SHA256, (
            f"Device {dev_id}: reassembled image SHA-256 mismatch"
        )
