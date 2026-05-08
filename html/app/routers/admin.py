"""Admin API routes — Phase 6: JWT authentication. Phase 7: OTA campaign management."""
from __future__ import annotations

import asyncio
import hashlib
import os
import re
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Literal, Optional

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import load_pem_private_key

import asyncpg
from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile, status
from pydantic import BaseModel, Field, field_validator

from app.auth.jwt import check_password, create_token, hash_password, require_role
from app.config import settings
from app.metrics import ota_campaign_success_rate
from app.db.queries import (
    compute_campaign_success_rate,
    count_completed_devices,
    count_eligible_devices,
    create_admin_user,
    delete_admin_user,
    get_admin_user,
    get_admin_user_by_id,
    get_campaign,
    get_max_firmware_version,
    insert_campaign,
    list_admin_users,
    list_terminal_campaigns_ordered,
    set_campaign_cancelled,
    set_campaign_in_progress,
    set_campaign_paused,
    set_campaign_resumed,
    update_admin_user_info,
    update_admin_user_password,
)
from app.deps import get_db

router = APIRouter(prefix="/admin", tags=["admin"])


# ── Phase 7 helpers ───────────────────────────────────────────────────────────

_DEVICE_ID_RE = re.compile(r"^\d{6}$")


class StartCampaignRequest(BaseModel):
    rollout_window_days: int = Field(default=10, ge=1, le=30)
    slot_len_sec: Optional[int] = Field(default=None, ge=1)
    target_cohort_ids: Optional[list[str]] = None

    @field_validator("target_cohort_ids")
    @classmethod
    def validate_cohort_ids(cls, v: Optional[list[str]]) -> Optional[list[str]]:
        if v is not None:
            for item in v:
                if not _DEVICE_ID_RE.match(item):
                    raise ValueError(f"cohort id {item!r} must be exactly 6 decimal digits")
        return v


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


def _sign_firmware(data: bytes, key_path: str) -> str | None:
    """Sign firmware with Ed25519. Returns hex signature, or None if no key configured."""
    if not key_path:
        return None
    pem = Path(key_path).read_bytes()
    key: Ed25519PrivateKey = load_pem_private_key(pem, password=None)  # type: ignore[assignment]
    return key.sign(data).hex()


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
        "access_token": create_token(sub=user["username"], role=user["role"], sub_id=user["id"]),
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
    rows = await list_admin_users(conn)
    return [dict(r) for r in rows]


# ── Phase UM2: user management API ───────────────────────────────────────────

_VALID_ROLES: frozenset[str] = frozenset({"viewer", "operator", "admin"})


class CreateUserRequest(BaseModel):
    username: str = Field(min_length=1, max_length=64, pattern=r"^[a-zA-Z0-9_.\-]+$")
    password: str = Field(min_length=8)
    role: Literal["viewer", "operator", "admin"]


class UpdateUserInfoRequest(BaseModel):
    username: str | None = Field(default=None, min_length=1, max_length=64)
    role: Literal["viewer", "operator", "admin"] | None = None


class ChangePasswordRequest(BaseModel):
    current_password: str | None = None
    new_password: str = Field(min_length=8)


