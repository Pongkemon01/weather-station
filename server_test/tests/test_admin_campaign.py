"""Phase 7 admin campaign lifecycle integration tests (T3-series).

Uses INTERNAL_URL + admin JWT (no mTLS needed for admin path).
DB assertions require TEST_DB_DSN.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

import bcrypt
import pytest

pytestmark = pytest.mark.asyncio

# Deterministic firmware payloads — reproducible SHA-256 every run.
_SMALL_FW = bytes(i % 251 for i in range(2048))          # 2 KB
_SMALL_FW_2 = bytes((i + 7) % 251 for i in range(2048))  # distinct 2 KB
_LARGE_FW = bytes(i % 251 for i in range(409600))         # 400 KB


# ── T3-1: Auth smoke tests ────────────────────────────────────────────────────

async def test_t3_1_wrong_password_returns_401(admin_client):
    r = await admin_client.post(
        "/admin/login", data={"username": "admin", "password": "definitely_wrong"}
    )
    assert r.status_code == 401


async def test_t3_1_viewer_cannot_upload(admin_client, db):
    pw_hash = bcrypt.hashpw(b"viewerpass", bcrypt.gensalt()).decode()
    await db.execute(
        """
        INSERT INTO admin_users (username, password_hash, role)
        VALUES ('_t3_viewer', $1, 'viewer')
        ON CONFLICT (username) DO UPDATE SET password_hash = $1, role = 'viewer'
        """,
        pw_hash,
    )
    try:
        login_r = await admin_client.post(
            "/admin/login", data={"username": "_t3_viewer", "password": "viewerpass"}
        )
        viewer_token = login_r.json()["access_token"]
        r = await admin_client.post(
            "/admin/firmware/upload",
            headers={"Authorization": f"Bearer {viewer_token}"},
            files={"file": ("fw.bin", _SMALL_FW, "application/octet-stream")},
        )
        assert r.status_code == 403
    finally:
        await db.execute("DELETE FROM admin_users WHERE username = '_t3_viewer'")


# ── T3-2: Firmware upload ─────────────────────────────────────────────────────

async def test_t3_2_firmware_upload_response(admin, db, firmware_dir, campaign_cleanup):
    prev_max = await db.fetchval("SELECT COALESCE(MAX(version), 0) FROM ota_campaigns") or 0
    r = await admin.upload_firmware(_SMALL_FW)
    assert r.status_code == 200, r.text
    body = r.json()

    assert body["firmware_sha256"] == hashlib.sha256(_SMALL_FW).hexdigest()
    assert body["firmware_size"] == len(_SMALL_FW)
    assert body["version"] == prev_max + 1


async def test_t3_2_firmware_upload_db_row(admin, db, campaign_cleanup):
    r = await admin.upload_firmware(_SMALL_FW)
    cid = r.json()["id"]
    row = await db.fetchrow("SELECT * FROM ota_campaigns WHERE id = $1", cid)
    assert row is not None
    assert row["status"] == "draft"
    assert row["firmware_size"] == len(_SMALL_FW)
    assert row["firmware_sha256"] == hashlib.sha256(_SMALL_FW).hexdigest()


async def test_t3_2_firmware_file_exists_on_disk(admin, db, firmware_dir, campaign_cleanup):
    r = await admin.upload_firmware(_SMALL_FW)
    body = r.json()
    fw_file = firmware_dir / f"v{body['version']}.bin"
    assert fw_file.exists()
    assert hashlib.sha256(fw_file.read_bytes()).hexdigest() == body["firmware_sha256"]


# ── T3-3: Version auto-increment ─────────────────────────────────────────────

async def test_t3_3_version_increments(admin, campaign_cleanup):
    r1 = await admin.upload_firmware(_SMALL_FW)
    assert r1.status_code == 200
    v1 = r1.json()["version"]

    r2 = await admin.upload_firmware(_SMALL_FW_2)
    assert r2.status_code == 200
    assert r2.json()["version"] == v1 + 1


# ── T3-2/T3-3: Oversize upload rejected ──────────────────────────────────────

async def test_t3_2_oversize_rejected(admin, campaign_cleanup):
    # 480 KB + 1 byte over the limit (480 * 1024 + 1 = 491521)
    oversize = bytes(491521)
    r = await admin.upload_firmware(oversize)
    assert r.status_code == 413


# ── T3-4: Start rollout ───────────────────────────────────────────────────────

async def test_t3_4_start_sets_in_progress(admin, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]

    r = await admin.start_campaign(cid, rollout_window_days=10)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["status"] == "in_progress"
    assert body["rollout_window_days"] == 10
    assert body["rollout_start"] is not None


async def test_t3_4_start_sets_rollout_start_in_db(admin, db, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid)
    row = await db.fetchrow("SELECT status, rollout_start FROM ota_campaigns WHERE id = $1", cid)
    assert row["status"] == "in_progress"
    assert row["rollout_start"] is not None


# ── T3-4a: Start invariants ───────────────────────────────────────────────────

async def test_t3_4a_window_days_zero_rejected(admin, campaign_cleanup):
    r1 = await admin.upload_firmware(_SMALL_FW)
    cid = r1.json()["id"]
    r = await admin.start_campaign(cid, rollout_window_days=0)
    assert r.status_code == 422  # Pydantic ge=1 constraint


async def test_t3_4a_window_days_31_rejected(admin, campaign_cleanup):
    r1 = await admin.upload_firmware(_SMALL_FW)
    cid = r1.json()["id"]
    r = await admin.start_campaign(cid, rollout_window_days=31)
    assert r.status_code == 422  # Pydantic le=30 constraint


async def test_t3_4a_default_window_days_is_10(admin, db, campaign_cleanup):
    r1 = await admin.upload_firmware(_SMALL_FW)
    cid = r1.json()["id"]
    r = await admin.start_campaign(cid)  # omit rollout_window_days → default 10
    assert r.status_code == 200
    row = await db.fetchrow(
        "SELECT rollout_window_days FROM ota_campaigns WHERE id = $1", cid
    )
    assert row["rollout_window_days"] == 10


# ── T3-5: Pause / Resume ─────────────────────────────────────────────────────

async def test_t3_5_pause_sets_paused(admin, db, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid)

    r = await admin.pause_campaign(cid)
    assert r.status_code == 200
    row = await db.fetchrow("SELECT status FROM ota_campaigns WHERE id = $1", cid)
    assert row["status"] == "paused"


async def test_t3_5_resume_does_not_reset_rollout_start(admin, db, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid)

    row_started = await db.fetchrow(
        "SELECT rollout_start FROM ota_campaigns WHERE id = $1", cid
    )
    await admin.pause_campaign(cid)

    r = await admin.resume_campaign(cid)
    assert r.status_code == 200

    row_resumed = await db.fetchrow(
        "SELECT status, rollout_start FROM ota_campaigns WHERE id = $1", cid
    )
    assert row_resumed["status"] == "in_progress"
    assert row_resumed["rollout_start"] == row_started["rollout_start"]


# ── T3-6: Cancel ─────────────────────────────────────────────────────────────

async def test_t3_6_cancel_sets_terminal_status(admin, db, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid)

    r = await admin.cancel_campaign(cid)
    assert r.status_code == 200
    body = r.json()
    assert body["status"] == "cancelled"
    # success_rate is set (not NULL) after terminal transition
    assert body["success_rate"] is not None


async def test_t3_6_cancel_sets_success_rate_in_db(admin, db, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid)
    await admin.cancel_campaign(cid)

    row = await db.fetchrow("SELECT status, success_rate FROM ota_campaigns WHERE id = $1", cid)
    assert row["status"] == "cancelled"
    assert row["success_rate"] is not None  # 0.0 when no devices downloaded


async def test_t3_6_cancel_from_draft_allowed(admin, db, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    r = await admin.cancel_campaign(cid)
    assert r.status_code == 200
    row = await db.fetchrow("SELECT status FROM ota_campaigns WHERE id = $1", cid)
    assert row["status"] == "cancelled"


# ── T3-7: target_cohort_ids filter ───────────────────────────────────────────

async def test_t3_7_cohort_filter_excludes_other_devices(admin, dev, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    # Only 042001 and 042002 are eligible; test device is 999001
    await admin.start_campaign(
        cid, rollout_window_days=10, target_cohort_ids=["042001", "042002"]
    )
    meta = await dev.ota_poll()
    assert meta is None, "device outside cohort must see no update"


async def test_t3_7_null_cohort_includes_all_devices(admin, dev, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid, rollout_window_days=10)  # no cohort → whole fleet
    meta = await dev.ota_poll()
    # Device may be in a future slot (W > 0), but a metadata token must be present
    assert meta is not None, "device in whole-fleet campaign must receive metadata"


async def test_t3_7_empty_cohort_list_normalised_to_null(admin, db, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid, target_cohort_ids=[])  # empty list → NULL
    row = await db.fetchrow(
        "SELECT target_cohort_ids FROM ota_campaigns WHERE id = $1", cid
    )
    assert row["target_cohort_ids"] is None


# ── T3-7c: rollout_window_days immutability ───────────────────────────────────

async def test_t3_7c_start_twice_returns_conflict(admin, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid, rollout_window_days=10)

    r = await admin.start_campaign(cid, rollout_window_days=5)
    assert r.status_code == 409, "re-starting an in_progress campaign must return 409"


# ── T3-8: File integrity check on start ──────────────────────────────────────

async def test_t3_8_tampered_firmware_rejected_on_start(admin, firmware_dir, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    body = upload_r.json()
    cid = body["id"]
    version = body["version"]

    fw_file = firmware_dir / f"v{version}.bin"
    original = fw_file.read_bytes()
    fw_file.write_bytes(bytes(b ^ 0xFF for b in original))

    try:
        r = await admin.start_campaign(cid, rollout_window_days=10)
        assert r.status_code in (409, 422, 400), "tampered firmware must be rejected on start"
    finally:
        fw_file.write_bytes(original)


# ── T3-9: Campaign detail endpoint ───────────────────────────────────────────

async def test_t3_9_campaign_detail_returns_aggregates(admin, campaign_cleanup):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]
    await admin.start_campaign(cid, rollout_window_days=10)

    r = await admin.get_campaign(cid)
    assert r.status_code == 200
    body = r.json()

    assert body["id"] == cid
    assert body["status"] == "in_progress"
    assert "completed_device_count" in body
    assert "eligible_device_count" in body
    assert "current_slot" in body
    assert body["num_slots"] == 10 * 2


async def test_t3_9_campaign_detail_viewer_allowed(admin_client, admin, campaign_cleanup, db):
    upload_r = await admin.upload_firmware(_SMALL_FW)
    cid = upload_r.json()["id"]

    # Create viewer and get its token
    pw_hash = bcrypt.hashpw(b"viewerpass2", bcrypt.gensalt()).decode()
    await db.execute(
        """
        INSERT INTO admin_users (username, password_hash, role)
        VALUES ('_t3_viewer2', $1, 'viewer')
        ON CONFLICT (username) DO UPDATE SET password_hash = $1, role = 'viewer'
        """,
        pw_hash,
    )
    try:
        login_r = await admin_client.post(
            "/admin/login", data={"username": "_t3_viewer2", "password": "viewerpass2"}
        )
        viewer_token = login_r.json()["access_token"]
        r = await admin_client.get(
            f"/admin/campaign/{cid}",
            headers={"Authorization": f"Bearer {viewer_token}"},
        )
        assert r.status_code == 200
    finally:
        await db.execute("DELETE FROM admin_users WHERE username = '_t3_viewer2'")


async def test_t3_9_campaign_detail_404_for_unknown(admin, campaign_cleanup):
    r = await admin.get_campaign(999999999)
    assert r.status_code == 404
