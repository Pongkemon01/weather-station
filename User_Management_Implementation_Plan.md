# User Management Implementation Plan

> Phased plan for adding user management to the admin dashboard.
> Source tree lives in `html/`. Production host: `akp@robin-gpu.cpe.ku.ac.th`.
> Deploy via `bash html/scripts/deploy.sh` after each phase.
>
> Implement phases **strictly in order**. Each phase must pass all listed verifications before the next begins.
> **Architecture reference:** `Server_Architecture.md`
> **Test harness:** `User_Management_Test_Plan.md` (Python black-box verifiers in `server_test/`)

---

## Scope

**IoT data access:** All roles — `admin`, `operator`, and `viewer` — have identical access to every IoT page (dashboard, campaigns, device tables, sensor readings). Role distinctions apply **only** to user management capabilities, as listed below.

| Actor | User management capability |
|-------|---------------------------|
| **admin** | List all users; create user (any role); edit any user's info (username, role); change any user's password; delete other users; edit own profile (same as operator/viewer) |
| **operator / viewer** | View and edit own username; change own password (requires current password); cannot change own role |

**Hard rules:**
- Admin cannot delete themselves (prevent lockout).
- Admin cannot demote their own role via any form (separate guard).
- Username must be globally unique (enforce at DB and application layers).
- Password minimum 8 characters; bcrypt cost factor ≥ 12 (matches existing `auth/jwt.py`).
- CSRF token required on every form mutation (existing double-submit pattern).
- All HTML pages redirect to login if JWT cookie is missing or expired.

---

## Phase UM1 — DB Query Layer

**Files changed:** `html/app/db/queries.py`

**New functions** (all async, take an `asyncpg` connection as first argument):

```
list_admin_users(conn) → list[Record]
    SELECT id, username, role, created_at
    FROM admin_users
    ORDER BY CASE role WHEN 'admin' THEN 0 WHEN 'operator' THEN 1 ELSE 2 END, created_at

get_admin_user_by_id(conn, user_id: int) → Record | None
    SELECT id, username, password_hash, role, created_at
    FROM admin_users WHERE id = $1

username_exists(conn, username: str, exclude_id: int | None = None) → bool
    SELECT EXISTS(SELECT 1 FROM admin_users WHERE username = $1 AND id != $2)
    (exclude_id defaults to -1 when None so the NOT match is always safe)

create_admin_user(conn, username: str, password_hash: str, role: str) → int
    INSERT INTO admin_users (username, password_hash, role)
    VALUES ($1, $2, $3)
    ON CONFLICT (username) DO NOTHING
    RETURNING id
    (returns None if conflict — caller raises HTTPException 409)

update_admin_user_info(conn, user_id: int, username: str, role: str) → int
    UPDATE admin_users SET username = $2, role = $3
    WHERE id = $1
    RETURNING id
    (returns None on conflict — caller raises 409)

update_admin_user_password(conn, user_id: int, password_hash: str) → bool
    UPDATE admin_users SET password_hash = $2 WHERE id = $1
    returns rowcount > 0

delete_admin_user(conn, user_id: int) → bool
    DELETE FROM admin_users WHERE id = $1
    returns rowcount > 0
```

**Verifications:**
- [x] UM1-1 `pytest html/tests/test_queries.py` — 15/15 pass (2026-05-08)
- [x] UM1-2 `username_exists` returns `False` when `exclude_id` matches the row being tested (self-update case).
- [x] UM1-3 `create_admin_user` with duplicate username returns `None` without raising.

---

## Phase UM2 — JSON API Endpoints

**Files changed:** `html/app/routers/admin.py`

Add the following endpoints under the existing JWT auth structure. All use the `require_role` dependency; CRUD endpoints also use the shared db pool.

### Create user — `POST /admin/users`
- Role: `admin`
- Body (JSON): `{username, password, role}`
- Validate: role ∈ `{viewer, operator, admin}`; password ≥ 8 chars; username 1–64 chars, alphanumeric + `_.-`
- Hash password with bcrypt (cost 12) via existing `hash_password()` in `auth/jwt.py`
- Call `create_admin_user`; if `None` returned → 409 `{detail: "Username already exists"}`
- Return 201 `{id, username, role, created_at}`

