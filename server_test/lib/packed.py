"""Encode/decode Weather_Data_Packed_t exactly as firmware does.

Binary layout (little-endian, packed):
  Header:  u16 region_id | u16 station_id | u8 count          = 5 bytes
  Record:  u32 ts | i16 temp | i16 hum | i16 pres | u16 light
           | i16 rain | i16 dew | i16 bus                      = 18 bytes each

Max batch: 28 records → 5 + 28×18 = 509 bytes ≤ 512-byte upload window.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta

from .fixedpt import from_fixed, to_fixed

_HEADER = struct.Struct("<HHB")
_RECORD = struct.Struct("<IhhhHhhh")

_Y2K = datetime(2000, 1, 1, tzinfo=UTC)


@dataclass
class Sample:
    ts: int              # seconds since 2000-01-01 UTC
    temperature: float   # °C
    humidity: float      # %RH
    pressure: float      # kPa
    light_par: int       # µmol/s·m²
    rainfall: float      # mm/hr
    dew_point: float     # °C
    bus_value: float     # blast unit of severity

    @property
    def time(self) -> datetime:
        return _Y2K + timedelta(seconds=self.ts)


def encode(region: int, station: int, samples: list[Sample]) -> bytes:
    """Pack samples into the binary upload payload."""
    hdr = _HEADER.pack(region, station, len(samples))
    body = b"".join(
        _RECORD.pack(
            s.ts,
            to_fixed(s.temperature),
            to_fixed(s.humidity),
            to_fixed(s.pressure),
            s.light_par,
            to_fixed(s.rainfall),
            to_fixed(s.dew_point),
            to_fixed(s.bus_value),
        )
        for s in samples
    )
    return hdr + body


def make_sample(**overrides) -> Sample:
    """Return a default sample with optional field overrides."""
    defaults: dict = {
        "ts": 769622400,    # 2024-05-19T00:00:00Z
        "temperature": 25.5,
        "humidity": 60.25,
        "pressure": 101.3,
        "light_par": 1500,
        "rainfall": 2.75,
        "dew_point": 17.0,
        "bus_value": 0.5,
    }
    defaults.update(overrides)
    return Sample(**defaults)
