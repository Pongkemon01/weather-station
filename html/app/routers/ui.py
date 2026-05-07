"""Admin HTML UI routes — Phase 8: HTMX + Jinja2 operator interface."""
from __future__ import annotations

import hashlib
import os
import tempfile
from datetime import datetime, timezone
from math import ceil
from pathlib import Path
from typing import Optional

import asyncpg
from fastapi import APIRouter, Depends, File, Form, Request, UploadFile
from fastapi.responses import HTMLResponse, RedirectResponse

from app.auth.csrf import generate as gen_csrf
from app.auth.csrf import verify as check_csrf
from app.auth.jwt import _ROLE_LEVELS, check_password, create_token, verify_token
from app.config import settings
from app.db.queries import (
    compute_campaign_success_rate,
    count_completed_devices,
    count_devices,
    count_eligible_devices,
    get_admin_user,
    get_campaign,
    get_max_firmware_version,
    insert_campaign,
    list_all_campaigns,
    list_devices,
    list_terminal_campaigns_ordered,
    set_campaign_cancelled,
    set_campaign_in_progress,
    set_campaign_paused,
    set_campaign_resumed,
)
from app.deps import get_db
from app.templating import templates

router = APIRouter(prefix="/admin", tags=["admin-ui"])

_PAGE_SIZE = 20


# ── Auth helpers ──────────────────────────────────────────────────────────────

def _redirect_login(request: Request):
    """For HTMX requests send HX-Redirect; otherwise a normal 303 redirect."""
    if request.headers.get("HX-Request") == "true":
        return HTMLResponse("", status_code=200, headers={"HX-Redirect": "/admin/login.html"})
    return RedirectResponse("/admin/login.html", status_code=303)


def _get_user(request: Request, min_role: str = "viewer"):
    """Return (user_dict, None) or (None, redirect_response)."""
    token = request.cookies.get("access_token")
    if not token:
        return None, _redirect_login(request)
    try:
        user = verify_token(token)
    except Exception:
        return None, _redirect_login(request)
    if _ROLE_LEVELS.get(user.get("role", ""), -1) < _ROLE_LEVELS.get(min_role, 0):
        return None, HTMLResponse("<p>Insufficient privileges.</p>", status_code=403)
    return user, None


def _ctx(request: Request, user: dict, **kwargs) -> dict:
    return {"request": request, "user": user, "csrf_token": gen_csrf(), **kwargs}


def _csrf_ok(request: Request, form_token: str = "") -> bool:
    """Accept CSRF token from X-CSRF-Token header or from a form field."""
    return check_csrf(request.headers.get("X-CSRF-Token", "")) or check_csrf(form_token)


def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


async def _sweep_retention(conn: asyncpg.Connection) -> None:
    terminal = await list_terminal_campaigns_ordered(conn)
    for row in terminal[settings.firmware_keep_n:]:
        try:
            Path(row["firmware_file_path"]).unlink(missing_ok=True)
        except OSError:
            pass


# ── Login / logout ────────────────────────────────────────────────────────────

@router.get("/login.html", response_class=HTMLResponse)
async def login_page(request: Request):
    return templates.TemplateResponse(
        request, "login.html", {"csrf_token": gen_csrf()}
    )


@router.post("/login.html", response_class=HTMLResponse)
async def login_post(
    request: Request,
    username: str = Form(...),
    password: str = Form(...),
    csrf_token: str = Form(...),
    conn: asyncpg.Connection = Depends(get_db),
):
    if not check_csrf(csrf_token):
        return templates.TemplateResponse(
            request, "login.html",
            {"csrf_token": gen_csrf(), "error": "Invalid CSRF token — reload and retry."},
            status_code=400,
        )
    user = await get_admin_user(conn, username)
    if not user or not check_password(password, user["password_hash"]):
        return templates.TemplateResponse(
            request, "login.html",
            {"csrf_token": gen_csrf(), "error": "Invalid username or password."},
            status_code=401,
        )
    token = create_token(sub=user["username"], role=user["role"])
    resp = RedirectResponse("/admin/campaigns", status_code=303)
    resp.set_cookie("access_token", token, httponly=True, samesite="strict", max_age=86400)
    return resp


@router.post("/logout-ui", response_class=HTMLResponse)
async def logout(request: Request, csrf_token: str = Form(...)):
    if not check_csrf(csrf_token):
        return HTMLResponse("Bad request.", status_code=400)
    resp = RedirectResponse("/admin/login.html", status_code=303)
    resp.delete_cookie("access_token", httponly=True, samesite="strict")
    return resp


