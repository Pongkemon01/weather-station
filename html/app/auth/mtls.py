"""mTLS client-certificate verification dependency."""
from __future__ import annotations

from fastapi import Header, HTTPException

from app.metrics import cert_verify_failures_total


async def mtls_required(
    x_ssl_client_verify: str | None = Header(default=None, alias="X-SSL-Client-Verify"),
) -> None:
    """Reject the request unless Nginx has confirmed a valid client certificate."""
    if x_ssl_client_verify is None:
        cert_verify_failures_total.labels(reason="missing").inc()
        raise HTTPException(status_code=403, detail="Client certificate required")
    if x_ssl_client_verify != "SUCCESS":
        cert_verify_failures_total.labels(reason="failed").inc()
        raise HTTPException(status_code=403, detail="Client certificate required")