@router.post("/users", status_code=201)
async def create_user(
    body: CreateUserRequest,
    _caller: dict = Depends(require_role("admin")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    pw_hash = hash_password(body.password)
    new_id = await create_admin_user(conn, body.username, pw_hash, body.role)
    if new_id is None:
        raise HTTPException(status_code=409, detail="Username already exists")
    row = await get_admin_user_by_id(conn, new_id)
    return {"id": row["id"], "username": row["username"], "role": row["role"], "created_at": row["created_at"]}


@router.put("/users/{user_id}")
async def update_user_info(
    user_id: int,
    body: UpdateUserInfoRequest,
    caller: dict = Depends(require_role("viewer")),
    conn: asyncpg.Connection = Depends(get_db),
) -> dict:
    caller_is_admin = caller["role"] == "admin"
    caller_id: int | None = caller.get("sub_id")
    # Fallback for tokens issued before sub_id was added (24 h TTL makes this transient)
    if caller_id is None:
        row = await get_admin_user(conn, caller["sub"])
        caller_id = row["id"] if row else None

    if not caller_is_admin and user_id != caller_id:
        raise HTTPException(status_code=403, detail="Forbidden")
    if not caller_is_admin and body.role is not None:
        raise HTTPException(status_code=403, detail="Cannot change own role")

    current = await get_admin_user_by_id(conn, user_id)
    if current is None:
        raise HTTPException(status_code=404, detail="User not found")

    new_username = body.username if body.username is not None else current["username"]
    new_role = body.role if body.role is not None else current["role"]

    if caller_is_admin and user_id == caller_id and new_role != "admin":
        raise HTTPException(status_code=403, detail="Admin cannot demote themselves")

    try:
        result = await update_admin_user_info(conn, user_id, new_username, new_role)
    except asyncpg.UniqueViolationError:
        raise HTTPException(status_code=409, detail="Username already exists")

    if result is None:
        raise HTTPException(status_code=404, detail="User not found")

    return {"id": user_id, "username": new_username, "role": new_role}


@router.put("/users/{user_id}/password", status_code=204)
async def change_password(
    user_id: int,
    body: ChangePasswordRequest,
    caller: dict = Depends(require_role("viewer")),
    conn: asyncpg.Connection = Depends(get_db),
) -> None:
    caller_is_admin = caller["role"] == "admin"
    caller_id: int | None = caller.get("sub_id")
    if caller_id is None:
        row = await get_admin_user(conn, caller["sub"])
        caller_id = row["id"] if row else None

    if not caller_is_admin and user_id != caller_id:
        raise HTTPException(status_code=403, detail="Forbidden")

    if user_id == caller_id:
        if body.current_password is None:
            raise HTTPException(status_code=422, detail="Current password required")
        current = await get_admin_user_by_id(conn, user_id)
        if current is None:
            raise HTTPException(status_code=404, detail="User not found")
        if not check_password(body.current_password, current["password_hash"]):
            raise HTTPException(status_code=422, detail="Current password incorrect")

    pw_hash = hash_password(body.new_password)
    ok = await update_admin_user_password(conn, user_id, pw_hash)
    if not ok:
        raise HTTPException(status_code=404, detail="User not found")


@router.delete("/users/{user_id}", status_code=204)
async def delete_user(
    user_id: int,
    caller: dict = Depends(require_role("admin")),
    conn: asyncpg.Connection = Depends(get_db),
) -> None:
    caller_id: int | None = caller.get("sub_id")
    if caller_id is None:
        row = await get_admin_user(conn, caller["sub"])
        caller_id = row["id"] if row else None

    if user_id == caller_id:
        raise HTTPException(status_code=403, detail="Cannot delete yourself")
    ok = await delete_admin_user(conn, user_id)
    if not ok:
        raise HTTPException(status_code=404, detail="User not found")


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

    # Optional Ed25519 signature (S10-4). Written only when SIGNING_PRIVATE_KEY_PATH is set.
    sig_hex = _sign_firmware(data, settings.signing_private_key_path)
    if sig_hex is not None:
        (firmware_dir / f"v{new_version}.sig").write_text(sig_hex)

    await _sweep_firmware_retention(conn, settings.firmware_keep_n)
    response: dict = {
        "id": campaign_id,
        "version": new_version,
        "firmware_sha256": sha256,
        "firmware_size": size,
    }
    if sig_hex is not None:
        response["firmware_ed25519_sig"] = sig_hex
    return response


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

    # Integrity check (S7-5): re-hash file off the event loop to avoid blocking.
    fp = Path(row["firmware_file_path"])
    if not fp.exists():
        raise HTTPException(status_code=409, detail="firmware file missing")

    def _check_integrity() -> str | None:
        data = fp.read_bytes()
        if len(data) != row["firmware_size"]:
            return "firmware file size mismatch"
        if _sha256_hex(data) != row["firmware_sha256"]:
            return "firmware SHA-256 mismatch"
        return None

    err = await asyncio.to_thread(_check_integrity)
    if err:
        raise HTTPException(status_code=409, detail=err)

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
