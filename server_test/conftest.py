"""Shared fixtures for server integration tests.

Environment (.env file, gitignored):
  INTERNAL_URL   FastAPI direct URL, e.g. http://127.0.0.1:8000 (via SSH tunnel)
  BASE_URL       External Nginx URL, e.g. https://robin-gpu.cpe.ku.ac.th
  CA_BUNDLE      Path to pki/private_ca_chain.pem
  DEVICE_CERT    Path to device chain PEM (cert + intermediate)
  DEVICE_KEY     Path to device private key
  TEST_DB_DSN    asyncpg DSN for DB assertions (optional)
  ADMIN_USER     Admin username for integration tests (default: admin)
  ADMIN_PASS     Admin password for integration tests
"""
from __future__ import annotations

import os
from pathlib import Path

import asyncpg
import httpx
import pytest
import pytest_asyncio
from dotenv import load_dotenv

from lib.admin import AdminClient
from lib.mock_device import MockDevice

load_dotenv(Path(__file__).parent / ".env", override=False)

INTERNAL_URL = os.getenv("INTERNAL_URL", "")
BASE_URL     = os.getenv("BASE_URL", "")
CA_BUNDLE    = os.getenv("CA_BUNDLE", "")
DEVICE_CERT  = os.getenv("DEVICE_CERT", "")
DEVICE_KEY   = os.getenv("DEVICE_KEY", "")
TEST_DB_DSN  = os.getenv("TEST_DB_DSN", "")
# Absolute path to the server's firmware/ dir; must be writable from the test runner.
FIRMWARE_DIR = os.getenv("FIRMWARE_DIR", "")
ADMIN_USER   = os.getenv("ADMIN_USER", "admin")
ADMIN_PASS   = os.getenv("ADMIN_PASS", "")

# Region reserved for all integration test devices — never a real station.
TEST_REGION = 999


def _need(name: str, val: str) -> str:
    if not val:
        pytest.skip(f"{name} not configured in .env")
    return val


# ── Primary fixture: direct FastAPI (no Nginx, injected verify header) ────────
@pytest_asyncio.fixture
async def dev(request) -> MockDevice:
    """MockDevice hitting FastAPI directly with X-SSL-Client-Verify injected.

    Uses TEST_REGION=999 and a station id supplied via indirect parametrize or
    defaults to station 1.
    """
    station = getattr(request, "param", 1)
    url = _need("INTERNAL_URL", INTERNAL_URL)
    async with MockDevice(
        region=TEST_REGION,
        station=station,
        base_url=url,
        inject_verify_header=True,
    ) as d:
        yield d


# ── mTLS fixture: real cert through Nginx ─────────────────────────────────────
@pytest_asyncio.fixture
async def dev_mtls(request) -> MockDevice:
    """MockDevice with real mTLS through Nginx (requires Phase 4 deploy)."""
    station = getattr(request, "param", 1)
    url  = _need("BASE_URL", BASE_URL)
    cert = _need("DEVICE_CERT", DEVICE_CERT)
    key  = _need("DEVICE_KEY", DEVICE_KEY)
    ca   = _need("CA_BUNDLE", CA_BUNDLE)
    async with MockDevice(
        region=TEST_REGION,
        station=station,
        base_url=url,
        cert=(Path(cert), Path(key)),
        ca_bundle=Path(ca),
    ) as d:
        yield d


# ── No-cert fixture for 403 tests ─────────────────────────────────────────────
@pytest_asyncio.fixture
async def dev_no_cert() -> MockDevice:
    """MockDevice hitting FastAPI directly WITHOUT the verify header → 403."""
    url = _need("INTERNAL_URL", INTERNAL_URL)
    async with MockDevice(
        region=TEST_REGION,
        station=99,
        base_url=url,
        inject_verify_header=False,   # no header → mtls_required raises 403
    ) as d:
        yield d


# ── FIRMWARE_DIR fixture ──────────────────────────────────────────────────────
@pytest.fixture
def firmware_dir() -> Path:
    """Absolute path to server's firmware/ directory, writable from test runner."""
    return Path(_need("FIRMWARE_DIR", FIRMWARE_DIR))


# ── INTERNAL_URL string fixture ───────────────────────────────────────────────
@pytest.fixture
def internal_url() -> str:
    """FastAPI direct URL (no Nginx) for constructing ad-hoc MockDevices."""
    return _need("INTERNAL_URL", INTERNAL_URL)


