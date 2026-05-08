# User Management Verification Plan

> Black-box verifiers for the user management feature.
> Test file: `server_test/tests/test_user_management.py`
> Companion to `User_Management_Implementation_Plan.md`.
>
> Tests target the live/staging server at `https://robin-gpu.cpe.ku.ac.th/` (or `STAGING_URL` override).
> All tests clean up after themselves — no test users survive past teardown.

---

## Design Principles

- **No server-side imports.** Every assertion is against HTTP responses only.
- **Isolated test users.** All created users have username prefix `test_um_` and are deleted in teardown.
- **Role matrix coverage.** Every auth guard is exercised from all relevant roles: `admin`, `operator`, `viewer`, and unauthenticated.
- **CSRF coverage.** Every mutating form route is tested with and without a valid CSRF token.
- **Idempotent teardown.** Fixtures delete `test_um_*` users via API before and after each test class.
- **IoT data parity.** All roles must reach the same IoT pages (dashboard, campaigns, devices). Role differentiation is user management only — TUM0 enforces this invariant.

---

## Fixtures (`conftest.py` additions)

```python
@pytest_asyncio.fixture
async def admin_session(base_url, admin_credentials):
    """Returns an httpx.AsyncClient with admin JWT cookie set."""

@pytest_asyncio.fixture
async def operator_session(base_url, admin_session):
    """Creates a test operator user, returns its session client; deletes on teardown."""

@pytest_asyncio.fixture
async def viewer_session(base_url, admin_session):
    """Creates a test viewer user, returns its session client; deletes on teardown."""

@pytest_asyncio.fixture
async def test_user_id(admin_session):
    """Creates a test_um_target user (viewer); yields its id; deletes on teardown."""
```

All fixtures are async and use `httpx.AsyncClient` with the `access_token` cookie set by the login flow.

---

## Test Groups

### TUM0 — IoT Data Access Parity

Confirms that all roles see identical IoT content. User management is the only axis of role differentiation.

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM0-1 | Admin loads dashboard | admin | 200, contains sensor/campaign data |
| TUM0-2 | Operator loads dashboard | operator | 200, same page structure as admin |
| TUM0-3 | Viewer loads dashboard | viewer | 200, same page structure as admin |
| TUM0-4 | Admin loads campaigns page | admin | 200 |
| TUM0-5 | Operator loads campaigns page | operator | 200, identical content to admin view |
| TUM0-6 | Viewer loads campaigns page | viewer | 200, identical content to admin view |
| TUM0-7 | Unauthenticated loads dashboard | — | redirect to `/admin/login.html` |

```python
class TestIoTDataAccessParity:
    async def test_dashboard_admin(self, admin_session): ...
    async def test_dashboard_operator(self, operator_session): ...
    async def test_dashboard_viewer(self, viewer_session): ...
    async def test_campaigns_operator(self, operator_session): ...
    async def test_campaigns_viewer(self, viewer_session): ...
    async def test_unauthenticated_redirected(self, base_url): ...
```

---

### TUM1 — API: Create User (`POST /admin/users`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM1-1 | Valid payload (viewer role) | admin | 201, body has `id`, `username`, `role="viewer"`, `created_at` |
| TUM1-2 | Valid payload (admin role) | admin | 201, `role="admin"` |
| TUM1-3 | Duplicate username | admin | 409 |
| TUM1-4 | Password < 8 chars | admin | 422 |
| TUM1-5 | Invalid role string | admin | 422 |
| TUM1-6 | Username with illegal chars | admin | 422 |
| TUM1-7 | Missing `username` field | admin | 422 |
| TUM1-8 | Authenticated as operator | operator | 403 |
| TUM1-9 | Unauthenticated | — | 401 |

```python
class TestCreateUser:
    async def test_create_viewer(self, admin_session, cleanup_test_users): ...
    async def test_create_duplicate(self, admin_session, test_user_id): ...
    async def test_short_password(self, admin_session): ...
    async def test_invalid_role(self, admin_session): ...
    async def test_operator_forbidden(self, operator_session): ...
    async def test_unauthenticated(self, base_url): ...
```

---

