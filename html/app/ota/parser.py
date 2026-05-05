"""Binary parser for Weather_Data_Packed_t upload payloads."""
from __future__ import annotations

import struct
from datetime import UTC, datetime, timedelta

from app.ota.fixedpt import from_fixed

_HEADER = struct.Struct("<HHB")   # region_id, station_id, count
_RECORD = struct.Struct("<IhhhHhhh")  # ts, temp, hum, pres, light, rain, dew, bus
_RECORD_SIZE = 18

assert _RECORD.size == _RECORD_SIZE, "struct size mismatch"

_Y2K_EPOCH = datetime(2000, 1, 1, tzinfo=UTC)


def y2k_to_utc(seconds: int) -> datetime:
    """Convert Y2K epoch (seconds since 2000-01-01 UTC) to timezone-aware datetime."""
    return _Y2K_EPOCH + timedelta(seconds=seconds)


def parse_upload(payload: bytes) -> tuple[int, int, list[dict]]:
    """Parse a binary upload payload into (region_id, station_id, records).

    Each record dict contains 'time' (datetime) and float sensor fields.
    Raises ValueError on malformed input.
    """
    if len(payload) < _HEADER.size:
        raise ValueError("payload too short for header")
    region, station, count = _HEADER.unpack_from(payload)
    expected = _HEADER.size + count * _RECORD_SIZE
    if len(payload) != expected:
        raise ValueError(
            f"payload length {len(payload)} != expected {expected} (count={count})"
        )
    records: list[dict] = []
    offset = _HEADER.size
    for _ in range(count):
        ts, temp, hum, pres, light, rain, dew, bus = _RECORD.unpack_from(payload, offset)
        records.append({
            "time": y2k_to_utc(ts),
            "temperature": from_fixed(temp),
            "humidity": from_fixed(hum),
            "pressure": from_fixed(pres),
            "light_par": light,
            "rainfall": from_fixed(rain),
            "dew_point": from_fixed(dew),
            "bus_value": from_fixed(bus),
        })
        offset += _RECORD_SIZE
    return region, station, records
