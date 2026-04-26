"""
Migration 0002: seed initial admin user.

Reads ADMIN_USERNAME (default 'admin') and ADMIN_PASSWORD from env.
ADMIN_PASSWORD must be set; migration raises if absent.
ON CONFLICT DO NOTHING makes re-runs safe.
"""
from __future__ import annotations

import asyncpg
import bcrypt


async def run(conn: asyncpg.Connection, env: dict[str, str]) -> None:
    username = env.get("ADMIN_USERNAME", "admin")
    password = env.get("ADMIN_PASSWORD", "")
    if not password:
        raise ValueError(
            "ADMIN_PASSWORD must be set in iot.env before running migrations"
        )
    pw_hash = bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()
    await conn.execute(
        """
        INSERT INTO admin_users (username, password_hash, role)
        VALUES ($1, $2, 'admin')
        ON CONFLICT (username) DO NOTHING
        """,
        username,
        pw_hash,
    )
