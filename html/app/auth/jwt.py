"""JWT token creation/verification, bcrypt password check, RBAC dependency."""
from __future__ import annotations

import bcrypt
import jwt
from datetime import UTC, datetime, timedelta
from fastapi import Depends, HTTPException, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

from app.config import settings

_ALGORITHM = "HS256"
_LIFETIME_H = 24
_ROLE_LEVELS: dict[str, int] = {"viewer": 0, "operator": 1, "admin": 2}

_bearer = HTTPBearer(auto_error=False)


def create_token(sub: str, role: str) -> str:
    payload = {
        "sub": sub,
        "role": role,
        "exp": datetime.now(UTC) + timedelta(hours=_LIFETIME_H),
    }
    return jwt.encode(payload, settings.jwt_secret, algorithm=_ALGORITHM)


def verify_token(token: str) -> dict:
    try:
        return jwt.decode(token, settings.jwt_secret, algorithms=[_ALGORITHM])
    except jwt.ExpiredSignatureError:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="Token expired")
    except jwt.InvalidTokenError:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="Invalid token")


def check_password(plain: str, hashed: str) -> bool:
    return bcrypt.checkpw(plain.encode(), hashed.encode())


def get_current_user(
    creds: HTTPAuthorizationCredentials | None = Depends(_bearer),
) -> dict:
    if creds is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, detail="Missing token")
    return verify_token(creds.credentials)


def require_role(min_role: str):
    """Dependency factory — 401 on missing/invalid token, 403 on insufficient role."""
    min_level = _ROLE_LEVELS[min_role]

    def _dep(user: dict = Depends(get_current_user)) -> dict:
        if _ROLE_LEVELS.get(user.get("role", ""), -1) < min_level:
            raise HTTPException(status.HTTP_403_FORBIDDEN, detail="Insufficient role")
        return user

    return _dep
