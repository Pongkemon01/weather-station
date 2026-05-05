"""S3-7: Unit tests for parser.py + fixedpt.py.

Parses fixture byte-strings constructed from known Weather_Data_Packed_t values
and asserts all fields round-trip within ±1 LSB (1/128 ≈ 0.0078).
"""
from __future__ import annotations

import struct
from datetime import UTC, datetime

import pytest

from app.ota.fixedpt import from_fixed, to_fixed
from app.ota.parser import parse_upload, y2k_to_utc

_HEADER = struct.Struct("<HHB")
_RECORD = struct.Struct("<IhhhHhhh")
_LSB = 1 / 128


def _build_payload(region: int, station: int, samples: list[dict]) -> bytes:
    hdr = _HEADER.pack(region, station, len(samples))
    body = b"".join(
        _RECORD.pack(
            s["ts"],
            to_fixed(s["temperature"]),
            to_fixed(s["humidity"]),
            to_fixed(s["pressure"]),
            s["light_par"],
            to_fixed(s["rainfall"]),
            to_fixed(s["dew_point"]),
            to_fixed(s["bus_value"]),
        )
        for s in samples
    )
    return hdr + body


_SAMPLE = {
    "ts": 769622400,  # Y2K epoch; 2024-05-19T00:00:00Z
    "temperature": 25.5,
    "humidity": 60.25,
    "pressure": 101.3,
    "light_par": 1500,
    "rainfall": 2.75,
    "dew_point": 17.0,
    "bus_value": 0.5,
}


class TestFixedpt:
    @pytest.mark.parametrize("v", [0.0, 1.0, 25.5, 127.9921875, 255.9921875])
    def test_positive_round_trip(self, v):
        assert abs(from_fixed(to_fixed(v)) - v) <= _LSB

    @pytest.mark.parametrize("v", [-1.0, -25.5, -128.0, -256.0])
    def test_negative_round_trip(self, v):
        assert abs(from_fixed(to_fixed(v)) - v) <= _LSB

    def test_zero(self):
        assert from_fixed(0) == 0.0

    def test_sign_extension(self):
        # raw 0xFFFF should be -1/128 ≈ -0.0078125
        assert from_fixed(0xFFFF) == pytest.approx(-_LSB, abs=1e-9)


class TestY2kEpoch:
    def test_zero_is_y2k(self):
        assert y2k_to_utc(0) == datetime(2000, 1, 1, tzinfo=UTC)

    def test_aware(self):
        dt = y2k_to_utc(86400)
        assert dt.tzinfo is not None


class TestParser:
    def test_single_record_round_trip(self):
        payload = _build_payload(42, 1, [_SAMPLE])
        region, station, records = parse_upload(payload)
        assert region == 42
        assert station == 1
        assert len(records) == 1
        r = records[0]
        assert abs(r["temperature"] - _SAMPLE["temperature"]) <= _LSB
        assert abs(r["humidity"] - _SAMPLE["humidity"]) <= _LSB
        assert abs(r["pressure"] - _SAMPLE["pressure"]) <= _LSB
        assert r["light_par"] == _SAMPLE["light_par"]
        assert abs(r["rainfall"] - _SAMPLE["rainfall"]) <= _LSB
        assert abs(r["dew_point"] - _SAMPLE["dew_point"]) <= _LSB
        assert abs(r["bus_value"] - _SAMPLE["bus_value"]) <= _LSB

    def test_max_batch_28_records(self):
        # 28 × 18 + 5 = 509 bytes (max batch per plan)
        payload = _build_payload(1, 2, [_SAMPLE] * 28)
        assert len(payload) == 509
        _, _, records = parse_upload(payload)
        assert len(records) == 28

    def test_y2k_epoch_zero_timestamp(self):
        sample = {**_SAMPLE, "ts": 0}
        payload = _build_payload(1, 1, [sample])
        _, _, records = parse_upload(payload)
        assert records[0]["time"] == datetime(2000, 1, 1, tzinfo=UTC)

    def test_rejects_too_short(self):
        with pytest.raises(ValueError):
            parse_upload(b"\x00\x01\x00")

    def test_rejects_count_mismatch(self):
        # Build 4 records but set count=5 in header
        payload = bytearray(_build_payload(1, 1, [_SAMPLE] * 4))
        payload[4] = 5  # count byte at header offset 4
        with pytest.raises(ValueError, match="payload length"):
            parse_upload(bytes(payload))

    def test_rejects_extra_bytes(self):
        # Trailing garbage byte
        payload = _build_payload(1, 1, [_SAMPLE]) + b"\x00"
        with pytest.raises(ValueError, match="payload length"):
            parse_upload(payload)

    def test_field_boundary_extremes(self):
        sample = {**_SAMPLE, "temperature": -256.0, "humidity": 255.9921875}
        payload = _build_payload(1, 1, [sample])
        _, _, records = parse_upload(payload)
        assert abs(records[0]["temperature"] - (-256.0)) <= _LSB
        assert abs(records[0]["humidity"] - 255.9921875) <= _LSB
