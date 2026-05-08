"""User Management black-box tests.

TUM0 — IoT data access parity (all roles reach same pages).
TUM1 — POST /admin/users (create).
TUM2 — PUT  /admin/users/{id} (update info).
TUM3 — PUT  /admin/users/{id}/password (change password).
TUM4 — DELETE /admin/users/{id} (delete).

Requires in server_test/.env:
  INTERNAL_URL=http://127.0.0.1:8000  (via SSH tunnel)
  BASE_URL=https://robin-gpu.cpe.ku.ac.th
  ADMIN_USER=admin
  ADMIN_PASS=<password>
"""
from __future__ import annotations

import os
from typing import AsyncGenerator

import httpx
import pytest
import pytest_asyncio

from conftest import ADMIN_PASS, ADMIN_USER, INTERNAL_URL, _need

TEST_PW = "TestPass1!"


# ── Session fixtures ──────────────────────────────────────────────────────────

@pytest_asyncio.fixture
async def admin_session(admin_client, admin_token) -> httpx.AsyncClient:
    """admin_client with Bearer + cookie auth."""
    admin_client.headers["Authorization"] = f"Bearer {admin_token}"
    admin_client.cookies.set("access_token", admin_token)
    return admin_client


async def _make_user_session(
    admin_session: httpx.AsyncClient,
    internal_url: str,
    role: str,
) -> AsyncGenerator[tuple[httpx.AsyncClient, int], None]:
    """Create a test user, open its own session, yield (client, uid), then delete."""
    username = f"test_um_{role[:2]}_{os.urandom(3).hex()}"
    r = await admin_session.post(
        "/admin/users",
        json={"username": username, "password": TEST_PW, "role": role},
    )
    assert r.status_code == 201, f"fixture create {role}: {r.text}"
    uid = r.json()["id"]

    async with httpx.AsyncClient(base_url=internal_url) as client:
        lr = await client.post(
            "/admin/login", data={"username": username, "password": TEST_PW}
        )
        assert lr.status_code == 200, f"fixture login {role}: {lr.text}"
        token = lr.json()["access_token"]
        client.headers["Authorization"] = f"Bearer {token}"
        client.cookies.set("access_token", token)
        yield client, uid

    await admin_session.delete(f"/admin/users/{uid}")


@pytest_asyncio.fixture
async def operator_user(admin_session, internal_url):
    async for item in _make_user_session(admin_session, internal_url, "operator"):
        yield item


@pytest_asyncio.fixture
async def viewer_user(admin_session, internal_url):
    async for item in _make_user_session(admin_session, internal_url, "viewer"):
        yield item


@pytest_asyncio.fixture
async def target_user_id(admin_session):
    """Viewer test user for CRUD ops; teardown ignores 404 (test may have deleted it)."""
    username = f"test_um_tgt_{os.urandom(3).hex()}"
    r = await admin_session.post(
        "/admin/users",
        json={"username": username, "password": TEST_PW, "role": "viewer"},
    )
    assert r.status_code == 201, r.text
    uid = r.json()["id"]
    yield uid
    await admin_session.delete(f"/admin/users/{uid}")  # 404 is harmless


@pytest_asyncio.fixture
async def admin_user_id(admin_session) -> int:
    """Return the live admin user id from GET /admin/users."""
    r = await admin_session.get("/admin/users")
    assert r.status_code == 200
    return next(u["id"] for u in r.json() if u["username"] == ADMIN_USER)


# ── TUM0: IoT Data Access Parity ─────────────────────────────────────────────

