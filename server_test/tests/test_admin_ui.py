"""Phase 8 Admin UI end-to-end integration tests (T4-series).

Exercises the HTMX + Jinja2 operator interface via HTTP (browser simulation).
Requires INTERNAL_URL, ADMIN_USER, ADMIN_PASS in .env.
DB-backed tests also require TEST_DB_DSN and FIRMWARE_DIR.
"""
from __future__ import annotations

import os
import re

import httpx
import pytest
import pytest_asyncio

pytestmark = pytest.mark.asyncio

# conftest.py already ran load_dotenv() before this module is imported.
_INTERNAL_URL = os.getenv("INTERNAL_URL", "")
_ADMIN_USER   = os.getenv("ADMIN_USER", "admin")
_ADMIN_PASS   = os.getenv("ADMIN_PASS", "")

_FW_BYTES = bytes(i % 251 for i in range(2048))  # 2 KB deterministic test firmware

# CSRF token extraction: hx-headers attribute (authenticated pages) or form hidden input (login).
_CSRF_HX_RE   = re.compile(r'"X-CSRF-Token":\s*"([^"]+)"')
_CSRF_FORM_RE = re.compile(r'name="csrf_token"\s+value="([^"]+)"')


def _need(name: str, val: str) -> str:
    if not val:
        pytest.skip(f"{name} not configured in .env")
    return val


def _extract_csrf(html: str) -> str:
    """Return the first CSRF token found in the HTML, from hx-headers or form input."""
    m = _CSRF_HX_RE.search(html) or _CSRF_FORM_RE.search(html)
    assert m, f"CSRF token missing in HTML:\n{html[:500]}"
    return m.group(1)


# ── Fixtures ──────────────────────────────────────────────────────────────────

@pytest_asyncio.fixture
async def ui_client():
    """Browser-like httpx client: follows redirects, persists cookies across requests."""
    url = _need("INTERNAL_URL", _INTERNAL_URL)
    async with httpx.AsyncClient(base_url=url, follow_redirects=True) as client:
        yield client


@pytest_asyncio.fixture
async def logged_in(ui_client):
    """ui_client pre-authenticated as admin (cookie set); yields the client."""
    _need("ADMIN_PASS", _ADMIN_PASS)
    # GET login page to obtain a fresh CSRF token.
    page = await ui_client.get("/admin/login.html")
    assert page.status_code == 200
    csrf = _extract_csrf(page.text)
    r = await ui_client.post(
        "/admin/login.html",
        data={"username": _ADMIN_USER, "password": _ADMIN_PASS, "csrf_token": csrf},
    )
    assert r.status_code == 200, f"UI login failed: {r.status_code} — {r.text[:300]}"
    assert "campaigns" in r.url.path, (
        f"Expected redirect to /admin/campaigns, got {r.url.path}"
    )
    return ui_client


# ── T4-1: Login page ──────────────────────────────────────────────────────────

async def test_t4_1_login_page_renders(ui_client):
    r = await ui_client.get("/admin/login.html")
    assert r.status_code == 200
    assert _CSRF_FORM_RE.search(r.text), "login.html must embed csrf_token hidden input"
    assert "Sign In" in r.text


# ── T4-2: Bad credentials show inline error ───────────────────────────────────

async def test_t4_2_bad_credentials_returns_error(ui_client):
    page = await ui_client.get("/admin/login.html")
    csrf = _extract_csrf(page.text)
    r = await ui_client.post(
        "/admin/login.html",
        data={"username": "admin", "password": "totally_wrong", "csrf_token": csrf},
    )
    assert r.status_code == 401
    assert "Invalid username or password" in r.text


# ── T4-3: Invalid CSRF rejected ───────────────────────────────────────────────

async def test_t4_3_invalid_csrf_login_rejected(ui_client):
    r = await ui_client.post(
        "/admin/login.html",
        data={"username": "admin", "password": "any", "csrf_token": "bad_token"},
    )
    assert r.status_code == 400
    assert "CSRF" in r.text


# ── T4-4: Unauthenticated access redirects to login ──────────────────────────

async def test_t4_4_campaigns_unauthenticated_redirects(ui_client):
    r = await ui_client.get("/admin/campaigns")
    assert "/admin/login.html" in r.url.path


async def test_t4_4_dashboard_unauthenticated_redirects(ui_client):
    r = await ui_client.get("/admin/dashboard")
    assert "/admin/login.html" in r.url.path


# ── T4-5: Authenticated pages render ─────────────────────────────────────────

async def test_t4_5_campaigns_page_loads(logged_in):
    r = await logged_in.get("/admin/campaigns")
    assert r.status_code == 200
    assert "Campaigns" in r.text
    # Admin role: upload form with CSRF must be present.
    assert _CSRF_FORM_RE.search(r.text), "campaigns.html must embed csrf_token in upload form"


