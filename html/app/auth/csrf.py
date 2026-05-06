"""CSRF token helpers — stateless HMAC-signed double-submit pattern."""
from __future__ import annotations

import hashlib
import hmac
import secrets

from app.config import settings


def generate() -> str:
    """Return a signed CSRF token: '{nonce}.{hmac}'."""
    nonce = secrets.token_hex(16)
    sig = hmac.new(settings.jwt_secret.encode(), msg=nonce.encode(), digestmod=hashlib.sha256).hexdigest()
    return f"{nonce}.{sig}"


def verify(token: str) -> bool:
    """Return True iff token was issued by this server (signature valid)."""
    try:
        nonce, sig = token.rsplit(".", 1)
        expected = hmac.new(
            settings.jwt_secret.encode(), msg=nonce.encode(), digestmod=hashlib.sha256
        ).hexdigest()
        return hmac.compare_digest(sig, expected)
    except Exception:
        return False