class TestIoTDataAccessParity:
    async def test_dashboard_admin(self, admin_session):
        r = await admin_session.get("/admin/dashboard")
        assert r.status_code == 200

    async def test_dashboard_operator(self, operator_user):
        client, _ = operator_user
        r = await client.get("/admin/dashboard")
        assert r.status_code == 200

    async def test_dashboard_viewer(self, viewer_user):
        client, _ = viewer_user
        r = await client.get("/admin/dashboard")
        assert r.status_code == 200

    async def test_campaigns_operator(self, operator_user):
        client, _ = operator_user
        r = await client.get("/admin/campaigns")
        assert r.status_code == 200

    async def test_campaigns_viewer(self, viewer_user):
        client, _ = viewer_user
        r = await client.get("/admin/campaigns")
        assert r.status_code == 200

    async def test_unauthenticated_redirected(self, internal_url):
        async with httpx.AsyncClient(
            base_url=internal_url, follow_redirects=False
        ) as c:
            r = await c.get("/admin/dashboard")
        assert r.status_code in (302, 303)
        assert "login" in r.headers.get("location", "").lower()


# ── TUM1: API — Create User ───────────────────────────────────────────────────

class TestCreateUser:
    async def test_create_viewer(self, admin_session):
        uname = f"test_um_new_{os.urandom(3).hex()}"
        r = await admin_session.post(
            "/admin/users",
            json={"username": uname, "password": TEST_PW, "role": "viewer"},
        )
        assert r.status_code == 201
        body = r.json()
        assert body["username"] == uname and body["role"] == "viewer" and "id" in body
        await admin_session.delete(f"/admin/users/{body['id']}")

    async def test_create_admin_role(self, admin_session):
        uname = f"test_um_adm_{os.urandom(3).hex()}"
        r = await admin_session.post(
            "/admin/users",
            json={"username": uname, "password": TEST_PW, "role": "admin"},
        )
        assert r.status_code == 201 and r.json()["role"] == "admin"
        await admin_session.delete(f"/admin/users/{r.json()['id']}")

    async def test_create_duplicate(self, admin_session, target_user_id):
        users = (await admin_session.get("/admin/users")).json()
        existing_name = next(u["username"] for u in users if u["id"] == target_user_id)
        r = await admin_session.post(
            "/admin/users",
            json={"username": existing_name, "password": TEST_PW, "role": "viewer"},
        )
        assert r.status_code == 409

    async def test_short_password(self, admin_session):
        r = await admin_session.post(
            "/admin/users",
            json={"username": "test_um_short", "password": "ab12", "role": "viewer"},
        )
        assert r.status_code == 422

    async def test_invalid_role(self, admin_session):
        r = await admin_session.post(
            "/admin/users",
            json={"username": "test_um_badrole", "password": TEST_PW, "role": "superuser"},
        )
        assert r.status_code == 422

    async def test_username_illegal_chars(self, admin_session):
        r = await admin_session.post(
            "/admin/users",
            json={"username": "bad name!", "password": TEST_PW, "role": "viewer"},
        )
        assert r.status_code == 422

    async def test_missing_username(self, admin_session):
        r = await admin_session.post(
            "/admin/users", json={"password": TEST_PW, "role": "viewer"}
        )
        assert r.status_code == 422

    async def test_operator_forbidden(self, operator_user):
        client, _ = operator_user
        r = await client.post(
            "/admin/users",
            json={"username": "test_um_opfail", "password": TEST_PW, "role": "viewer"},
        )
        assert r.status_code == 403

    async def test_unauthenticated(self, internal_url):
        async with httpx.AsyncClient(base_url=internal_url) as c:
            r = await c.post(
                "/admin/users",
                json={"username": "test_um_anon", "password": TEST_PW, "role": "viewer"},
            )
        assert r.status_code == 401


# ── TUM2: API — Update User Info ─────────────────────────────────────────────

