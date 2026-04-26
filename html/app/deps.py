"""Shared FastAPI dependencies."""
from __future__ import annotations

from collections.abc import AsyncGenerator

import asyncpg
from fastapi import Depends

from app.db.pool import get_pool


async def get_db() -> AsyncGenerator[asyncpg.Connection, None]:
    """Yield a database connection from the pool for the duration of a request."""
    async with get_pool().acquire() as conn:
        yield conn