### Update user info — `PUT /admin/users/{user_id}`
- Role: `viewer` (minimum — self) or `admin` (any)
- Body (JSON): `{username?, role?}`
- Guard: if caller is not admin and `user_id != caller.sub_id` → 403
- Guard: if caller is admin and `user_id == caller.sub_id` and `role` changes to non-admin → 403 `{detail: "Admin cannot demote themselves"}`
- Guard: if caller is not admin and `role` is provided → 403 `{detail: "Cannot change own role"}`
- Call `update_admin_user_info`; if `None` → 409; if not found → 404
- Return 200 `{id, username, role}`

### Change password — `PUT /admin/users/{user_id}/password`
- Role: `viewer` (minimum — self) or `admin` (any)
- Body (JSON): `{current_password?, new_password}`
- Guard: if caller is not admin and `user_id != caller.sub_id` → 403
- Guard: if caller is self (regardless of role), `current_password` required — verify bcrypt; if wrong → 422 `{detail: "Current password incorrect"}`
- Guard: admin changing *other* user's password — `current_password` not required
- Validate: `new_password` ≥ 8 chars
- Hash and call `update_admin_user_password`
- Return 204

### Delete user — `DELETE /admin/users/{user_id}`
- Role: `admin`
- Guard: `user_id == caller.sub_id` → 403 `{detail: "Cannot delete yourself"}`
- Call `delete_admin_user`; if `False` → 404
- Return 204

### List users — `GET /admin/users` (already exists, extend)
- Currently returns only usernames. Extend response to include `id`, `role`, `created_at`.
- No behaviour change, just richer payload.

**Verifications:**
- [x] UM2-1 `POST /admin/users` with valid payload as admin → 201 with new `id`. (2026-05-08)
- [x] UM2-2 `POST /admin/users` duplicate username → 409. (2026-05-08)
- [x] UM2-3 `PUT /admin/users/{id}` as operator on own account, no `role` field → 200. (2026-05-08)
- [x] UM2-4 `PUT /admin/users/{id}` as operator on other user → 403. (2026-05-08)
- [x] UM2-5 `PUT /admin/users/{id}` as operator with `role` field → 403. (2026-05-08)
- [x] UM2-6 `PUT /admin/users/{admin_id}/password` as admin on own account, wrong `current_password` → 422. (2026-05-08)
- [x] UM2-7 `PUT /admin/users/{other_id}/password` as admin, no `current_password` → 204. (2026-05-08)
- [x] UM2-8 `DELETE /admin/users/{self_id}` as admin → 403. (2026-05-08)
- [x] UM2-9 `DELETE /admin/users/{other_id}` as admin → 204. (2026-05-08)

---

## Phase UM3 — HTML UI Routes

**Files changed:** `html/app/routers/ui.py`

All routes read `access_token` cookie via `_get_user()` and redirect to login on failure.

### User management page — `GET /admin/users.html`
- Role: `admin`
- Calls `list_admin_users`
- Renders `users.html` with `users` list and `user` (current user) context

### Profile page — `GET /admin/profile.html`
- Role: `viewer` (minimum)
- Calls `get_admin_user_by_id(conn, current_user.sub_id)`
- Renders `profile.html` with `profile` and `user` context

### Create user form — `POST /admin/users/create-ui`
- Role: `admin`
- Form fields: `username`, `password`, `confirm_password`, `role`, `csrf_token`
- Validate CSRF; validate `password == confirm_password`; validate password ≥ 8 chars; validate role
- Call `create_admin_user`; on success redirect to `/admin/users.html?created=1`; on 409 re-render form with error
- Uses `hx-post` (HTMX) to replace `#user-table` partial on success, or render inline error on failure