# ── Raw string fixtures for T4 (mTLS / Nginx tests via BASE_URL) ──────────────
@pytest.fixture
def base_url() -> str:
    """External Nginx URL for mTLS tests."""
    return _need("BASE_URL", BASE_URL)


@pytest.fixture
def ca_bundle() -> str:
    """Path to CA bundle PEM for TLS verification."""
    return _need("CA_BUNDLE", CA_BUNDLE)


@pytest.fixture
def device_cert() -> tuple[str, str]:
    """(cert_path, key_path) for the shared device client cert."""
    _need("DEVICE_CERT", DEVICE_CERT)
    _need("DEVICE_KEY", DEVICE_KEY)
    return (DEVICE_CERT, DEVICE_KEY)


# ── DB connection for assertions ──────────────────────────────────────────────
@pytest_asyncio.fixture
async def db():
    """Read-write asyncpg connection for assertions and cleanup.

    Skipped when TEST_DB_DSN is not configured.
    """
    _need("TEST_DB_DSN", TEST_DB_DSN)
    conn = await asyncpg.connect(TEST_DB_DSN)
    yield conn
    await conn.close()


# ── Cleanup: remove region=999 rows after every test that touches DB ──────────
async def _purge_test_rows(conn: asyncpg.Connection) -> None:
    await conn.execute(
        """
        DELETE FROM weather_records
        WHERE device_id IN (SELECT id FROM devices WHERE region_id = $1)
        """,
        TEST_REGION,
    )
    await conn.execute(
        "DELETE FROM ingest_log WHERE idempotency_key LIKE $1",
        f"{TEST_REGION:03d}%",
    )
    await conn.execute("DELETE FROM devices WHERE region_id = $1", TEST_REGION)


@pytest_asyncio.fixture
async def db_cleanup(db):
    """Yield db connection with a clean slate; purge TEST_REGION rows before AND after.

    Pre-purge removes stale rows from tests that don't use this fixture.
    Post-purge removes rows inserted by the current test.
    """
    await _purge_test_rows(db)
    yield db
    await _purge_test_rows(db)


# ── Admin HTTP client ─────────────────────────────────────────────────────────

@pytest_asyncio.fixture
async def admin_client():
    """Unauthenticated httpx client pointed at INTERNAL_URL (admin endpoints)."""
    url = _need("INTERNAL_URL", INTERNAL_URL)
    async with httpx.AsyncClient(base_url=url) as client:
        yield client


@pytest_asyncio.fixture
async def admin_token(admin_client):
    """Login as ADMIN_USER/ADMIN_PASS and return the raw JWT string."""
    _need("ADMIN_PASS", ADMIN_PASS)
    r = await admin_client.post(
        "/admin/login",
        data={"username": ADMIN_USER, "password": ADMIN_PASS},
    )
    assert r.status_code == 200, f"Admin login failed: {r.text}"
    return r.json()["access_token"]


@pytest_asyncio.fixture
async def admin(admin_client, admin_token):
    """Authenticated AdminClient for Phase 7 campaign tests."""
    return AdminClient(admin_client, admin_token)


# ── Campaign cleanup: remove campaigns created during the test ─────────────────

async def _purge_campaigns_above(conn, pre_max: int) -> None:
    """Delete ota_campaigns with version > pre_max and their firmware files."""
    rows = await conn.fetch(
        "SELECT id, firmware_file_path FROM ota_campaigns WHERE version > $1", pre_max
    )
    if not rows:
        return
    ids = [r["id"] for r in rows]
    await conn.execute(
        "DELETE FROM download_completions WHERE campaign_id = ANY($1::int[])", ids
    )
    for r in rows:
        try:
            Path(r["firmware_file_path"]).unlink(missing_ok=True)
        except OSError:
            pass
    await conn.execute("DELETE FROM ota_campaigns WHERE id = ANY($1::int[])", ids)


@pytest_asyncio.fixture
async def campaign_cleanup(db):
    """Capture pre-test max campaign version; delete all test campaigns on teardown."""
    pre_max = (
        await db.fetchval("SELECT COALESCE(MAX(version), 0) FROM ota_campaigns") or 0
    )
    yield db
    await _purge_campaigns_above(db, pre_max)
