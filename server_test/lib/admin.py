"""AdminClient: HTTP helper for T3 admin campaign integration tests."""
from __future__ import annotations

import httpx


class AdminClient:
    """Wraps an authenticated httpx.AsyncClient for admin API calls."""

    def __init__(self, client: httpx.AsyncClient, token: str) -> None:
        self._client = client
        self._token = token

    def _auth(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self._token}"}

    async def upload_firmware(self, data: bytes, filename: str = "firmware.bin") -> httpx.Response:
        return await self._client.post(
            "/admin/firmware/upload",
            headers=self._auth(),
            files={"file": (filename, data, "application/octet-stream")},
        )

    async def start_campaign(self, campaign_id: int, **kwargs) -> httpx.Response:
        return await self._client.post(
            f"/admin/campaign/{campaign_id}/start",
            headers=self._auth(),
            json=kwargs,
        )

    async def pause_campaign(self, campaign_id: int) -> httpx.Response:
        return await self._client.post(
            f"/admin/campaign/{campaign_id}/pause",
            headers=self._auth(),
        )

    async def resume_campaign(self, campaign_id: int) -> httpx.Response:
        return await self._client.post(
            f"/admin/campaign/{campaign_id}/resume",
            headers=self._auth(),
        )

    async def cancel_campaign(self, campaign_id: int) -> httpx.Response:
        return await self._client.post(
            f"/admin/campaign/{campaign_id}/cancel",
            headers=self._auth(),
        )

    async def get_campaign(self, campaign_id: int) -> httpx.Response:
        return await self._client.get(
            f"/admin/campaign/{campaign_id}",
            headers=self._auth(),
        )