### Update info form — `POST /admin/users/{user_id}/update-info-ui`
- Role: `viewer` (minimum)
- Form fields: `username`, `role` (role field only rendered for admin), `csrf_token`
- Apply same guards as UM2 `PUT /admin/users/{id}`
- On success: if admin on users page → return `user_table.html` partial (HTMX swap); if self on profile page → redirect `profile.html?updated=1`
- On error: return form fragment with `hx-swap-oob` error message

### Change password form — `POST /admin/users/{user_id}/password-ui`
- Role: `viewer` (minimum)
- Form fields: `current_password` (shown when self), `new_password`, `confirm_password`, `csrf_token`
- Validate `new_password == confirm_password`; apply same guards as UM2
- On success: return empty 200 with `HX-Trigger: passwordChanged` header (HTMX triggers success toast)
- On error: return form fragment with inline error message

### Delete user — `POST /admin/users/{user_id}/delete-ui`
- Role: `admin`
- Form fields: `csrf_token`
- Guard: cannot delete self
- On success: remove row from DOM via HTMX `hx-target="closest tr" hx-swap="outerHTML"` returning empty string

**Verifications:**
- [x] UM3-1 `GET /admin/users.html` as admin → 200, table shows all users. (2026-05-08)
- [x] UM3-2 `GET /admin/users.html` as operator → redirect to login (or 403 fragment). (2026-05-08)
- [x] UM3-3 `GET /admin/profile.html` as viewer → 200, shows own username and role. (2026-05-08)
- [x] UM3-4 `POST /admin/users/create-ui` CSRF mismatch → 403. (2026-05-08)
- [x] UM3-5 `POST /admin/users/create-ui` `password != confirm_password` → form re-render with error, no DB write. (2026-05-08)
- [x] UM3-6 `POST /admin/users/{id}/update-info-ui` operator on own account, no role field → 200. (2026-05-08)
- [x] UM3-7 `POST /admin/users/{id}/password-ui` wrong `current_password` → form error message, no hash change. (2026-05-08)

---

## Phase UM4 — Templates

**New files:**
- `html/app/templates/users.html`
- `html/app/templates/profile.html`
- `html/app/templates/partials/user_table.html`

**Modified files:**
- `html/app/templates/base.html` — add nav links

### `base.html` nav additions
```html
{# admin-only: Users link #}
{% if user and user.role == 'admin' %}
<li class="nav-item">
  <a class="nav-link {% if active == 'users' %}active{% endif %}"
     href="/admin/users.html">Users</a>
</li>
{% endif %}

{# all authenticated users: Profile link (footer user pill becomes clickable) #}
{% if user %}
<a href="/admin/profile.html" class="...">{{ user.sub }} · {{ user.role }}</a>
{% endif %}
```

### `users.html` structure
```
{% extends "base.html" %}
{% block content %}
  <h2>Users</h2>
  <button hx-get="/admin/users/new-modal" hx-target="#modal">New User</button>

  <table id="user-table">
    {% include "partials/user_table.html" %}
  </table>

  <div id="modal"></div>  {# create-user modal target #}
{% endblock %}
```

### `partials/user_table.html` structure
```
<thead>
  <tr><th>Username</th><th>Role</th><th>Created</th><th></th></tr>
</thead>
<tbody>
{% for u in users %}
<tr id="user-row-{{ u.id }}">
  <td>{{ u.username }}</td>
  <td><span class="badge badge-{{ u.role }}">{{ u.role }}</span></td>
  <td>{{ u.created_at | datetimeformat }}</td>
  <td>
    <button hx-get="/admin/users/{{ u.id }}/edit-modal" hx-target="#modal">Edit</button>
    {% if u.id != user.sub_id %}
    <form hx-post="/admin/users/{{ u.id }}/delete-ui"
          hx-target="#user-row-{{ u.id }}" hx-swap="outerHTML">
      <input type="hidden" name="csrf_token" value="{{ csrf_token }}">
      <button type="submit" onclick="return confirm('Delete {{ u.username }}?')">Delete</button>
    </form>
    {% endif %}
  </td>
</tr>
{% endfor %}
</tbody>
```

