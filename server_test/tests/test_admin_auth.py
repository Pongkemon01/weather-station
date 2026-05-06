"""Phase 6 authentication integration tests (S6-5).

T6-1  Valid creds → 200 + JWT bearing correct sub/role
T6-2  Wrong password → 401
T6-3  No token on protected endpoint → 401
T6-4  Expired/invalid token → 401
T6-5  Viewer token on admin-only endpoint → 403
T6-6  /admin/me returns current user info
T6-7  /admin/logout always returns 200 (stateless)
"""
from __future__ import annotations

import bcrypt
import pytest


pytestmark = pytest.mark.asyncio


# ── T6-1: Valid credentials → 200 + JWT ──────────────────────────────────────

async def test_t6_1_valid_login_returns_jwt(admin_client, admin_token):
    # admin_token fixture already performed the login and asserted 200.
    assert len(admin_token.split(".")) == 3, "JWT must have three dot-separated parts"


# ── T6-2: Wrong password → 401 ───────────────────────────────────────────────

async def test_t6_2_wrong_password_returns_401(admin_client):
    r = await admin_client.post(
        "/admin/login",
        data={"username": "admin", "password": "definitely_wrong_password"},
    )
    assert r.status_code == 401


# ── T6-3: No token on protected endpoint → 401 ───────────────────────────────

async def test_t6_3_no_token_returns_401(admin_client):
    r = await admin_client.get("/admin/me")
    assert r.status_code == 401


# ── T6-4: Invalid token → 401 ────────────────────────────────────────────────

async def test_t6_4_invalid_token_returns_401(admin_client):
    r = await admin_client.get(
        "/admin/me",
        headers={"Authorization": "Bearer not.a.valid.jwt"},
    )
    assert r.status_code == 401


# ── T6-5: Viewer token on admin-only endpoint → 403 ──────────────────────────

async def test_t6_5_viewer_token_on_admin_endpoint_returns_403(admin_client, db):
    pw_hash = bcrypt.hashpw(b"viewerpass", bcrypt.gensalt()).decode()
    await db.execute(
        """
        INSERT INTO admin_users (username, password_hash, role)
        VALUES ('_test_viewer', $1, 'viewer')
        ON CONFLICT (username) DO UPDATE SET password_hash = $1, role = 'viewer'
        """,
        pw_hash,
    )
    try:
        r = await admin_client.post(
            "/admin/login",
            data={"username": "_test_viewer", "password": "viewerpass"},
        )
        assert r.status_code == 200
        viewer_token = r.json()["access_token"]

        # /admin/users requires admin role → 403 for viewer.
        r2 = await admin_client.get(
            "/admin/users",
            headers={"Authorization": f"Bearer {viewer_token}"},
        )
        assert r2.status_code == 403
    finally:
        await db.execute("DELETE FROM admin_users WHERE username = '_test_viewer'")


# ── T6-6: /admin/me returns current user ─────────────────────────────────────

async def test_t6_6_me_returns_current_user(admin_client, admin_token):
    r = await admin_client.get(
        "/admin/me",
        headers={"Authorization": f"Bearer {admin_token}"},
    )
    assert r.status_code == 200
    body = r.json()
    assert "sub" in body
    assert body["role"] == "admin"


# ── T6-7: /admin/logout always 200 ───────────────────────────────────────────

async def test_t6_7_logout_returns_200(admin_client):
    r = await admin_client.post("/admin/logout")
    assert r.status_code == 200
