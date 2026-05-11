# User Management Verification Plan

> Black-box verifiers for the user management feature.
> Test file: `server_test/tests/test_user_management.py`. Implementation: `User_Management_Implementation_Plan.md`.
> Target: `https://robin-gpu.cpe.ku.ac.th/` (or `STAGING_URL` override).
> **Status: 75/75 pass in 81 s** on `robin-gpu.cpe.ku.ac.th` (Python 3.13, pytest 9.0.3, 2026-05-08).

---

## Design

- No server imports. HTTP contract only.
- Isolated test users — username prefix `test_um_`; teardown deletes all `test_um_*`.
- Role matrix coverage from `admin`, `operator`, `viewer`, and unauthenticated.
- CSRF coverage — every mutating form route tested with + without valid token.
- **TUM0 invariant:** all three roles reach every IoT page (dashboard, campaigns, devices, sensor data). Role differentiation is user-management only.

---

## Fixtures (`conftest.py` additions)

```python
@pytest_asyncio.fixture
async def admin_session(base_url, admin_credentials):
    """httpx.AsyncClient with admin JWT cookie."""

@pytest_asyncio.fixture
async def operator_session(base_url, admin_session):
    """Creates test_um_operator; yields session; deletes on teardown."""

@pytest_asyncio.fixture
async def viewer_session(base_url, admin_session):
    """Creates test_um_viewer; yields session; deletes on teardown."""

@pytest_asyncio.fixture
async def test_user_id(admin_session):
    """Creates test_um_target (viewer); yields id; deletes on teardown."""

@pytest_asyncio.fixture(autouse=True)
async def cleanup_um_users(admin_session):
    yield
    users = (await admin_session.get("/admin/users")).json()
    for u in users:
        if u["username"].startswith("test_um_"):
            await admin_session.delete(f"/admin/users/{u['id']}")
```

All sessions are `httpx.AsyncClient` with `access_token` cookie set via login.

---

## Coverage Summary (75/75 pass)

| Group | Scope | Planned | Implemented | Status |
|-------|-------|---------|-------------|--------|
| **TUM0** IoT data parity (all roles) | dashboard/campaigns/sensor reach + unauth redirect | 7  | 6  | ✓ pass |
| **TUM1** API create (`POST /admin/users`)           | valid (viewer/admin/op); duplicate 409; short pw 422; invalid role 422; illegal chars 422; missing field 422; op forbidden 403; unauth 401 | 9  | 9  | ✓ pass |
| **TUM2** API update info (`PUT /admin/users/{id}`)  | admin updates other; admin changes own username (no role sent); admin self-demote 403; operator self; operator sends role 403; operator on other 403; viewer on other 403; duplicate 409; not-found 404 | 10 | 10 | ✓ pass |
| **TUM3** API change password (`PUT /admin/users/{id}/password`) | admin own correct/wrong/missing current; admin on other no current; admin on other any current; operator own correct/wrong; operator on other 403; new pw <8 422; deleted-user post-login | 10 | 9  | ✓ pass |
| **TUM4** API delete (`DELETE /admin/users/{id}`)    | admin deletes other 204; admin self 403; operator any 403; not-found 404; deleted cannot login 401 | 5  | 5  | ✓ pass |
| **TUM5** UI users page (`GET /admin/users.html`)    | admin 200 + table; operator blocked; viewer blocked; unauth → login; admin's own row has no delete; other rows have delete | 6  | 5  | ✓ pass |
| **TUM6** UI profile page (`GET /admin/profile.html`)| admin (role dropdown present); operator (no dropdown); viewer; unauth; profile link in nav | 5  | 5  | ✓ pass |
| **TUM7** UI form create (`POST /admin/users/create-ui`) | valid; missing CSRF 403; wrong CSRF 403; pw mismatch inline (no DB write); short pw inline; duplicate inline; operator 403 | 7  | 7  | ✓ pass |
| **TUM8** UI form update info                         | admin updates other; admin self-demote inline/403; operator updates self; operator sends role 403; missing CSRF 403 | 6  | 5  | ✓ pass |
| **TUM9** UI form change password                     | self correct; mismatch confirm inline; wrong current inline; admin on other (no current); missing CSRF 403; new pw login | 6  | 6  | ✓ pass |
| **TUM10** UI form delete                             | admin deletes other (HTMX row remove); admin self 403; missing CSRF 403; operator 403; deleted row absent from table refresh | 5  | 5  | ✓ pass |
| **TUM11** Navigation                                 | admin sees Users link; operator does not; profile link present | 3  | 3  | ✓ pass |
| **Total** | | **79** | **75** | **75/75 pass** |

> 4 plan-level scenarios consolidated during implementation (TUM0-4, TUM3-2/3, TUM5-2, TUM8-5 merged into related tests).

---

## Helpers

CSRF extraction:

```python
async def get_csrf_token(session: httpx.AsyncClient, page_url: str) -> str:
    """Loads page; extracts csrf_token from <input> or meta tag (regex; no html lib)."""
```

Teardown (autouse fixture): list all users via `/admin/users`, delete those with `username.startswith("test_um_")`.

---

## Execution

```bash
# From project root
cd server_test
pytest tests/test_user_management.py -v

# Staging override
STAGING_URL=https://staging.example.com pytest tests/test_user_management.py -v

# Single group
pytest tests/test_user_management.py::TestCreateUser -v
```

Required env vars (`server_test/.env`):

```
STAGING_URL=https://robin-gpu.cpe.ku.ac.th
ADMIN_USER=admin
ADMIN_PASS=<from iot.env>
```