### TUM2 — API: Update User Info (`PUT /admin/users/{id}`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM2-1 | Admin updates another user's username | admin | 200, new username in response |
| TUM2-2 | Admin updates another user's role | admin | 200, new role in response |
| TUM2-3 | Admin tries to change own role to operator | admin | 403 |
| TUM2-4 | Admin updates own username (role not sent) | admin | 200 |
| TUM2-5 | Operator updates own username | operator | 200 |
| TUM2-6 | Operator sends `role` field | operator | 403 |
| TUM2-7 | Operator updates another user | operator | 403 |
| TUM2-8 | Viewer updates another user | viewer | 403 |
| TUM2-9 | Duplicate username on update | admin | 409 |
| TUM2-10 | Non-existent user_id | admin | 404 |

```python
class TestUpdateUserInfo:
    async def test_admin_updates_other(self, admin_session, test_user_id): ...
    async def test_admin_role_self_demotion(self, admin_session, admin_user_id): ...
    async def test_admin_updates_own_username(self, admin_session, admin_user_id): ...
    async def test_operator_updates_self(self, operator_session, operator_user_id): ...
    async def test_operator_sends_role(self, operator_session, operator_user_id): ...
    async def test_operator_updates_other(self, operator_session, test_user_id): ...
    async def test_duplicate_username(self, admin_session, test_user_id): ...
    async def test_not_found(self, admin_session): ...
```

---

### TUM3 — API: Change Password (`PUT /admin/users/{id}/password`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM3-1 | Admin changes own password with correct `current_password` | admin | 204 |
| TUM3-2 | Admin changes own password with wrong `current_password` | admin | 422 |
| TUM3-3 | Admin changes own password, `current_password` omitted | admin | 422 |
| TUM3-4 | Admin changes other user's password, no `current_password` | admin | 204 |
| TUM3-5 | Admin changes other user's password with any `current_password` (accepted — no check) | admin | 204 |
| TUM3-6 | Operator changes own password, correct `current_password` | operator | 204 |
| TUM3-7 | Operator changes own password, wrong `current_password` | operator | 422 |
| TUM3-8 | Operator changes other user's password | operator | 403 |
| TUM3-9 | New password < 8 chars | admin | 422 |
| TUM3-10 | Verify new password works (login after change) | — | login succeeds |

```python
class TestChangePassword:
    async def test_admin_change_own_correct_current(self, admin_session): ...
    async def test_admin_change_own_wrong_current(self, admin_session): ...
    async def test_admin_change_other_no_current(self, admin_session, test_user_id): ...
    async def test_operator_change_own(self, operator_session, operator_user_id): ...
    async def test_operator_change_other(self, operator_session, test_user_id): ...
    async def test_new_password_login(self, admin_session, test_user_id, base_url): ...
```

---

### TUM4 — API: Delete User (`DELETE /admin/users/{id}`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM4-1 | Admin deletes a test user | admin | 204 |
| TUM4-2 | Admin deletes themselves | admin | 403 |
| TUM4-3 | Operator tries to delete any user | operator | 403 |
| TUM4-4 | Delete non-existent user_id | admin | 404 |
| TUM4-5 | Deleted user can no longer login | — | 401 on login |

```python
class TestDeleteUser:
    async def test_delete_other(self, admin_session, test_user_id): ...
    async def test_delete_self(self, admin_session, admin_user_id): ...
    async def test_operator_forbidden(self, operator_session, test_user_id): ...
    async def test_not_found(self, admin_session): ...
    async def test_deleted_cannot_login(self, admin_session, test_user_id, base_url): ...
```

---

### TUM5 — UI: Users Page (`GET /admin/users.html`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM5-1 | Admin loads users page | admin | 200, HTML contains `<table`, at least admin's own row |
| TUM5-2 | Operator loads users page | operator | redirect to login or 403 |
| TUM5-3 | Viewer loads users page | viewer | redirect to login or 403 |
| TUM5-4 | Unauthenticated | — | redirect to `/admin/login.html` |
| TUM5-5 | Admin's own row has no delete button | admin | `#user-row-{admin_id}` does not contain delete form |
| TUM5-6 | Other user's row has delete button | admin | row for test_user contains delete form |

```python
class TestUsersPage:
    async def test_admin_sees_table(self, admin_session, admin_user_id): ...
    async def test_operator_blocked(self, operator_session): ...
    async def test_unauthenticated_redirected(self, base_url): ...
    async def test_no_delete_on_own_row(self, admin_session, admin_user_id): ...
    async def test_delete_on_other_row(self, admin_session, test_user_id): ...
```

