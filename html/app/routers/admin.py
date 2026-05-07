"""Admin API routes — Phase 6: JWT authentication. Phase 7: OTA campaign management."""
from __future__ import annotations

import hashlib
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import asyncpg
from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile, status
from pydantic import BaseModel, Field

from app.auth.jwt import check_password, create_token, require_role
from app.config import settings
from app.metrics import ota_campaign_success_rate
from app.db.queries import (
    compute_campaign_success_rate,
    count_completed_devices,
    count_eligible_devices,
    get_admin_user,
    get_campaign,
    get_max_firmware_version,
    insert_campaign,
    list_terminal_campaigns_ordered,
    set_campaign_cancelled,
    set_campaign_in_progress,
    set_campaign_paused,
    set_campaign_resumed,
)
from app.deps import get_db

router = APIRouter(prefix="/admin", tags=["admin"])


# ── Phase 7 helpers ───────────────────────────────────────────────────────────

class StartCampaignRequest(BaseModel):
    rollout_window_days: int = Field(default=10, ge=1, le=30)
    slot_len_sec: Optional[int] = None
    target_cohort_ids: Optional[list[str]] = None


async def _sweep_firmware_retention(conn: asyncpg.Connection, keep_n: int) -> None:
    """Delete .bin files for terminal campaigns beyond the keep_n window."""
    terminal = await list_terminal_campaigns_ordered(conn)
    for row in terminal[keep_n:]:
        try:
            Path(row["firmware_file_path"]).unlink(missing_ok=True)
        except OSError:
            pass


def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


async def _get_campaign_or_404(conn: asyncpg.Connection, campaign_id: int):
    row = await get_campaign(conn, campaign_id)
    if row is None:
        raise HTTPException(status_code=404, detail="campaign not found")
    return row


def _require_status(row, *allowed: str) -> None:
    if row["status"] not in allowed:
        raise HTTPException(
            status_code=409,
            detail=f"invalid transition from '{row['status']}'",
        )