async def test_t4_5_dashboard_page_loads(logged_in):
    r = await logged_in.get("/admin/dashboard")
    assert r.status_code == 200


async def test_t4_5_device_table_partial_loads(logged_in):
    r = await logged_in.get("/admin/devices/table?page=1")
    assert r.status_code == 200


async def test_t4_5_campaigns_list_partial_loads(logged_in):
    r = await logged_in.get("/admin/campaigns/list")
    assert r.status_code == 200


# ── T4-6: Firmware upload via UI form ────────────────────────────────────────

async def test_t4_6_firmware_upload_ui_success(logged_in, campaign_cleanup):
    page = await logged_in.get("/admin/campaigns")
    csrf = _extract_csrf(page.text)

    r = await logged_in.post(
        "/admin/firmware/upload-ui",
        data={"csrf_token": csrf},
        files={"file": ("fw.bin", _FW_BYTES, "application/octet-stream")},
    )
    assert r.status_code == 200, r.text
    assert "Uploaded v" in r.text
    assert "SHA-256" in r.text
    assert "Campaign #" in r.text
    assert "draft" in r.text.lower()


async def test_t4_6_oversize_upload_shows_error(logged_in, campaign_cleanup):
    page = await logged_in.get("/admin/campaigns")
    csrf = _extract_csrf(page.text)

    r = await logged_in.post(
        "/admin/firmware/upload-ui",
        data={"csrf_token": csrf},
        files={"file": ("big.bin", bytes(491521), "application/octet-stream")},
    )
    assert r.status_code == 200  # UI returns 200 with error fragment (not 413)
    assert "too large" in r.text.lower()


async def test_t4_6_upload_invalid_csrf_rejected(logged_in, campaign_cleanup):
    r = await logged_in.post(
        "/admin/firmware/upload-ui",
        data={"csrf_token": "tampered"},
        files={"file": ("fw.bin", _FW_BYTES, "application/octet-stream")},
    )
    assert r.status_code == 400
    assert "CSRF" in r.text


# ── T4-7: Full end-to-end flow: upload → start → detail → progress ───────────

async def test_t4_7_full_rollout_flow(logged_in, campaign_cleanup):
    # Step 1: Upload firmware binary.
    page = await logged_in.get("/admin/campaigns")
    csrf = _extract_csrf(page.text)

    upload_r = await logged_in.post(
        "/admin/firmware/upload-ui",
        data={"csrf_token": csrf},
        files={"file": ("fw.bin", _FW_BYTES, "application/octet-stream")},
    )
    assert upload_r.status_code == 200, upload_r.text

    m = re.search(r"Campaign #(\d+)", upload_r.text)
    assert m, f"Campaign ID not in upload response:\n{upload_r.text[:400]}"
    campaign_id = int(m.group(1))

    # Step 2: Start the rollout.
    # HTMX injects X-CSRF-Token from hx-headers on the <body> tag.
    campaigns_page = await logged_in.get("/admin/campaigns")
    csrf2 = _extract_csrf(campaigns_page.text)

    start_r = await logged_in.post(
        f"/admin/campaign/{campaign_id}/start-ui",
        data={"rollout_window_days": "10"},
        headers={"X-CSRF-Token": csrf2},
    )
    assert start_r.status_code == 200, start_r.text
    # Campaign list partial reflects new status badge.
    assert "in progress" in start_r.text  # `_` replaced by ` ` in template filter

    # Step 3: Campaign detail page.
    detail_r = await logged_in.get(f"/admin/campaign/{campaign_id}/detail")
    assert detail_r.status_code == 200
    assert str(campaign_id) in detail_r.text

    # Step 4: Progress partial (polled by HTMX every 15 s in the browser).
    progress_r = await logged_in.get(f"/admin/campaign/{campaign_id}/progress")
    assert progress_r.status_code == 200
    assert "Devices completed" in progress_r.text


# ── T4-8: Logout clears the session cookie ───────────────────────────────────

async def test_t4_8_logout_clears_session(logged_in):
    page = await logged_in.get("/admin/campaigns")
    csrf = _extract_csrf(page.text)

    logout_r = await logged_in.post("/admin/logout-ui", data={"csrf_token": csrf})
    # Logout redirects to login (follow_redirects=True, so status is 200 at final URL).
    assert logout_r.status_code == 200
    assert "/admin/login.html" in logout_r.url.path

    # Cookie cleared from jar — protected page must redirect back to login.
    assert not logged_in.cookies.get("access_token")
    r = await logged_in.get("/admin/campaigns")
    assert "/admin/login.html" in r.url.path
