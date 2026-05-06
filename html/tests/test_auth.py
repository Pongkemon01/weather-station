"""Unit tests for app.auth.jwt — Phase 6 (S6-5)."""
from __future__ import annotations

import bcrypt
import jwt as pyjwt
import pytest
from datetime import UTC, datetime, timedelta
from fastapi import HTTPException

from app.auth.jwt import _ALGORITHM, check_password, create_token, verify_token
from app.config import settings


def test_create_and_verify_round_trip():
    token = create_token("alice", "admin")
    payload = verify_token(token)
    assert payload["sub"] == "alice"
    assert payload["role"] == "admin"


def test_token_exp_is_24h_from_now():
    before = datetime.now(UTC)
    token = create_token("alice", "viewer")
    payload = pyjwt.decode(token, settings.jwt_secret, algorithms=[_ALGORITHM])
    exp = datetime.fromtimestamp(payload["exp"], tz=UTC)
    delta = exp - before
    assert timedelta(hours=23, minutes=59) < delta <= timedelta(hours=24, seconds=5)


def test_expired_token_raises_401():
    payload = {"sub": "bob", "role": "viewer", "exp": datetime.now(UTC) - timedelta(seconds=1)}
    expired = pyjwt.encode(payload, settings.jwt_secret, algorithm=_ALGORITHM)
    with pytest.raises(HTTPException) as exc_info:
        verify_token(expired)
    assert exc_info.value.status_code == 401


def test_invalid_token_raises_401():
    with pytest.raises(HTTPException) as exc_info:
        verify_token("not.a.valid.token")
    assert exc_info.value.status_code == 401


def test_tampered_token_raises_401():
    token = create_token("carol", "admin")
    # Flip a character in the signature portion.
    parts = token.split(".")
    parts[2] = parts[2][:-1] + ("A" if parts[2][-1] != "A" else "B")
    with pytest.raises(HTTPException) as exc_info:
        verify_token(".".join(parts))
    assert exc_info.value.status_code == 401


def test_check_password_correct():
    hashed = bcrypt.hashpw(b"secret", bcrypt.gensalt()).decode()
    assert check_password("secret", hashed) is True


def test_check_password_wrong():
    hashed = bcrypt.hashpw(b"correct", bcrypt.gensalt()).decode()
    assert check_password("wrong", hashed) is False