class TestUpdateUserInfo:
    async def test_admin_updates_other_username(self, admin_session, target_user_id):
        new_name = f"test_um_ren_{os.urandom(3).hex()}"
        r = await admin_session.put(
            f"/admin/users/{target_user_id}", json={"username": new_name}
        )
        assert r.status_code == 200 and r.json()["username"] == new_name

    async def test_admin_updates_other_role(self, admin_session, target_user_id):
        r = await admin_session.put(
            f"/admin/users/{target_user_id}", json={"role": "operator"}
        )
        assert r.status_code == 200 and r.json()["role"] == "operator"

    async def test_admin_self_demotion_blocked(self, admin_session, admin_user_id):
        r = await admin_session.put(
            f"/admin/users/{admin_user_id}", json={"role": "operator"}
        )
        assert r.status_code == 403

    async def test_admin_updates_own_username(self, admin_session, admin_user_id):
        # Reset to original name — no actual change
        r = await admin_session.put(
            f"/admin/users/{admin_user_id}", json={"username": ADMIN_USER}
        )
        assert r.status_code == 200

    async def test_operator_updates_self(self, operator_user):
        client, uid = operator_user
        new_name = f"test_um_opself_{os.urandom(3).hex()}"
        r = await client.put(f"/admin/users/{uid}", json={"username": new_name})
        assert r.status_code == 200

    async def test_operator_sends_role(self, operator_user):
        client, uid = operator_user
        r = await client.put(f"/admin/users/{uid}", json={"role": "admin"})
        assert r.status_code == 403

    async def test_operator_updates_other(self, operator_user, target_user_id):
        client, _ = operator_user
        r = await client.put(
            f"/admin/users/{target_user_id}", json={"username": "test_um_opother"}
        )
        assert r.status_code == 403

    async def test_viewer_updates_other(self, viewer_user, target_user_id):
        client, _ = viewer_user
        r = await client.put(
            f"/admin/users/{target_user_id}", json={"username": "test_um_vwother"}
        )
        assert r.status_code == 403

    async def test_duplicate_username(self, admin_session, target_user_id):
        r = await admin_session.put(
            f"/admin/users/{target_user_id}", json={"username": ADMIN_USER}
        )
        assert r.status_code == 409

    async def test_not_found(self, admin_session):
        r = await admin_session.put("/admin/users/999999", json={"username": "ghost"})
        assert r.status_code == 404


# ── TUM3: API — Change Password ───────────────────────────────────────────────

class TestChangePassword:
    async def test_admin_change_own_correct_current(self, admin_session, admin_user_id):
        r = await admin_session.put(
            f"/admin/users/{admin_user_id}/password",
            json={"current_password": ADMIN_PASS, "new_password": ADMIN_PASS},
        )
        assert r.status_code == 204

    async def test_admin_change_own_wrong_current(self, admin_session, admin_user_id):
        r = await admin_session.put(
            f"/admin/users/{admin_user_id}/password",
            json={"current_password": "WrongPass99!", "new_password": "NewPass123!"},
        )
        assert r.status_code == 422

    async def test_admin_change_own_missing_current(self, admin_session, admin_user_id):
        r = await admin_session.put(
            f"/admin/users/{admin_user_id}/password",
            json={"new_password": "NewPass123!"},
        )
        assert r.status_code == 422

    async def test_admin_change_other_no_current(self, admin_session, target_user_id):
        r = await admin_session.put(
            f"/admin/users/{target_user_id}/password",
            json={"new_password": "NewPass123!"},
        )
        assert r.status_code == 204

    async def test_operator_change_own(self, operator_user):
        client, uid = operator_user
        r = await client.put(
            f"/admin/users/{uid}/password",
            json={"current_password": TEST_PW, "new_password": "NewPass123!"},
        )
        assert r.status_code == 204

    async def test_operator_change_own_wrong_current(self, operator_user):
        client, uid = operator_user
        r = await client.put(
            f"/admin/users/{uid}/password",
            json={"current_password": "WrongPass99!", "new_password": "NewPass123!"},
        )
        assert r.status_code == 422

    async def test_operator_change_other(self, operator_user, target_user_id):
        client, _ = operator_user
        r = await client.put(
            f"/admin/users/{target_user_id}/password",
            json={"current_password": TEST_PW, "new_password": "NewPass123!"},
        )
        assert r.status_code == 403

    async def test_short_new_password(self, admin_session, target_user_id):
        r = await admin_session.put(
            f"/admin/users/{target_user_id}/password",
            json={"new_password": "ab12"},
        )
        assert r.status_code == 422

    async def test_new_password_works_for_login(
        self, admin_session, target_user_id, internal_url
    ):
        new_pw = "NewLogin123!"
        await admin_session.put(
            f"/admin/users/{target_user_id}/password", json={"new_password": new_pw}
        )
        users = (await admin_session.get("/admin/users")).json()
        username = next(u["username"] for u in users if u["id"] == target_user_id)
        async with httpx.AsyncClient(base_url=internal_url) as c:
            lr = await c.post("/admin/login", data={"username": username, "password": new_pw})
        assert lr.status_code == 200


