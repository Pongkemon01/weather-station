"""Unit tests for Phase UM1 user-management query functions (html/app/db/queries.py).

Uses AsyncMock to fake asyncpg connections — no live DB required.
Covers UM1-1 (all functions), UM1-2 (username_exists self-update), UM1-3 (create conflict).
"""
from __future__ import annotations

import pytest
from unittest.mock import AsyncMock, MagicMock

from app.db.queries import (
    list_admin_users,
    get_admin_user_by_id,
    username_exists,
    create_admin_user,
    update_admin_user_info,
    update_admin_user_password,
    delete_admin_user,
)


def _conn(**method_returns) -> AsyncMock:
    """Return a minimal asyncpg Connection mock with specified method return values."""
    conn = AsyncMock()
    for method, retval in method_returns.items():
        getattr(conn, method).return_value = retval
    return conn


def _row(id_val: int) -> MagicMock:
    """Return a mock asyncpg Record where row['id'] == id_val."""
    row = MagicMock()
    row.__getitem__ = MagicMock(return_value=id_val)
    return row


# ---------------------------------------------------------------------------
# list_admin_users
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_list_admin_users_returns_rows():
    rows = [{"id": 1, "username": "admin", "role": "admin", "created_at": None}]
    conn = _conn(fetch=rows)
    assert await list_admin_users(conn) == rows
    conn.fetch.assert_awaited_once()


# ---------------------------------------------------------------------------
# get_admin_user_by_id
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_get_admin_user_by_id_found():
    row = {"id": 5, "username": "alice", "password_hash": "x", "role": "viewer", "created_at": None}
    conn = _conn(fetchrow=row)
    assert await get_admin_user_by_id(conn, 5) == row
    conn.fetchrow.assert_awaited_once()


@pytest.mark.asyncio
async def test_get_admin_user_by_id_not_found():
    conn = _conn(fetchrow=None)
    assert await get_admin_user_by_id(conn, 999) is None


# ---------------------------------------------------------------------------
# username_exists — UM1-2
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_username_exists_true():
    conn = _conn(fetchval=True)
    assert await username_exists(conn, "alice") is True


@pytest.mark.asyncio
async def test_username_exists_false():
    conn = _conn(fetchval=False)
    assert await username_exists(conn, "alice") is False


@pytest.mark.asyncio
async def test_username_exists_none_exclude_uses_minus_one():
    """UM1-2: exclude_id=None must pass -1 to the query (safe default — no real id is -1)."""
    conn = _conn(fetchval=False)
    await username_exists(conn, "alice", exclude_id=None)
    _, args, _ = conn.fetchval.mock_calls[0]
    assert args[2] == -1


@pytest.mark.asyncio
async def test_username_exists_self_update_passes_real_id():
    """UM1-2: self-update case — own id passed so the row is excluded from the EXISTS check."""
    conn = _conn(fetchval=False)
    await username_exists(conn, "alice", exclude_id=42)
    _, args, _ = conn.fetchval.mock_calls[0]
    assert args[2] == 42


# ---------------------------------------------------------------------------
# create_admin_user — UM1-3
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_create_admin_user_returns_new_id():
    conn = _conn(fetchrow=_row(7))
    assert await create_admin_user(conn, "bob", "hash", "viewer") == 7


@pytest.mark.asyncio
async def test_create_admin_user_duplicate_returns_none():
    """UM1-3: ON CONFLICT DO NOTHING → fetchrow returns None → function returns None without raising."""
    conn = _conn(fetchrow=None)
    result = await create_admin_user(conn, "bob", "hash", "viewer")
    assert result is None


# ---------------------------------------------------------------------------
# update_admin_user_info
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_update_admin_user_info_found():
    conn = _conn(fetchrow=_row(3))
    assert await update_admin_user_info(conn, 3, "new_name", "operator") == 3


@pytest.mark.asyncio
async def test_update_admin_user_info_not_found():
    conn = _conn(fetchrow=None)
    assert await update_admin_user_info(conn, 999, "x", "viewer") is None


# ---------------------------------------------------------------------------
# update_admin_user_password
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_update_admin_user_password_success():
    conn = _conn(execute="UPDATE 1")
    assert await update_admin_user_password(conn, 1, "newhash") is True


@pytest.mark.asyncio
async def test_update_admin_user_password_not_found():
    conn = _conn(execute="UPDATE 0")
    assert await update_admin_user_password(conn, 999, "newhash") is False


# ---------------------------------------------------------------------------
# delete_admin_user
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_delete_admin_user_success():
    conn = _conn(execute="DELETE 1")
    assert await delete_admin_user(conn, 1) is True


@pytest.mark.asyncio
async def test_delete_admin_user_not_found():
    conn = _conn(execute="DELETE 0")
    assert await delete_admin_user(conn, 999) is False