### `profile.html` structure
```
{% extends "base.html" %}
{% block content %}
  <h2>Profile</h2>

  {# Section 1: Info #}
  <form hx-post="/admin/users/{{ profile.id }}/update-info-ui"
        hx-target="#info-result">
    <input name="username" value="{{ profile.username }}">
    {% if user.role == 'admin' %}
    <select name="role">...options...</select>
    {% else %}
    <p>Role: {{ profile.role }}</p>
    {% endif %}
    <input type="hidden" name="csrf_token" value="{{ csrf_token }}">
    <button type="submit">Save</button>
  </form>
  <div id="info-result"></div>

  {# Section 2: Change password #}
  <form hx-post="/admin/users/{{ profile.id }}/password-ui"
        hx-target="#pw-result">
    <input type="password" name="current_password" placeholder="Current password">
    <input type="password" name="new_password" placeholder="New password (min 8)">
    <input type="password" name="confirm_password" placeholder="Confirm">
    <input type="hidden" name="csrf_token" value="{{ csrf_token }}">
    <button type="submit">Change Password</button>
  </form>
  <div id="pw-result"></div>
{% endblock %}
```

**Verifications:**
- [x] UM4-1 Admin nav sidebar shows "Users" link; operator/viewer sidebar does not. (2026-05-08 — TUM11-1/2)
- [x] UM4-2 Footer user pill links to `/admin/profile.html`. (2026-05-08 — TUM11-3)
- [x] UM4-3 Delete button absent on admin's own row in `users.html`. (2026-05-08 — TUM5-5)
- [x] UM4-4 Role dropdown absent on profile page for non-admin users. (2026-05-08 — TUM6-2)
- [x] UM4-5 `confirm_password` mismatch shows inline error without page reload. (2026-05-08 — TUM7-4)

---

## Phase UM5 — Add Modal Routes (HTMX fragments)

**Files changed:** `html/app/routers/ui.py`

These routes return small HTML fragments (no base template) consumed by HTMX to populate `#modal`.

### `GET /admin/users/new-modal`
- Role: `admin`
- Renders `partials/user_form_modal.html` (create mode): username input, password + confirm inputs, role select, CSRF hidden field, `hx-post` to `/admin/users/create-ui`

### `GET /admin/users/{user_id}/edit-modal`
- Role: `admin`
- Fetches user by id; renders `partials/user_form_modal.html` (edit mode): pre-filled username + role (read-only password section replaced with change-password accordion), CSRF hidden field

**New template:** `html/app/templates/partials/user_form_modal.html`
- Single template, mode toggled by `create` boolean context variable
- Uses `<dialog>` element closed via `hx-on::after-request` or JS `dialog.close()`

**Verifications:**
- [x] UM5-1 `GET /admin/users/new-modal` as admin → 200 fragment with form fields. (2026-05-08)
- [x] UM5-2 `GET /admin/users/{id}/edit-modal` as admin → 200 fragment pre-filled with username and role. (2026-05-08)
- [x] UM5-3 Modal closes after successful create or edit (HTMX `afterRequest` handler). (2026-05-08)

---

## Deployment Checklist

Before deploying each phase to production:

1. Run `server_test/` suite against staging (see `User_Management_Test_Plan.md`)
2. `bash html/scripts/deploy.sh` (deploys to `akp@robin-gpu.cpe.ku.ac.th`)
3. SSH in and `journalctl -u iot-server -n 50` — confirm no startup errors
4. Manual smoke test: login as admin, navigate to Users page, create a test user, edit it, delete it; login as operator, verify Users page is blocked, profile page works

---

## Files Created / Modified Summary

| File | Status |
|------|--------|
| `html/app/db/queries.py` | Modified — 7 new functions |
| `html/app/routers/admin.py` | Modified — 4 new API endpoints + extend list |
| `html/app/routers/ui.py` | Modified — 7 new HTML routes |
| `html/app/templates/base.html` | Modified — nav + profile pill link |
| `html/app/templates/users.html` | New |
| `html/app/templates/profile.html` | New |
| `html/app/templates/partials/user_table.html` | New |
| `html/app/templates/partials/user_form_modal.html` | New |