# ── TUM4: API — Delete User ───────────────────────────────────────────────────

class TestDeleteUser:
    async def test_delete_other(self, admin_session, target_user_id):
        r = await admin_session.delete(f"/admin/users/{target_user_id}")
        assert r.status_code == 204

    async def test_delete_self(self, admin_session, admin_user_id):
        r = await admin_session.delete(f"/admin/users/{admin_user_id}")
        assert r.status_code == 403

    async def test_operator_forbidden(self, operator_user, target_user_id):
        client, _ = operator_user
        r = await client.delete(f"/admin/users/{target_user_id}")
        assert r.status_code == 403

    async def test_not_found(self, admin_session):
        r = await admin_session.delete("/admin/users/999999")
        assert r.status_code == 404

    async def test_deleted_cannot_login(
        self, admin_session, target_user_id, internal_url
    ):
        users = (await admin_session.get("/admin/users")).json()
        username = next(u["username"] for u in users if u["id"] == target_user_id)
        await admin_session.delete(f"/admin/users/{target_user_id}")
        async with httpx.AsyncClient(base_url=internal_url) as c:
            lr = await c.post(
                "/admin/login", data={"username": username, "password": TEST_PW}
            )
        assert lr.status_code == 401


# ── Helpers ───────────────────────────────────────────────────────────────────

import re as _re


async def _csrf(session: httpx.AsyncClient, page: str = "/admin/dashboard") -> str:
    """Load a page and extract the CSRF token."""
    r = await session.get(page)
    # hx-headers body attribute
    m = _re.search(r'"X-CSRF-Token":\s*"([^"]+)"', r.text)
    if m:
        return m.group(1)
    # hidden input fallback
    m = _re.search(r'name="csrf_token"[^>]*value="([^"]+)"', r.text)
    if m:
        return m.group(1)
    m = _re.search(r'value="([^"]+)"[^>]*name="csrf_token"', r.text)
    return m.group(1) if m else ""


# ── TUM5: UI — Users Page ─────────────────────────────────────────────────────

class TestUsersPage:
    async def test_admin_sees_table(self, admin_session, admin_user_id):
        r = await admin_session.get("/admin/users.html")
        assert r.status_code == 200
        assert "<table" in r.text
        assert f'id="user-row-{admin_user_id}"' in r.text

    async def test_operator_blocked(self, operator_user):
        client, _ = operator_user
        r = await client.get("/admin/users.html")
        assert r.status_code in (200, 302, 303, 403)
        if r.status_code == 200:
            assert "login" in r.headers.get("HX-Redirect", "").lower() or "Insufficient" in r.text

    async def test_unauthenticated_redirected(self, internal_url):
        async with httpx.AsyncClient(base_url=internal_url, follow_redirects=False) as c:
            r = await c.get("/admin/users.html")
        assert r.status_code in (302, 303)
        assert "login" in r.headers.get("location", "").lower()

    async def test_no_delete_on_own_row(self, admin_session, admin_user_id):
        r = await admin_session.get("/admin/users.html")
        assert r.status_code == 200
        assert f'/admin/users/{admin_user_id}/delete-ui' not in r.text

    async def test_delete_on_other_row(self, admin_session, target_user_id):
        r = await admin_session.get("/admin/users.html")
        assert r.status_code == 200
        assert f'/admin/users/{target_user_id}/delete-ui' in r.text


