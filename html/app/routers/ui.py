"""Admin HTML UI routes — Phase 8: HTMX + Jinja2 operator interface."""
from __future__ import annotations

import hashlib
import os
import tempfile
from datetime import date as _date_type, datetime, timezone
from math import ceil
from pathlib import Path
from typing import Optional

import asyncpg
from fastapi import APIRouter, Depends, File, Form, Request, UploadFile
from fastapi.responses import HTMLResponse, RedirectResponse

from app.auth.csrf import generate as gen_csrf
from app.auth.csrf import verify as check_csrf
from app.auth.jwt import _ROLE_LEVELS, check_password, create_token, hash_password, verify_token
from app.config import settings
from app.db.queries import (
    compute_campaign_success_rate,
    count_completed_devices,
    count_devices,
    count_eligible_devices,
    count_weather_records,
    create_admin_user,
    delete_admin_user,
    get_admin_user,
    get_admin_user_by_id,
    get_campaign,
    get_max_firmware_version,
    insert_campaign,
    list_admin_users,
    list_all_campaigns,
    list_devices,
    list_terminal_campaigns_ordered,
    list_weather_records,
    set_campaign_cancelled,
    set_campaign_in_progress,
    set_campaign_paused,
    set_campaign_resumed,
    update_admin_user_info,
    update_admin_user_password,
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
    token = create_token(sub=user["username"], role=user["role"], sub_id=user["id"])
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


# ── Root redirect ─────────────────────────────────────────────────────────────

@router.get("/", response_class=HTMLResponse)
async def admin_root(request: Request):
    token = request.cookies.get("access_token")
    if token:
        try:
            verify_token(token)
            return RedirectResponse("/admin/dashboard", status_code=302)
        except Exception:
            pass
    return RedirectResponse("/admin/login.html", status_code=302)


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


_SENSOR_PAGE_SIZE = 20


def _parse_date(s: Optional[str]):
    if not s:
        return None
    try:
        return _date_type.fromisoformat(s)
    except ValueError:
        return None


def _build_filter_qs(region_id, station_id, date_from, date_to, bus_min, bus_max) -> str:
    parts = []
    if region_id is not None:
        parts.append(f"region_id={region_id}")
    if station_id is not None:
        parts.append(f"station_id={station_id}")
    if date_from:
        parts.append(f"date_from={date_from}")
    if date_to:
        parts.append(f"date_to={date_to}")
    if bus_min is not None:
        parts.append(f"bus_min={bus_min}")
    if bus_max is not None:
        parts.append(f"bus_max={bus_max}")
    return "&".join(parts)


# ── Sensor data browse ────────────────────────────────────────────────────────

@router.get("/sensor-data", response_class=HTMLResponse)
async def sensor_data_page(
    request: Request,
    region_id: Optional[int] = None,
    station_id: Optional[int] = None,
    date_from: Optional[str] = None,
    date_to: Optional[str] = None,
    bus_min: Optional[float] = None,
    bus_max: Optional[float] = None,
):
    user, redir = _get_user(request)
    if redir:
        return redir
    initial_qs = _build_filter_qs(
        region_id, station_id,
        date_from, date_to,
        bus_min, bus_max,
    )
    return templates.TemplateResponse(
        request, "sensor_data.html",
        _ctx(
            request, user,
            region_id=region_id,
            station_id=station_id,
            date_from=date_from,
            date_to=date_to,
            bus_min=bus_min,
            bus_max=bus_max,
            initial_qs=initial_qs,
        ),
    )


@router.get("/sensor-data/table", response_class=HTMLResponse)
async def sensor_data_table(
    request: Request,
    region_id: Optional[int] = None,
    station_id: Optional[int] = None,
    date_from: Optional[str] = None,
    date_to: Optional[str] = None,
    bus_min: Optional[float] = None,
    bus_max: Optional[float] = None,
    page: int = 1,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request)
    if redir:
        return redir
    page = max(1, page)
    offset = (page - 1) * _SENSOR_PAGE_SIZE
    df = _parse_date(date_from)
    dt = _parse_date(date_to)
    records = await list_weather_records(
        conn,
        region_id=region_id, station_id=station_id,
        date_from=df, date_to=dt,
        bus_min=bus_min, bus_max=bus_max,
        limit=_SENSOR_PAGE_SIZE, offset=offset,
    )
    total = await count_weather_records(
        conn,
        region_id=region_id, station_id=station_id,
        date_from=df, date_to=dt,
        bus_min=bus_min, bus_max=bus_max,
    )
    total_pages = max(1, ceil(total / _SENSOR_PAGE_SIZE))
    filter_qs = _build_filter_qs(region_id, station_id, date_from, date_to, bus_min, bus_max)
    return templates.TemplateResponse(
        request, "partials/sensor_data_table.html",
        {
            "request": request,
            "records": records,
            "page": page,
            "total_pages": total_pages,
            "filter_qs": filter_qs,
        },
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


# ── UM3: User management HTML routes ─────────────────────────────────────────

async def _get_caller_id(conn: asyncpg.Connection, user: dict) -> Optional[int]:
    """Resolve caller DB id from token sub_id (fast) or username lookup (fallback)."""
    caller_id = user.get("sub_id")
    if caller_id is None:
        row = await get_admin_user(conn, user["sub"])
        caller_id = row["id"] if row else None
    return caller_id


def _user_table_ctx(request: Request, users: list, user: dict, caller_id: Optional[int]) -> dict:
    return {
        "request": request,
        "users": [dict(u) for u in users],
        "user": user,
        "caller_id": caller_id,
        "csrf_token": gen_csrf(),
    }


@router.get("/users.html", response_class=HTMLResponse)
async def users_page(request: Request, conn: asyncpg.Connection = Depends(get_db)):
    user, redir = _get_user(request, min_role="admin")
    if redir:
        return redir
    users = await list_admin_users(conn)
    caller_id = await _get_caller_id(conn, user)
    return templates.TemplateResponse(
        request, "users.html",
        _ctx(request, user, users=[dict(u) for u in users], caller_id=caller_id),
    )


@router.get("/profile.html", response_class=HTMLResponse)
async def profile_page(request: Request, conn: asyncpg.Connection = Depends(get_db)):
    user, redir = _get_user(request)
    if redir:
        return redir
    profile_id = await _get_caller_id(conn, user)
    profile = await get_admin_user_by_id(conn, profile_id) if profile_id else None
    if profile is None:
        return HTMLResponse("User not found.", status_code=404)
    return templates.TemplateResponse(
        request, "profile.html",
        _ctx(request, user, profile=dict(profile)),
    )


@router.post("/users/create-ui", response_class=HTMLResponse)
async def create_user_ui(
    request: Request,
    username: str = Form(...),
    password: str = Form(...),
    confirm_password: str = Form(...),
    role: str = Form(...),
    csrf_token: str = Form(default=""),
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="admin")
    if redir:
        return redir
    if not _csrf_ok(request, csrf_token):
        return HTMLResponse('<div class="alert alert-error">CSRF validation failed.</div>', status_code=403)
    if password != confirm_password:
        return HTMLResponse('<div class="alert alert-error">Passwords do not match.</div>')
    if len(password) < 8:
        return HTMLResponse('<div class="alert alert-error">Password must be at least 8 characters.</div>')
    if role not in ("viewer", "operator", "admin"):
        return HTMLResponse('<div class="alert alert-error">Invalid role.</div>')
    pw_hash = hash_password(password)
    new_id = await create_admin_user(conn, username, pw_hash, role)
    if new_id is None:
        return HTMLResponse('<div class="alert alert-error">Username already exists.</div>')
    if request.headers.get("HX-Request") == "true":
        caller_id = await _get_caller_id(conn, user)
        all_users = await list_admin_users(conn)
        return templates.TemplateResponse(
            request, "partials/user_table.html",
            _user_table_ctx(request, all_users, user, caller_id),
        )
    return RedirectResponse("/admin/users.html?created=1", status_code=303)


@router.post("/users/{user_id}/update-info-ui", response_class=HTMLResponse)
async def update_user_info_ui(
    request: Request,
    user_id: int,
    username: str = Form(default=""),
    role: str = Form(default=""),
    csrf_token: str = Form(default=""),
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request)
    if redir:
        return redir
    if not _csrf_ok(request, csrf_token):
        return HTMLResponse('<div class="alert alert-error">CSRF validation failed.</div>', status_code=403)

    caller_is_admin = user["role"] == "admin"
    caller_id = await _get_caller_id(conn, user)

    if not caller_is_admin and user_id != caller_id:
        return HTMLResponse('<div class="alert alert-error">Forbidden.</div>', status_code=403)

    role_provided = bool(role)
    if not caller_is_admin and role_provided:
        return HTMLResponse('<div class="alert alert-error">Cannot change own role.</div>', status_code=403)

    current = await get_admin_user_by_id(conn, user_id)
    if current is None:
        return HTMLResponse("User not found.", status_code=404)

    new_username = username if username else current["username"]
    new_role = role if role_provided else current["role"]

    if caller_is_admin and user_id == caller_id and new_role != "admin":
        return HTMLResponse('<div class="alert alert-error">Admin cannot demote themselves.</div>', status_code=403)

    try:
        result = await update_admin_user_info(conn, user_id, new_username, new_role)
    except asyncpg.UniqueViolationError:
        return HTMLResponse('<div class="alert alert-error">Username already exists.</div>')

    if result is None:
        return HTMLResponse("User not found.", status_code=404)

    if request.headers.get("HX-Request") == "true" and request.headers.get("HX-Target") == "user-table":
        all_users = await list_admin_users(conn)
        return templates.TemplateResponse(
            request, "partials/user_table.html",
            _user_table_ctx(request, all_users, user, caller_id),
        )
    if request.headers.get("HX-Request") == "true":
        resp = HTMLResponse('<div class="alert alert-success">Saved.</div>')
        resp.headers["HX-Redirect"] = "/admin/profile.html?updated=1"
        return resp
    dest = "/admin/users.html" if user_id != caller_id else "/admin/profile.html?updated=1"
    return RedirectResponse(dest, status_code=303)


@router.post("/users/{user_id}/password-ui", response_class=HTMLResponse)
async def change_password_ui(
    request: Request,
    user_id: int,
    current_password: str = Form(default=""),
    new_password: str = Form(...),
    confirm_password: str = Form(...),
    csrf_token: str = Form(default=""),
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request)
    if redir:
        return redir
    if not _csrf_ok(request, csrf_token):
        return HTMLResponse('<div class="alert alert-error">CSRF validation failed.</div>', status_code=403)

    caller_is_admin = user["role"] == "admin"
    caller_id = await _get_caller_id(conn, user)

    if not caller_is_admin and user_id != caller_id:
        return HTMLResponse('<div class="alert alert-error">Forbidden.</div>', status_code=403)
    if new_password != confirm_password:
        return HTMLResponse('<div class="alert alert-error">Passwords do not match.</div>')
    if len(new_password) < 8:
        return HTMLResponse('<div class="alert alert-error">Password must be at least 8 characters.</div>')

    if user_id == caller_id:
        if not current_password:
            return HTMLResponse('<div class="alert alert-error">Current password required.</div>')
        current = await get_admin_user_by_id(conn, user_id)
        if current is None:
            return HTMLResponse("User not found.", status_code=404)
        if not check_password(current_password, current["password_hash"]):
            return HTMLResponse('<div class="alert alert-error">Current password incorrect.</div>')

    pw_hash = hash_password(new_password)
    ok = await update_admin_user_password(conn, user_id, pw_hash)
    if not ok:
        return HTMLResponse("User not found.", status_code=404)
    resp = HTMLResponse('<div class="alert alert-success">Password changed successfully.</div>')
    resp.headers["HX-Trigger"] = "passwordChanged"
    return resp


# ── UM5: Modal fragment routes ────────────────────────────────────────────────

@router.get("/users/new-modal", response_class=HTMLResponse)
async def new_user_modal(request: Request):
    user, redir = _get_user(request, min_role="admin")
    if redir:
        return redir
    return templates.TemplateResponse(
        request, "partials/user_form_modal.html",
        {"request": request, "create": True, "csrf_token": gen_csrf()},
    )


@router.get("/users/{user_id}/edit-modal", response_class=HTMLResponse)
async def edit_user_modal(
    request: Request,
    user_id: int,
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="admin")
    if redir:
        return redir
    profile = await get_admin_user_by_id(conn, user_id)
    if profile is None:
        return HTMLResponse("User not found.", status_code=404)
    return templates.TemplateResponse(
        request, "partials/user_form_modal.html",
        {"request": request, "create": False, "profile": dict(profile), "csrf_token": gen_csrf()},
    )


@router.post("/users/{user_id}/delete-ui", response_class=HTMLResponse)
async def delete_user_ui(
    request: Request,
    user_id: int,
    csrf_token: str = Form(default=""),
    conn: asyncpg.Connection = Depends(get_db),
):
    user, redir = _get_user(request, min_role="admin")
    if redir:
        return redir
    if not _csrf_ok(request, csrf_token):
        return HTMLResponse('<div class="alert alert-error">CSRF validation failed.</div>', status_code=403)

    caller_id = await _get_caller_id(conn, user)
    if user_id == caller_id:
        return HTMLResponse('<div class="alert alert-error">Cannot delete yourself.</div>', status_code=403)

    ok = await delete_admin_user(conn, user_id)
    if not ok:
        return HTMLResponse("User not found.", status_code=404)
    return HTMLResponse("")  # HTMX swaps row target to empty, removing the row
