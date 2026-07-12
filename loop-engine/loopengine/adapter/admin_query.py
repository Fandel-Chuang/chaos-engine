"""Admin API 查询器 - spec 6.5。

通过 HTTP 请求 ChaosEngine Admin 后台 API，查询引擎运行时状态。
Admin 后台由 Lapis 提供，默认监听 http://127.0.0.1:9090。
"""

from __future__ import annotations

import httpx


class AdminQuery:
    """ChaosEngine Admin API 查询器。

    封装对 Admin Web API 的异步 HTTP 请求，超时 5 秒。
    """

    def __init__(self, base_url: str = "http://127.0.0.1:9090") -> None:
        """初始化 Admin API 查询器。

        Args:
            base_url: Admin 后台基础 URL。
        """
        self.base_url = base_url.rstrip("/")
        self.timeout = 5.0

    async def get(self, path: str) -> dict:
        """发送 GET 请求到 Admin API。

        Args:
            path: API 路径（如 "/api/stats"）。

        Returns:
            dict: JSON 响应。请求失败时返回空 dict。
        """
        url = f"{self.base_url}{path}"
        async with httpx.AsyncClient(timeout=self.timeout) as client:
            try:
                resp = await client.get(url)
                if resp.status_code == 200:
                    return resp.json()
            except httpx.RequestError:
                pass
        return {}

    async def get_stats(self) -> dict:
        """查询统计信息（fps, uptime, entities 等）。

        GET /api/stats
        """
        return await self.get("/api/stats")

    async def get_aoi(self) -> dict:
        """查询 AOI 状态。

        GET /api/aoi
        """
        return await self.get("/api/aoi")

    async def get_cell(self) -> dict:
        """查询 Cell 网格状态。

        GET /api/cell
        """
        return await self.get("/api/cell")

    async def get_entities(self) -> dict:
        """查询实体列表。

        GET /api/entities
        """
        return await self.get("/api/entities")

    async def health_check(self) -> bool:
        """检查 Admin 后台是否存活。

        GET /api/health，返回 200 则存活。

        Returns:
            bool: Admin 是否存活。
        """
        url = f"{self.base_url}/api/health"
        async with httpx.AsyncClient(timeout=self.timeout) as client:
            try:
                resp = await client.get(url)
                return resp.status_code == 200
            except httpx.RequestError:
                return False
