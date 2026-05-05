"""mTLS client-certificate verification dependency."""
from __future__ import annotations

from fastapi import Header, HTTPException


async def mtls_required(
    x_ssl_client_verify: str | None = Header(default=None, alias="X-SSL-Client-Verify"),
) -> None:
    """Reject the request unless Nginx has confirmed a valid client certificate."""
    if x_ssl_client_verify != "SUCCESS":
        raise HTTPException(status_code=403, detail="Client certificate required")