---

### TUM6 — UI: Profile Page (`GET /admin/profile.html`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM6-1 | Admin loads own profile | admin | 200, username shown, role dropdown present |
| TUM6-2 | Operator loads own profile | operator | 200, username shown, role field read-only (no dropdown) |
| TUM6-3 | Viewer loads own profile | viewer | 200 |
| TUM6-4 | Unauthenticated | — | redirect to login |
| TUM6-5 | Profile page links to `/admin/profile.html` from nav | admin | footer pill `<a>` href is `/admin/profile.html` |

```python
class TestProfilePage:
    async def test_admin_profile(self, admin_session): ...
    async def test_operator_profile(self, operator_session): ...
    async def test_viewer_profile(self, viewer_session): ...
    async def test_unauthenticated(self, base_url): ...
    async def test_nav_link_present(self, admin_session): ...
```

---

### TUM7 — UI Form: Create User (`POST /admin/users/create-ui`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM7-1 | Valid form submission | admin | redirect to `/admin/users.html?created=1` or HTMX 200 with updated table |
| TUM7-2 | Missing CSRF token | admin | 403 |
| TUM7-3 | Wrong CSRF token | admin | 403 |
| TUM7-4 | `password != confirm_password` | admin | 200 with inline error, no new user in DB |
| TUM7-5 | Password < 8 chars | admin | 200 with inline error |
| TUM7-6 | Duplicate username | admin | 200 with inline error |
| TUM7-7 | Operator submits form | operator | 403 |

```python
class TestCreateUserForm:
    async def test_valid_create(self, admin_session, admin_csrf_token): ...
    async def test_missing_csrf(self, admin_session): ...
    async def test_password_mismatch(self, admin_session, admin_csrf_token): ...
    async def test_short_password(self, admin_session, admin_csrf_token): ...
    async def test_duplicate_username(self, admin_session, admin_csrf_token, test_user_id): ...
    async def test_operator_blocked(self, operator_session, operator_csrf_token): ...
```

---

### TUM8 — UI Form: Update Info (`POST /admin/users/{id}/update-info-ui`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM8-1 | Admin updates another user's username | admin | 200, table or redirect |
| TUM8-2 | Admin updates another user's role | admin | 200, role updated |
| TUM8-3 | Admin sends self-demotion (role=viewer on own id) | admin | 403 or inline error |
| TUM8-4 | Operator updates own username | operator | 200 |
| TUM8-5 | Operator sends role field | operator | 403 or inline error |
| TUM8-6 | Missing CSRF | operator | 403 |

```python
class TestUpdateInfoForm:
    async def test_admin_updates_other(self, admin_session, admin_csrf_token, test_user_id): ...
    async def test_admin_self_demotion(self, admin_session, admin_csrf_token, admin_user_id): ...
    async def test_operator_updates_self(self, operator_session, operator_csrf_token, operator_user_id): ...
    async def test_operator_sends_role(self, operator_session, operator_csrf_token, operator_user_id): ...
    async def test_missing_csrf(self, operator_session, operator_user_id): ...
```

---

### TUM9 — UI Form: Change Password (`POST /admin/users/{id}/password-ui`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM9-1 | User changes own password, all fields correct | operator | 200 success indicator |
| TUM9-2 | `new_password != confirm_password` | operator | 200 inline error, password unchanged |
| TUM9-3 | Wrong `current_password` | operator | 200 inline error |
| TUM9-4 | Admin changes other's password, no `current_password` field sent | admin | 200 success |
| TUM9-5 | Missing CSRF | operator | 403 |
| TUM9-6 | Verify new password works via login | operator | login with new password succeeds |

```python
class TestChangePasswordForm:
    async def test_self_change_correct(self, operator_session, operator_csrf_token, operator_user_id): ...
    async def test_mismatch_confirm(self, operator_session, operator_csrf_token, operator_user_id): ...
    async def test_wrong_current(self, operator_session, operator_csrf_token, operator_user_id): ...
    async def test_admin_changes_other(self, admin_session, admin_csrf_token, test_user_id): ...
    async def test_new_password_login(self, operator_session, operator_csrf_token, operator_user_id, base_url): ...
```

---

### TUM10 — UI Form: Delete User (`POST /admin/users/{id}/delete-ui`)