# ── TUM6: UI — Profile Page ───────────────────────────────────────────────────

class TestProfilePage:
    async def test_admin_profile(self, admin_session):
        r = await admin_session.get("/admin/profile.html")
        assert r.status_code == 200
        assert 'name="role"' in r.text  # role dropdown present

    async def test_operator_profile(self, operator_user):
        client, _ = operator_user
        r = await client.get("/admin/profile.html")
        assert r.status_code == 200
        assert '<select name="role"' not in r.text  # no role dropdown

    async def test_viewer_profile(self, viewer_user):
        client, _ = viewer_user
        r = await client.get("/admin/profile.html")
        assert r.status_code == 200

    async def test_unauthenticated(self, internal_url):
        async with httpx.AsyncClient(base_url=internal_url, follow_redirects=False) as c:
            r = await c.get("/admin/profile.html")
        assert r.status_code in (302, 303)
        assert "login" in r.headers.get("location", "").lower()

    async def test_profile_link_in_nav(self, admin_session):
        r = await admin_session.get("/admin/dashboard")
        assert r.status_code == 200
        assert 'href="/admin/profile.html"' in r.text


# ── TUM7: UI Form — Create User ───────────────────────────────────────────────

class TestCreateUserForm:
    async def test_valid_create(self, admin_session):
        token = await _csrf(admin_session, "/admin/dashboard")
        uname = f"test_um_ui_{_re.sub(r'[^a-z0-9]', '', __import__('os').urandom(3).hex())}"
        r = await admin_session.post(
            "/admin/users/create-ui",
            data={"username": uname, "password": "TestPass1!", "confirm_password": "TestPass1!", "role": "viewer", "csrf_token": token},
            follow_redirects=True,
        )
        assert r.status_code == 200
        # Teardown
        users = (await admin_session.get("/admin/users")).json()
        uid = next((u["id"] for u in users if u["username"] == uname), None)
        if uid:
            await admin_session.delete(f"/admin/users/{uid}")

    async def test_missing_csrf(self, admin_session):
        r = await admin_session.post(
            "/admin/users/create-ui",
            data={"username": "test_um_nocsrf", "password": "TestPass1!", "confirm_password": "TestPass1!", "role": "viewer"},
        )
        assert r.status_code == 403

    async def test_wrong_csrf(self, admin_session):
        r = await admin_session.post(
            "/admin/users/create-ui",
            data={"username": "test_um_badcsrf", "password": "TestPass1!", "confirm_password": "TestPass1!", "role": "viewer", "csrf_token": "invalid"},
        )
        assert r.status_code == 403

    async def test_password_mismatch(self, admin_session):
        token = await _csrf(admin_session)
        r = await admin_session.post(
            "/admin/users/create-ui",
            data={"username": "test_um_mismatch", "password": "TestPass1!", "confirm_password": "DifferentPass!", "role": "viewer", "csrf_token": token},
        )
        assert r.status_code == 200
        assert "do not match" in r.text.lower() or "mismatch" in r.text.lower() or "alert" in r.text

    async def test_short_password(self, admin_session):
        token = await _csrf(admin_session)
        r = await admin_session.post(
            "/admin/users/create-ui",
            data={"username": "test_um_short", "password": "abc", "confirm_password": "abc", "role": "viewer", "csrf_token": token},
        )
        assert r.status_code == 200
        assert "alert" in r.text

    async def test_duplicate_username(self, admin_session, target_user_id):
        token = await _csrf(admin_session)
        users = (await admin_session.get("/admin/users")).json()
        existing = next(u["username"] for u in users if u["id"] == target_user_id)
        r = await admin_session.post(
            "/admin/users/create-ui",
            data={"username": existing, "password": "TestPass1!", "confirm_password": "TestPass1!", "role": "viewer", "csrf_token": token},
        )
        assert r.status_code == 200
        assert "already exists" in r.text.lower() or "alert" in r.text

    async def test_operator_blocked(self, operator_user):
        client, _ = operator_user
        token = await _csrf(client, "/admin/profile.html")
        r = await client.post(
            "/admin/users/create-ui",
            data={"username": "test_um_opblk", "password": "TestPass1!", "confirm_password": "TestPass1!", "role": "viewer", "csrf_token": token},
        )
        assert r.status_code == 403


