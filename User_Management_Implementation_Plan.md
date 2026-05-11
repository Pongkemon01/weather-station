# User Management Implementation Plan

> All UM phases complete (2026-05-08). 75/75 TUM tests pass on `robin-gpu.cpe.ku.ac.th`.
> Test plan: `User_Management_Test_Plan.md`. Status: `IMPLEMENTATION_STATUS.md`.

---

## Scope

All roles — **admin**, **operator**, **viewer** — have identical access to every IoT page (dashboard, campaigns, devices, sensor data). Roles differ **only** for user management:

| Actor | User management capability |
|-------|---------------------------|
| **admin** | List all users; create user (any role); edit any user's username/role; change any user's password; delete other users; edit own profile |
| **operator / viewer** | View and edit own username; change own password (requires current password); cannot change own role |

**Hard rules** (enforced at every layer):

- Admin cannot delete themselves (lockout prevention).
- Admin cannot demote their own role via any form.
- Username globally unique (DB `UNIQUE` + application 409).
- Password ≥ 8 chars; bcrypt cost ≥ 12 (matches existing `auth/jwt.py`).
- CSRF token required on every form mutation (HMAC double-submit).
- All HTML pages redirect to login on missing/expired JWT cookie.

---

## Phase Summary

| Phase | Files changed | Description |
|-------|---------------|-------------|
| UM1 | `app/db/queries.py` | 7 new async functions: `list_admin_users` (ORDER BY role then created_at), `get_admin_user_by_id`, `username_exists(exclude_id=None)`, `create_admin_user` (returns id or None on conflict), `update_admin_user_info` (returns id or None on conflict), `update_admin_user_password`, `delete_admin_user`. 15/15 unit tests pass. |
| UM2 | `app/routers/admin.py` | 4 new JSON endpoints + extend list: `POST /admin/users`, `PUT /admin/users/{id}`, `PUT /admin/users/{id}/password`, `DELETE /admin/users/{id}`. Self-demote / self-delete guards. Operator-on-other / role-field-on-self return 403. 9/9 verifications. |
| UM3 | `app/routers/ui.py` | 6 HTML routes: `GET /admin/users.html` (admin), `GET /admin/profile.html` (viewer+), `POST /admin/users/create-ui`, `POST /admin/users/{id}/update-info-ui`, `POST /admin/users/{id}/password-ui`, `POST /admin/users/{id}/delete-ui`. All form mutations CSRF-checked. HTMX-driven row/table swaps. 36/36 (TUM5–TUM11) tests pass. |
| UM4 | `app/templates/base.html`, `users.html`, `profile.html`, `partials/user_table.html` | Admin-only "Users" nav item; footer pill links to `/admin/profile.html`; admin's own row has no delete button; role dropdown absent for non-admin users; `confirm_password` mismatch inline error (no page reload). Verified by TUM5/6/7/11. |
| UM5 | `app/routers/ui.py`, `partials/user_form_modal.html` | Modal fragment routes: `GET /admin/users/new-modal` (create mode), `GET /admin/users/{id}/edit-modal` (edit mode). Single template with `create` boolean. Modal closes via HTMX `hx-on::after-request`. |

---

## API Contract

### `POST /admin/users` — admin only

```
Body: {username, password, role}
Validate: role ∈ {viewer,operator,admin}; password ≥ 8; username 1–64, [A-Za-z0-9_.-]
On conflict: 409 {detail: "Username already exists"}
Return: 201 {id, username, role, created_at}
```

### `PUT /admin/users/{user_id}` — viewer+ (self) or admin (any)

```
Body: {username?, role?}
Guards (in order):
  - non-admin caller targeting other user → 403
  - admin caller targeting self with role != admin → 403 "Admin cannot demote themselves"
  - non-admin caller sending role field → 403 "Cannot change own role"
On username conflict: 409   |   Not found: 404   |   Return: 200 {id, username, role}
```

### `PUT /admin/users/{user_id}/password` — viewer+ (self) or admin (any)

```
Body: {current_password?, new_password}
Guards:
  - non-admin caller targeting other user → 403
  - self (any role): current_password REQUIRED; bcrypt-verify; wrong → 422
  - admin targeting other: current_password NOT required
  - new_password < 8 → 422
Return: 204
```

### `DELETE /admin/users/{user_id}` — admin only

```
Guard: user_id == caller.sub_id → 403 "Cannot delete yourself"
Not found: 404   |   Return: 204
```

### `GET /admin/users` — extended (admin only)

Now returns `id`, `username`, `role`, `created_at` (previously usernames only).

---

## Form Routes (HTMX)

All `POST` form routes:

- Validate CSRF token (HMAC double-submit via `app/auth/csrf.py`).
- Mirror the JSON API guards above.
- Return either an HTMX partial fragment (table row, error inline) or a 303 redirect with `?created=1` / `?updated=1`.

Specific HTMX behaviors:

- **Create user** (`POST /admin/users/create-ui`): success → swap `#user-table`; conflict / validation error → inline error fragment.
- **Update info** (`POST /admin/users/{id}/update-info-ui`): admin on users page → `partials/user_table.html` swap; self on profile → redirect `profile.html?updated=1`. Error → `hx-swap-oob` error message.
- **Change password** (`POST /admin/users/{id}/password-ui`): success → 200 + `HX-Trigger: passwordChanged` header; error → inline error fragment.
- **Delete user** (`POST /admin/users/{id}/delete-ui`): success → `hx-target="closest tr" hx-swap="outerHTML"` returning empty string.

---

## Templates

```
app/templates/
├── base.html                         ; admin-only Users nav item; footer pill → profile
├── users.html                        ; admin-only; table + "New User" button → /admin/users/new-modal
├── profile.html                      ; viewer+; info form + password form (current_password shown on self)
└── partials/
    ├── user_table.html               ; row per user; no delete button on caller's own row
    └── user_form_modal.html          ; single template, mode toggled by `create` boolean
```

`base.html` snippet:

```html
{% if user and user.role == 'admin' %}
<li class="nav-item">
  <a class="nav-link {% if active == 'users' %}active{% endif %}"
     href="/admin/users.html">Users</a>
</li>
{% endif %}
```

`profile.html` admin-only role dropdown:

```html
{% if user.role == 'admin' %}
<select name="role">...</select>
{% else %}
<p>Role: {{ profile.role }}</p>
{% endif %}
```

---

## Deployment

`bash html/scripts/deploy.sh` → `journalctl -u iot-server -n 50` (verify no startup errors) → manual smoke: login as admin, create test user, edit, delete; login as operator, verify Users page blocked, profile page works.

---

## Files Changed / Added Summary

| File | Status |
|------|--------|
| `html/app/db/queries.py` | Modified — 7 new functions |
| `html/app/routers/admin.py` | Modified — 4 new API endpoints + extended list |
| `html/app/routers/ui.py` | Modified — 6 new HTML routes + 2 modal routes |
| `html/app/templates/base.html` | Modified — Users nav + profile pill |
| `html/app/templates/users.html` | New |
| `html/app/templates/profile.html` | New |
| `html/app/templates/partials/user_table.html` | New |
| `html/app/templates/partials/user_form_modal.html` | New |
