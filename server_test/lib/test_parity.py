"""T0-7: Parity tests for lib/ utilities (no network required).

Verifies that local CRC, fixedpt, and packed implementations match the
firmware/server implementations via pre-computed reference vectors.

Run: pytest server_test/lib/
"""
from __future__ import annotations

import struct

import pytest

from .crc32 import crc32_mpeg2
from .fixedpt import from_fixed, to_fixed, _LSB
from .packed import Sample, encode


# ── CRC-32/MPEG-2 (T0-3) ─────────────────────────────────────────────────────

@pytest.mark.parametrize("data,expected", [
    (b"",           0xFFFFFFFF),   # empty: init returned unchanged
    (b"123456789",  0x0376E6E7),   # standard MPEG-2 check value
])
def test_crc32_mpeg2_known_vectors(data, expected):
    assert crc32_mpeg2(data) == expected


def test_crc32_mpeg2_accumulation():
    """Chunked accumulation must equal one-shot."""
    data = b"Hello, world!"
    one_shot = crc32_mpeg2(data)
    crc = 0xFFFFFFFF
    for b in data:
        crc = crc32_mpeg2(bytes([b]), init=crc)
    assert crc == one_shot


def test_crc32_not_zlib_crc32():
    """Must differ from Python zlib.crc32 (different poly / reflection)."""
    import zlib
    data = b"123456789"
    assert crc32_mpeg2(data) != (zlib.crc32(data) & 0xFFFFFFFF)


# ── Fixed-point S9.7 (T0-4) ──────────────────────────────────────────────────

@pytest.mark.parametrize("v,raw", [
    (0.0,          0),
    (1.0,          128),
    (25.5,         3264),
    (255.9921875,  32767),
    (-1.0,         -128),
    (-256.0,       -32768),
])
def test_to_fixed_known_values(v, raw):
    assert to_fixed(v) == raw


@pytest.mark.parametrize("v", [0.0, 1.0, 25.5, 127.9921875, -1.0, -25.5, -256.0])
def test_fixed_round_trip(v):
    assert abs(from_fixed(to_fixed(v)) - v) <= _LSB


def test_from_fixed_sign_extension():
    # raw 0xFFFF as signed 16-bit = -1 → -1/128
    assert from_fixed(0xFFFF) == pytest.approx(-_LSB, abs=1e-9)


@pytest.mark.parametrize("v,expected", [
    (255.9921875,  _LSB * 32767),   # positive extreme
    (-256.0,       -256.0),         # negative extreme: -32768 / 128
])
def test_from_fixed_extremes(v, expected):
    assert abs(from_fixed(to_fixed(v)) - v) <= _LSB


# ── Packed encoder (T0-5) ─────────────────────────────────────────────────────

def test_encoded_length_single():
    s = Sample(ts=0, temperature=0, humidity=0, pressure=0,
               light_par=0, rainfall=0, dew_point=0, bus_value=0)
    assert len(encode(1, 2, [s])) == 23   # 5 + 1×18


def test_encoded_length_max_batch():
    s = Sample(ts=0, temperature=0, humidity=0, pressure=0,
               light_par=0, rainfall=0, dew_point=0, bus_value=0)
    payload = encode(1, 2, [s] * 28)
    assert len(payload) == 509    # 5 + 28×18


def test_header_bytes():
    s = Sample(ts=0, temperature=0, humidity=0, pressure=0,
               light_par=0, rainfall=0, dew_point=0, bus_value=0)
    payload = encode(42, 7, [s])
    region, station, count = struct.unpack_from("<HHB", payload)
    assert region == 42
    assert station == 7
    assert count == 1


def test_temperature_encoded_in_payload():
    s = Sample(ts=0, temperature=25.5, humidity=0, pressure=0,
               light_par=0, rainfall=0, dew_point=0, bus_value=0)
    payload = encode(1, 1, [s])
    _, temp_raw = struct.unpack_from("<Ih", payload, 5)   # ts(4) + temp(2)
    assert abs(from_fixed(temp_raw) - 25.5) <= _LSB
