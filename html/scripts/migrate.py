"""
Idempotent database migration runner.

Usage (run from html/ directory):
    python -m scripts.migrate              # apply all pending migrations
    python -m scripts.migrate --dry-run   # print pending SQL/Python files; do not apply
    python -m scripts.migrate --check     # exit 1 if any migration is pending

Migration files live in app/db/migrations/ and are applied in lexicographic order.
  *.sql  — executed verbatim inside a transaction
  *.py   — imported; async def run(conn, env) called inside the same transaction

Applied migrations are recorded in the schema_migrations table.
"""
from __future__ import annotations

import argparse
import asyncio
import importlib.util
import os
import sys
from pathlib import Path

import asyncpg
from dotenv import dotenv_values

# Locate directories relative to this file so the script works from any cwd.
_SCRIPTS_DIR = Path(__file__).parent
_HTML_DIR = _SCRIPTS_DIR.parent
_MIGRATIONS_DIR = _HTML_DIR / "app" / "db" / "migrations"

# Ensure html/ is on the path so `from app.config import settings` works in Python migrations.
if str(_HTML_DIR) not in sys.path:
    sys.path.insert(0, str(_HTML_DIR))


def _load_env() -> dict[str, str]:
    env_file = os.getenv("IOT_ENV_FILE", str(_HTML_DIR / "etc" / "iot.env"))
    env = {**dotenv_values(env_file), **os.environ}
    return env


def _migration_files() -> list[Path]:
    files = sorted(
        p for p in _MIGRATIONS_DIR.iterdir()
        if p.suffix in (".sql", ".py") and not p.name.startswith("_")
    )
    return files


async def _ensure_migrations_table(conn: asyncpg.Connection) -> None:
    await conn.execute(
        """
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version    VARCHAR(255) PRIMARY KEY,
            applied_at TIMESTAMPTZ  DEFAULT now()
        )
        """
    )


async def _applied_versions(conn: asyncpg.Connection) -> set[str]:
    rows = await conn.fetch("SELECT version FROM schema_migrations")
    return {r["version"] for r in rows}


async def _apply_sql(conn: asyncpg.Connection, path: Path) -> None:
    sql = path.read_text(encoding="utf-8")
    await conn.execute(sql)


async def _apply_py(conn: asyncpg.Connection, path: Path, env: dict[str, str]) -> None:
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    await module.run(conn, env)


async def main(dry_run: bool, check: bool) -> int:
    env = _load_env()
    dsn = env.get("DB_DSN", "")
    if not dsn:
        print("ERROR: DB_DSN not set in iot.env", file=sys.stderr)
        return 1

    conn = await asyncpg.connect(dsn=dsn)
    try:
        await _ensure_migrations_table(conn)
        applied = await _applied_versions(conn)
        pending = [f for f in _migration_files() if f.name not in applied]

        if not pending:
            print("No pending migrations.")
            return 0

        if check:
            print(f"{len(pending)} pending migration(s):")
            for f in pending:
                print(f"  {f.name}")
            return 1

        for path in pending:
            print(f"{'[dry-run] ' if dry_run else ''}Applying {path.name} ...", end="")
            if dry_run:
                if path.suffix == ".sql":
                    print()
                    print(path.read_text(encoding="utf-8"))
                else:
                    print(f"\n  (Python migration — would call run(conn, env))")
                continue

            async with conn.transaction():
                if path.suffix == ".sql":
                    await _apply_sql(conn, path)
                else:
                    await _apply_py(conn, path, env)
                await conn.execute(
                    "INSERT INTO schema_migrations (version) VALUES ($1)",
                    path.name,
                )
            print(" done.")

    finally:
        await conn.close()

    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run pending database migrations.")
    parser.add_argument("--dry-run", action="store_true", help="Print SQL; do not apply")
    parser.add_argument("--check", action="store_true", help="Exit 1 if pending migrations exist")
    args = parser.parse_args()

    sys.exit(asyncio.run(main(dry_run=args.dry_run, check=args.check)))