# ── TUM8: UI Form — Update Info ───────────────────────────────────────────────

class TestUpdateInfoForm:
    async def test_admin_updates_other(self, admin_session, target_user_id):
        token = await _csrf(admin_session)
        new_name = f"test_um_upd_{__import__('os').urandom(3).hex()}"
        r = await admin_session.post(
            f"/admin/users/{target_user_id}/update-info-ui",
            data={"username": new_name, "role": "viewer", "csrf_token": token},
            follow_redirects=True,
        )
        assert r.status_code == 200

    async def test_admin_self_demotion(self, admin_session, admin_user_id):
        token = await _csrf(admin_session)
        r = await admin_session.post(
            f"/admin/users/{admin_user_id}/update-info-ui",
            data={"username": ADMIN_USER, "role": "operator", "csrf_token": token},
        )
        assert r.status_code == 403

    async def test_operator_updates_self(self, operator_user):
        client, uid = operator_user
        token = await _csrf(client, "/admin/profile.html")
        new_name = f"test_um_opself_{__import__('os').urandom(3).hex()}"
        r = await client.post(
            f"/admin/users/{uid}/update-info-ui",
            data={"username": new_name, "csrf_token": token},
            follow_redirects=True,
        )
        assert r.status_code == 200

    async def test_operator_sends_role(self, operator_user):
        client, uid = operator_user
        token = await _csrf(client, "/admin/profile.html")
        r = await client.post(
            f"/admin/users/{uid}/update-info-ui",
            data={"username": ADMIN_USER, "role": "admin", "csrf_token": token},
        )
        assert r.status_code == 403

    async def test_missing_csrf(self, operator_user):
        client, uid = operator_user
        r = await client.post(
            f"/admin/users/{uid}/update-info-ui",
            data={"username": "test_um_nocsrf"},
        )
        assert r.status_code == 403


# ── TUM9: UI Form — Change Password ──────────────────────────────────────────

