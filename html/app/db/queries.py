"""Parameterized SQL queries — no ORM; asyncpg directly."""
from __future__ import annotations

import asyncpg

# Query implementations are added per phase alongside the routes that need them.
# Phase 3: upsert_device, ingest_log_exists/insert, insert_weather_records
# Phase 5: get_active_campaign_for_device
# Phase 7: get_max_firmware_version, list_campaigns_by_status, insert_campaign, etc.