# ── Device dashboard ──────────────────────────────────────────────────────────

@router.get("/dashboard", response_class=HTMLResponse)
async def dashboard(request: Request):
    user, redir = _get_user(request)
    if redir:
        return redir
    return templates.TemplateResponse(request, "dashboard.html", _ctx(request, user))


@router.get("/devices/table", response_class=HTMLResponse)
async def devices_table(
    request: Request,
    page: int = 1,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request)
    if redir:
        return redir
    page = max(1, page)
    offset = (page - 1) * _PAGE_SIZE
    devices = await list_devices(conn, limit=_PAGE_SIZE, offset=offset)
    total = await count_devices(conn)
    total_pages = max(1, ceil(total / _PAGE_SIZE))
    return templates.TemplateResponse(
        request, "partials/device_table.html",
        {"request": request, "devices": devices, "page": page, "total_pages": total_pages},
    )


# ── Campaigns page ────────────────────────────────────────────────────────────

@router.get("/campaigns", response_class=HTMLResponse)
async def campaigns_page(request: Request, conn: asyncpg.Connection = Depends(get_db)):
    user, redir = _get_user(request)
    if redir:
        return redir
    next_ver = await get_max_firmware_version(conn) + 1
    return templates.TemplateResponse(
        request, "campaigns.html",
        _ctx(request, user, next_version=next_ver),
    )


@router.get("/campaigns/list", response_class=HTMLResponse)
async def campaigns_list(request: Request, conn: asyncpg.Connection = Depends(get_db)):
    user, redir = _get_user(request)
    if redir:
        return redir
    campaigns = await list_all_campaigns(conn)
    return templates.TemplateResponse(
        request, "partials/campaign_list.html",
        {"request": request, "campaigns": campaigns},
    )


# ── Firmware upload ───────────────────────────────────────────────────────────

@router.post("/firmware/upload-ui", response_class=HTMLResponse)
async def upload_firmware_ui(
    request: Request,
    file: UploadFile = File(...),
    csrf_token: str = Form(default=""),
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="admin")
    if redir:
        return redir
    if not _csrf_ok(request, csrf_token):
        return HTMLResponse('<div class="alert alert-error">CSRF validation failed.</div>', status_code=400)

    data = await file.read()
    if len(data) > settings.max_firmware_size_bytes:
        return templates.TemplateResponse(
            request, "partials/upload_result.html",
            {
                "request": request,
                "error": f"File too large ({len(data):,} B). Limit: {settings.max_firmware_size_bytes:,} B (480 KB).",
            },
        )

    firmware_dir = Path(settings.firmware_dir)
    sha256 = _sha256_hex(data)
    size = len(data)
    fd, tmp_path = tempfile.mkstemp(dir=firmware_dir, suffix=".tmp")
    dest: Optional[Path] = None
    campaign_id: Optional[int] = None
    new_version: Optional[int] = None
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
        os.replace(tmp_path, dest)
        tmp_path = None
    except Exception as exc:
        return templates.TemplateResponse(
            request, "partials/upload_result.html",
            {"request": request, "error": f"Upload failed: {exc}"},
        )
    finally:
        if tmp_path is not None:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    await _sweep_retention(conn)
    return templates.TemplateResponse(
        request, "partials/upload_result.html",
        {
            "request": request,
            "campaign_id": campaign_id,
            "version": new_version,
            "sha256": sha256,
            "size": size,
        },
    )


# ── Campaign lifecycle actions ────────────────────────────────────────────────

async def _campaigns_response(request: Request, conn: asyncpg.Connection, error: str = ""):
    """Return campaign list fragment, or a redirect hint when called from detail page."""
    # HTMX sends HX-Target with the target element id (no '#').
    # Detail-page action buttons target 'campaign-status-msg'; respond with redirect.
    if request.headers.get("HX-Target") == "campaign-status-msg":
        msg = f'<div class="alert alert-error">{error}</div>' if error else (
            '<div class="alert alert-success">Done. '
            '<a href="/admin/campaigns">Back to campaigns</a></div>'
        )
        return HTMLResponse(msg, headers={"HX-Refresh": "false"})
    campaigns = await list_all_campaigns(conn)
    ctx: dict = {"request": request, "campaigns": campaigns}
    if error:
        ctx["error"] = error
    return templates.TemplateResponse(request, "partials/campaign_list.html", ctx)


