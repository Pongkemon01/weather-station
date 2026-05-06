"""OTA campaign selection and slot-wait computation."""
from __future__ import annotations

import zlib
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Optional

import asyncpg


@dataclass
class Campaign:
    id: int
    version: int
    firmware_file_path: str
    firmware_size: int
    firmware_sha256: str
    rollout_start: Optional[datetime]
    rollout_window_days: int
    slot_len_sec: int
    target_cohort_ids: Optional[list[str]]


async def get_active_campaign_for_device(
    conn: asyncpg.Connection, device_id: str
) -> Campaign | None:
    """Return highest-version in_progress campaign the device is eligible for, or None.

    Cohort match: NULL / empty array → entire fleet; otherwise device_id must appear in
    target_cohort_ids. device_id is the 6-char "{region:03d}{station:03d}" string.
    """
    row = await conn.fetchrow(
        """
        SELECT id, version, firmware_file_path, firmware_size, firmware_sha256,
               rollout_start, rollout_window_days, slot_len_sec, target_cohort_ids
        FROM ota_campaigns
        WHERE status = 'in_progress'
          AND (
              target_cohort_ids IS NULL
              OR cardinality(target_cohort_ids) = 0
              OR $1 = ANY(target_cohort_ids)
          )
        ORDER BY version DESC
        LIMIT 1
        """,
        device_id,
    )
    if row is None:
        return None
    return Campaign(
        id=row["id"],
        version=row["version"],
        firmware_file_path=row["firmware_file_path"],
        firmware_size=row["firmware_size"],
        firmware_sha256=row["firmware_sha256"],
        rollout_start=row["rollout_start"],
        rollout_window_days=row["rollout_window_days"],
        slot_len_sec=row["slot_len_sec"],
        target_cohort_ids=row["target_cohort_ids"],
    )


def compute_wait(device_id: str, campaign: Campaign) -> int:
    """Return seconds the device must wait before it may download.

    Uses zlib.crc32 to assign a stable slot per device, matching the firmware-side
    algorithm (Arch §3.3). Returns 0 if the device is in or past its assigned slot.
    Monotone-in-time: once eligible, always eligible (absent pause/cancel).
    """
    if not campaign.rollout_start or campaign.rollout_window_days <= 0:
        return 0

    slot_len = campaign.slot_len_sec
    num_slots = campaign.rollout_window_days * 2
    dev_slot = zlib.crc32(device_id.encode("ascii")) % num_slots

    now = datetime.now(tz=timezone.utc)
    elapsed = (now - campaign.rollout_start).total_seconds()
    now_slot = min(num_slots - 1, max(0, int(elapsed // slot_len)))

    if dev_slot <= now_slot:
        return 0
    return (dev_slot - now_slot) * slot_len