def _current_slot(row) -> int:
    if not row["rollout_start"] or row["rollout_window_days"] <= 0:
        return 0
    num_slots = row["rollout_window_days"] * 2
    elapsed = (datetime.now(timezone.utc) - row["rollout_start"]).total_seconds()
    return min(num_slots - 1, max(0, int(elapsed // row["slot_len_sec"])))


# ── Phase 6: auth ─────────────────────────────────────────────────────────────

@router.post("/login")
async def login(
    username: str = Form(...),
    password: str = Form(...),
    conn: asyncpg.Connection = Depends(get_db),
):
    user = await get_admin_user(conn, username)
    if not user or not check_password(password, user["password_hash"]):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="Invalid credentials")
    return {
        "access_token": create_token(sub=user["username"], role=user["role"]),
        "token_type": "bearer",
    }


@router.post("/logout")
async def logout():
    # JWT is stateless; client must discard the token.
    return {"status": "ok"}


@router.get("/me")
async def me(user: dict = Depends(require_role("viewer"))):
    return {"sub": user["sub"], "role": user["role"]}


@router.get("/users")
async def list_users(
    _user: dict = Depends(require_role("admin")),
    conn: asyncpg.Connection = Depends(get_db),
):
    rows = await conn.fetch("SELECT id, username, role, created_at FROM admin_users ORDER BY id")
    return [dict(r) for r in rows]


# ── Phase 7: firmware upload ──────────────────────────────────────────────────

@router.post("/firmware/upload")
async def upload_firmware(
    file: UploadFile = File(...),
    _user: dict = Depends(require_role("admin")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    """Upload firmware binary, auto-assign version, insert draft campaign.

    Rejects with 413 if file exceeds MAX_FIRMWARE_SIZE_BYTES.
    Writes atomically via tmp + os.replace after DB commit.
    Runs firmware retention sweep on success.
    """
    data = await file.read()
    if len(data) > settings.max_firmware_size_bytes:
        raise HTTPException(status_code=413, detail="firmware exceeds maximum size")

    firmware_dir = Path(settings.firmware_dir)
    sha256 = _sha256_hex(data)
    size = len(data)

    # Write to temp file before the transaction so we don't hold a file handle
    # during the DB round-trip.  Rename to final path only after DB commit.
    fd, tmp_path = tempfile.mkstemp(dir=firmware_dir, suffix=".tmp")
    dest: Path | None = None
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)

        async with conn.transaction():
            new_version = await get_max_firmware_version(conn) + 1
            dest = firmware_dir / f"v{new_version}.bin"
            campaign_id = await insert_campaign(
                conn,
                version=new_version,
                firmware_sha256=sha256,
                firmware_size=size,
                firmware_file_path=str(dest),
                slot_len_sec=settings.slot_len_sec,
            )

        # Rename only after the DB transaction commits successfully.
        os.replace(tmp_path, dest)
        tmp_path = None  # mark moved so finally block skips it
    finally:
        if tmp_path is not None:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    await _sweep_firmware_retention(conn, settings.firmware_keep_n)
    return {
        "id": campaign_id,
        "version": new_version,
        "firmware_sha256": sha256,
        "firmware_size": size,
    }


# ── Phase 7: campaign lifecycle ───────────────────────────────────────────────

@router.post("/campaign/{campaign_id}/start")
async def campaign_start(
    campaign_id: int,
    body: StartCampaignRequest,
    _user: dict = Depends(require_role("admin")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    """Transition campaign draft → in_progress.

    Re-verifies firmware SHA-256 and size on disk before starting (S7-5).
    rollout_window_days and slot_len_sec are frozen at this point.
    """
    row = await _get_campaign_or_404(conn, campaign_id)
    _require_status(row, "draft")

    # Integrity check (S7-5): re-hash file to catch tampering or corruption.
    fp = Path(row["firmware_file_path"])
    if not fp.exists():
        raise HTTPException(status_code=409, detail="firmware file missing")
    file_bytes = fp.read_bytes()
    if len(file_bytes) != row["firmware_size"]:
        raise HTTPException(status_code=409, detail="firmware file size mismatch")
    if _sha256_hex(file_bytes) != row["firmware_sha256"]:
        raise HTTPException(status_code=409, detail="firmware SHA-256 mismatch")

    # Normalise empty cohort list → NULL so whole-fleet has one canonical form.
    cohort = body.target_cohort_ids if body.target_cohort_ids else None
    slot_len = body.slot_len_sec if body.slot_len_sec is not None else settings.slot_len_sec

    await set_campaign_in_progress(
        conn,
        campaign_id,
        rollout_window_days=body.rollout_window_days,
        slot_len_sec=slot_len,
        target_cohort_ids=cohort,
    )
    updated = await get_campaign(conn, campaign_id)
    return dict(updated)


@router.post("/campaign/{campaign_id}/pause")
async def campaign_pause(
    campaign_id: int,
    _user: dict = Depends(require_role("operator")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    row = await _get_campaign_or_404(conn, campaign_id)
    _require_status(row, "in_progress")
    await set_campaign_paused(conn, campaign_id)
    return {"id": campaign_id, "status": "paused"}


@router.post("/campaign/{campaign_id}/resume")
async def campaign_resume(
    campaign_id: int,
    _user: dict = Depends(require_role("operator")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    """Resume a paused campaign. rollout_start is NOT reset (S7-3)."""
    row = await _get_campaign_or_404(conn, campaign_id)
    _require_status(row, "paused")
    await set_campaign_resumed(conn, campaign_id)
    return {"id": campaign_id, "status": "in_progress"}


@router.post("/campaign/{campaign_id}/cancel")
async def campaign_cancel(
    campaign_id: int,
    _user: dict = Depends(require_role("operator")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    """Cancel a campaign (terminal). Computes success_rate and runs retention sweep."""
    row = await _get_campaign_or_404(conn, campaign_id)
    _require_status(row, "draft", "in_progress", "paused")

    rate = await compute_campaign_success_rate(conn, campaign_id, row["firmware_size"])
    await set_campaign_cancelled(conn, campaign_id, rate)
    await _sweep_firmware_retention(conn, settings.firmware_keep_n)
    ota_campaign_success_rate.labels(campaign_id=str(campaign_id)).set(rate)
    return {"id": campaign_id, "status": "cancelled", "success_rate": rate}


@router.get("/campaign/{campaign_id}")
async def campaign_detail(
    campaign_id: int,
    _user: dict = Depends(require_role("viewer")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    """Return campaign row plus derived aggregates for the admin dashboard."""
    row = await _get_campaign_or_404(conn, campaign_id)
    completed = await count_completed_devices(conn, campaign_id, row["firmware_size"])
    eligible = await count_eligible_devices(conn, row["target_cohort_ids"])
    num_slots = row["rollout_window_days"] * 2

    result = dict(row)
    result["completed_device_count"] = completed
    result["eligible_device_count"] = eligible
    result["current_slot"] = _current_slot(row)
    result["num_slots"] = num_slots
    return result