@router.post("/campaign/{campaign_id}/start-ui", response_class=HTMLResponse)
async def campaign_start_ui(
    request: Request,
    campaign_id: int,
    rollout_window_days: int = Form(default=10),
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="admin")
    if redir:
        return redir
    if not _csrf_ok(request):
        return await _campaigns_response(request, conn, "CSRF validation failed.")

    row = await get_campaign(conn, campaign_id)
    if row is None:
        return await _campaigns_response(request, conn, "Campaign not found.")
    if row["status"] != "draft":
        return await _campaigns_response(request, conn, f"Cannot start: campaign is '{row['status']}'.")

    fp = Path(row["firmware_file_path"])
    try:
        file_bytes = fp.read_bytes()
    except OSError:
        return await _campaigns_response(request, conn, "Firmware file missing from disk.")
    if len(file_bytes) != row["firmware_size"] or _sha256_hex(file_bytes) != row["firmware_sha256"]:
        return await _campaigns_response(request, conn, "Firmware integrity check failed.")

    await set_campaign_in_progress(
        conn, campaign_id,
        rollout_window_days=max(1, min(30, rollout_window_days)),
        slot_len_sec=settings.slot_len_sec,
        target_cohort_ids=None,
    )
    return await _campaigns_response(request, conn)


@router.post("/campaign/{campaign_id}/pause-ui", response_class=HTMLResponse)
async def campaign_pause_ui(
    request: Request,
    campaign_id: int,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="operator")
    if redir:
        return redir
    if not _csrf_ok(request):
        return await _campaigns_response(request, conn, "CSRF validation failed.")
    await set_campaign_paused(conn, campaign_id)
    return await _campaigns_response(request, conn)


@router.post("/campaign/{campaign_id}/resume-ui", response_class=HTMLResponse)
async def campaign_resume_ui(
    request: Request,
    campaign_id: int,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="operator")
    if redir:
        return redir
    if not _csrf_ok(request):
        return await _campaigns_response(request, conn, "CSRF validation failed.")
    await set_campaign_resumed(conn, campaign_id)
    return await _campaigns_response(request, conn)


@router.post("/campaign/{campaign_id}/cancel-ui", response_class=HTMLResponse)
async def campaign_cancel_ui(
    request: Request,
    campaign_id: int,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="operator")
    if redir:
        return redir
    if not _csrf_ok(request):
        return await _campaigns_response(request, conn, "CSRF validation failed.")
    row = await get_campaign(conn, campaign_id)
    if row and row["status"] in ("draft", "in_progress", "paused"):
        rate = await compute_campaign_success_rate(conn, campaign_id, row["firmware_size"])
        await set_campaign_cancelled(conn, campaign_id, rate)
        await _sweep_retention(conn)
    return await _campaigns_response(request, conn)


# ── Campaign detail ───────────────────────────────────────────────────────────

@router.get("/campaign/{campaign_id}/detail", response_class=HTMLResponse)
async def campaign_detail_page(
    request: Request,
    campaign_id: int,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request)
    if redir:
        return redir
    row = await get_campaign(conn, campaign_id)
    if row is None:
        return HTMLResponse("Campaign not found.", status_code=404)
    return templates.TemplateResponse(
        request, "campaign_detail.html",
        _ctx(request, user, campaign=dict(row), campaign_id=campaign_id),
    )


@router.get("/campaign/{campaign_id}/progress", response_class=HTMLResponse)
async def campaign_progress(
    request: Request,
    campaign_id: int,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request)
    if redir:
        return redir
    row = await get_campaign(conn, campaign_id)
    if row is None:
        return HTMLResponse("Not found.", status_code=404)

    completed = await count_completed_devices(conn, campaign_id, row["firmware_size"])
    eligible = await count_eligible_devices(conn, row["target_cohort_ids"])

    num_slots = (row["rollout_window_days"] or 0) * 2
    current_slot = 0
    if row["rollout_start"] and row["slot_len_sec"] and num_slots > 0:
        elapsed = (datetime.now(timezone.utc) - row["rollout_start"]).total_seconds()
        current_slot = min(num_slots - 1, max(0, int(elapsed // row["slot_len_sec"])))

    return templates.TemplateResponse(
        request, "partials/campaign_progress.html",
        {
            "request": request,
            "campaign": dict(row),
            "completed": completed,
            "eligible": eligible,
            "current_slot": current_slot,
            "num_slots": num_slots,
        },
    )