class TestChangePasswordForm:
    async def test_self_change_correct(self, operator_user):
        client, uid = operator_user
        token = await _csrf(client, "/admin/profile.html")
        r = await client.post(
            f"/admin/users/{uid}/password-ui",
            data={"current_password": TEST_PW, "new_password": "NewPass123!", "confirm_password": "NewPass123!", "csrf_token": token},
        )
        assert r.status_code == 200
        assert "success" in r.text.lower() or "alert-success" in r.text

    async def test_mismatch_confirm(self, operator_user):
        client, uid = operator_user
        token = await _csrf(client, "/admin/profile.html")
        r = await client.post(
            f"/admin/users/{uid}/password-ui",
            data={"current_password": TEST_PW, "new_password": "NewPass123!", "confirm_password": "Different123!", "csrf_token": token},
        )
        assert r.status_code == 200
        assert "alert" in r.text

    async def test_wrong_current(self, operator_user):
        client, uid = operator_user
        token = await _csrf(client, "/admin/profile.html")
        r = await client.post(
            f"/admin/users/{uid}/password-ui",
            data={"current_password": "WrongPass99!", "new_password": "NewPass123!", "confirm_password": "NewPass123!", "csrf_token": token},
        )
        assert r.status_code == 200
        assert "incorrect" in r.text.lower() or "alert" in r.text

    async def test_admin_changes_other(self, admin_session, target_user_id):
        token = await _csrf(admin_session)
        r = await admin_session.post(
            f"/admin/users/{target_user_id}/password-ui",
            data={"new_password": "AdminSet123!", "confirm_password": "AdminSet123!", "csrf_token": token},
        )
        assert r.status_code == 200
        assert "success" in r.text.lower() or "alert-success" in r.text

    async def test_missing_csrf(self, operator_user):
        client, uid = operator_user
        r = await client.post(
            f"/admin/users/{uid}/password-ui",
            data={"current_password": TEST_PW, "new_password": "NewPass123!", "confirm_password": "NewPass123!"},
        )
        assert r.status_code == 403

    async def test_new_password_login(self, admin_session, operator_user, internal_url):
        client, uid = operator_user
        token = await _csrf(client, "/admin/profile.html")
        new_pw = "UIChanged123!"
        r = await client.post(
            f"/admin/users/{uid}/password-ui",
            data={"current_password": TEST_PW, "new_password": new_pw, "confirm_password": new_pw, "csrf_token": token},
        )
        assert r.status_code == 200
        users = (await admin_session.get("/admin/users")).json()
        uname = next((u["username"] for u in users if u["id"] == uid), None)
        assert uname is not None
        async with httpx.AsyncClient(base_url=internal_url) as c:
            lr = await c.post("/admin/login", data={"username": uname, "password": new_pw})
        assert lr.status_code == 200


# ── TUM10: UI Form — Delete User ─────────────────────────────────────────────

class TestDeleteUserForm:
    async def test_delete_other(self, admin_session, target_user_id):
        token = await _csrf(admin_session)
        r = await admin_session.post(
            f"/admin/users/{target_user_id}/delete-ui",
            data={"csrf_token": token},
        )
        assert r.status_code == 200
        assert r.text.strip() == ""

    async def test_delete_self(self, admin_session, admin_user_id):
        token = await _csrf(admin_session)
        r = await admin_session.post(
            f"/admin/users/{admin_user_id}/delete-ui",
            data={"csrf_token": token},
        )
        assert r.status_code == 403

    async def test_missing_csrf(self, admin_session, target_user_id):
        r = await admin_session.post(
            f"/admin/users/{target_user_id}/delete-ui",
            data={},
        )
        assert r.status_code == 403

    async def test_operator_blocked(self, operator_user, target_user_id):
        client, _ = operator_user
        token = await _csrf(client, "/admin/profile.html")
        r = await client.post(
            f"/admin/users/{target_user_id}/delete-ui",
            data={"csrf_token": token},
        )
        assert r.status_code == 403

    async def test_deleted_not_in_table(self, admin_session):
        import os
        uname = f"test_um_del_{os.urandom(3).hex()}"
        cr = await admin_session.post(
            "/admin/users",
            json={"username": uname, "password": TEST_PW, "role": "viewer"},
        )
        assert cr.status_code == 201
        uid = cr.json()["id"]
        token = await _csrf(admin_session)
        await admin_session.post(f"/admin/users/{uid}/delete-ui", data={"csrf_token": token})
        r = await admin_session.get("/admin/users.html")
        assert uname not in r.text


# ── TUM11: Navigation Integration ────────────────────────────────────────────

class TestNavigation:
    async def test_admin_sees_users_link(self, admin_session):
        r = await admin_session.get("/admin/dashboard")
        assert r.status_code == 200
        assert 'href="/admin/users.html"' in r.text

    async def test_operator_no_users_link(self, operator_user):
        client, _ = operator_user
        r = await client.get("/admin/dashboard")
        assert r.status_code == 200
        assert 'href="/admin/users.html"' not in r.text

    async def test_profile_link_present(self, operator_user):
        client, _ = operator_user
        r = await client.get("/admin/dashboard")
        assert r.status_code == 200
        assert 'href="/admin/profile.html"' in r.text