| ID | Scenario | Actor | Expected |
|----|----------|-------|----------|
| TUM10-1 | Admin deletes test user via form | admin | 200 empty body (HTMX row removal) or redirect |
| TUM10-2 | Admin deletes self via form | admin | 403 or inline error |
| TUM10-3 | Missing CSRF | admin | 403 |
| TUM10-4 | Operator submits form | operator | 403 |
| TUM10-5 | Deleted user no longer appears in users page | admin | table refresh does not contain deleted username |

```python
class TestDeleteUserForm:
    async def test_delete_other(self, admin_session, admin_csrf_token, test_user_id): ...
    async def test_delete_self(self, admin_session, admin_csrf_token, admin_user_id): ...
    async def test_missing_csrf(self, admin_session, test_user_id): ...
    async def test_operator_blocked(self, operator_session, operator_csrf_token, test_user_id): ...
    async def test_deleted_not_in_table(self, admin_session, admin_csrf_token): ...
```

---

### TUM11 — Navigation Integration

| ID | Scenario | Expected |
|----|----------|----------|
| TUM11-1 | Admin dashboard page HTML contains "Users" nav link | `href="/admin/users.html"` present |
| TUM11-2 | Operator dashboard page HTML does NOT contain "Users" nav link | link absent |
| TUM11-3 | Any authenticated page HTML contains profile link in footer/pill | `href="/admin/profile.html"` present |

```python
class TestNavigation:
    async def test_admin_sees_users_link(self, admin_session): ...
    async def test_operator_no_users_link(self, operator_session): ...
    async def test_profile_link_present(self, operator_session): ...
```

---

## CSRF Token Fixture

All form tests that need a CSRF token use a shared helper:

```python
async def get_csrf_token(session: httpx.AsyncClient, page_url: str) -> str:
    """Load a page, extract csrf_token from <input name="csrf_token"> or meta tag."""
    r = await session.get(page_url)
    # parse with html.parser or regex — no external deps needed
    ...
```

---

## Teardown Strategy

All test classes that create users include:

```python
@pytest_asyncio.fixture(autouse=True)
async def cleanup(admin_session):
    yield
    # delete all test_um_* usernames
    users = (await admin_session.get("/admin/users")).json()
    for u in users:
        if u["username"].startswith("test_um_"):
            await admin_session.delete(f"/admin/users/{u['id']}")
```

---

## Running the Tests

```bash
# From project root
cd server_test
pytest tests/test_user_management.py -v

# Against staging URL
STAGING_URL=https://staging.example.com pytest tests/test_user_management.py -v

# Single group
pytest tests/test_user_management.py::TestCreateUser -v
```

Required env vars (in `server_test/.env`):
```
STAGING_URL=https://robin-gpu.cpe.ku.ac.th
ADMIN_USER=admin
ADMIN_PASS=<from iot.env>
```

---

## Coverage Summary

| Phase | Test Group | Planned | Implemented | Status |
|-------|-----------|---------|-------------|--------|
| IoT data parity (all roles) | TUM0 | 7 | 6 | ✓ pass |
| UM2 API create | TUM1 | 9 | 9 | ✓ pass |
| UM2 API update info | TUM2 | 10 | 10 | ✓ pass |
| UM2 API change password | TUM3 | 10 | 9 | ✓ pass |
| UM2 API delete | TUM4 | 5 | 5 | ✓ pass |
| UM3 UI users page | TUM5 | 6 | 5 | ✓ pass |
| UM3 UI profile page | TUM6 | 5 | 5 | ✓ pass |
| UM3 UI create form | TUM7 | 7 | 7 | ✓ pass |
| UM3 UI update info form | TUM8 | 6 | 5 | ✓ pass |
| UM3 UI change password form | TUM9 | 6 | 6 | ✓ pass |
| UM3 UI delete form | TUM10 | 5 | 5 | ✓ pass |
| UM4 Navigation | TUM11 | 3 | 3 | ✓ pass |
| **Total** | | **79** | **75** | **75/75 pass** |

> **Verified 2026-05-08** — `pytest tests/test_user_management.py` on robin-gpu.cpe.ku.ac.th: **75 passed in 81 s**.
> 4 plan-level scenarios consolidated during implementation (TUM0-4, TUM3-2/3, TUM5-2, TUM8-5 merged into related tests).
