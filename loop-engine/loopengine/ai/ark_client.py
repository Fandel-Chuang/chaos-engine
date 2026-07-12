"""ARK API 客户端 - spec 5.4。

封装火山方舟 ARK API 的文本对话、视觉对话、流式对话接口。
兼容 OpenAI Chat Completions 格式。

内部使用 httpx.AsyncClient（超时 60s），复用连接池。
统一错误处理：HTTPError / Timeout / JSONDecodeError 返回有意义的信息。
成本统计：记录每次调用的 input_tokens / output_tokens / model，供 ModelRouter 汇总。
"""

from __future__ import annotations

import base64
import json
import time
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path
from typing import AsyncGenerator, Optional

import httpx


@dataclass
class UsageRecord:
    """单次 API 调用用量记录。"""

    model: str
    input_tokens: int
    output_tokens: int
    timestamp: float
    cost_cny: float = 0.0


@dataclass
class CostTracker:
    """成本统计器 - 按模型/按日汇总 token 用量和成本。

    供 ModelRouter 查询当日用量和预算检查。
    """

    # 按日期分组的用量记录：{date_str: [UsageRecord, ...]}
    _records: dict[str, list[UsageRecord]] = field(default_factory=dict)

    def record(
        self,
        model: str,
        input_tokens: int,
        output_tokens: int,
        cost_cny: float = 0.0,
    ) -> None:
        """记录一次 API 调用的用量。

        Args:
            model: 模型 ID。
            input_tokens: 输入 token 数。
            output_tokens: 输出 token 数。
            cost_cny: 本次调用成本（元）。
        """
        today = date.today().isoformat()
        if today not in self._records:
            self._records[today] = []
        self._records[today].append(
            UsageRecord(
                model=model,
                input_tokens=input_tokens,
                output_tokens=output_tokens,
                timestamp=time.time(),
                cost_cny=cost_cny,
            )
        )

    def get_daily_usage(self) -> dict:
        """获取今日各模型用量和成本汇总。

        Returns:
            dict: {model_id: {calls, input_tokens, output_tokens, cost_cny}}。
        """
        today = date.today().isoformat()
        records = self._records.get(today, [])

        summary: dict[str, dict] = {}
        for r in records:
            if r.model not in summary:
                summary[r.model] = {
                    "calls": 0,
                    "input_tokens": 0,
                    "output_tokens": 0,
                    "cost_cny": 0.0,
                }
            summary[r.model]["calls"] += 1
            summary[r.model]["input_tokens"] += r.input_tokens
            summary[r.model]["output_tokens"] += r.output_tokens
            summary[r.model]["cost_cny"] += r.cost_cny

        return summary

    def get_daily_cost(self) -> float:
        """获取今日总成本（元）。"""
        today = date.today().isoformat()
        return sum(r.cost_cny for r in self._records.get(today, []))


class ArkClient:
    """ARK API 客户端。

    封装火山方舟模型的文本对话、视觉对话、流式对话接口。
    内部复用 httpx.AsyncClient 连接池，超时 60 秒。
    自动统计每次调用的 token 用量。

    Usage:
        client = ArkClient(api_key="your_key")
        response = await client.chat(model="glm-5-2-260617", messages=[...])
        await client.close()
    """

    def __init__(
        self,
        api_key: str,
        base_url: str = "https://ark.cn-beijing.volces.com/api/coding/v3",
    ) -> None:
        """初始化 ARK 客户端。

        Args:
            api_key: ARK API 密钥。
            base_url: ARK API 基础 URL（含 /api/coding/v3 路径）。
        """
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self._client: Optional[httpx.AsyncClient] = None
        self.cost_tracker = CostTracker()

    async def _get_client(self) -> httpx.AsyncClient:
        """获取或创建 httpx.AsyncClient（复用连接池）。"""
        if self._client is None or self._client.is_closed:
            self._client = httpx.AsyncClient(
                base_url=self.base_url,
                headers={
                    "Authorization": f"Bearer {self.api_key}",
                    "Content-Type": "application/json",
                },
                timeout=60.0,
            )
        return self._client

    async def chat(
        self,
        model: str,
        messages: list[dict],
        max_tokens: int = 4096,
        temperature: float = 0.7,
    ) -> str:
        """文本对话。

        Args:
            model: 模型 ID（如 "glm-5-2-260617"）。
            messages: OpenAI 格式消息列表。
            max_tokens: 最大输出 token 数。
            temperature: 采样温度。

        Returns:
            str: 模型回复文本。

        Raises:
            RuntimeError: HTTP 错误 / 超时 / JSON 解析失败时，
                          抛出含上下文信息的 RuntimeError。
        """
        client = await self._get_client()

        try:
            resp = await client.post(
                "/chat/completions",
                json={
                    "model": model,
                    "messages": messages,
                    "max_tokens": max_tokens,
                    "temperature": temperature,
                },
            )
            resp.raise_for_status()
        except httpx.TimeoutException:
            raise RuntimeError(
                f"ARK API 请求超时 (60s): model={model}, "
                f"messages_count={len(messages)}"
            )
        except httpx.HTTPStatusError as e:
            raise RuntimeError(
                f"ARK API HTTP 错误 ({e.response.status_code}): "
                f"model={model}, body={e.response.text[:500]}"
            )
        except httpx.RequestError as e:
            raise RuntimeError(
                f"ARK API 网络错误: model={model}, error={e}"
            )

        try:
            data = resp.json()
        except (json.JSONDecodeError, ValueError) as e:
            raise RuntimeError(
                f"ARK API 响应 JSON 解析失败: model={model}, error={e}"
            )

        # 统计 token 用量
        usage = data.get("usage", {})
        self.cost_tracker.record(
            model=model,
            input_tokens=usage.get("prompt_tokens", 0),
            output_tokens=usage.get("completion_tokens", 0),
        )

        return data["choices"][0]["message"]["content"]

    async def vision_chat(
        self,
        image_path: str,
        prompt: str,
        model: str = "glm-5-2-260617",
    ) -> str:
        """视觉对话（图片 + 文本）。

        读取图片为 base64，构造 OpenAI Vision 格式 messages，
        POST /chat/completions，image_url 用 data:image/png;base64,...。

        Args:
            image_path: 图片文件路径。
            prompt: 文本 prompt。
            model: 视觉模型 ID（默认 glm-5-2-260617）。

        Returns:
            str: 模型回复文本。

        Raises:
            RuntimeError: 图片读取失败 / HTTP 错误 / 超时 / JSON 解析失败。
            FileNotFoundError: 图片文件不存在。
        """
        # 读取图片为 base64
        path = Path(image_path)
        if not path.exists():
            raise FileNotFoundError(f"图片文件不存在: {image_path}")

        try:
            image_bytes = path.read_bytes()
        except OSError as e:
            raise RuntimeError(f"图片读取失败: {image_path}, error={e}")

        image_b64 = base64.b64encode(image_bytes).decode("utf-8")

        # 推断 MIME 类型（默认 png）
        suffix = path.suffix.lower()
        mime_map = {
            ".png": "image/png",
            ".jpg": "image/jpeg",
            ".jpeg": "image/jpeg",
            ".gif": "image/gif",
            ".webp": "image/webp",
        }
        mime_type = mime_map.get(suffix, "image/png")

        client = await self._get_client()

        try:
            resp = await client.post(
                "/chat/completions",
                json={
                    "model": model,
                    "messages": [
                        {
                            "role": "user",
                            "content": [
                                {"type": "text", "text": prompt},
                                {
                                    "type": "image_url",
                                    "image_url": {
                                        "url": f"data:{mime_type};base64,{image_b64}"
                                    },
                                },
                            ],
                        }
                    ],
                    "max_tokens": 500,
                    "temperature": 0.1,  # 低温度保证判定稳定性
                },
            )
            resp.raise_for_status()
        except httpx.TimeoutException:
            raise RuntimeError(
                f"ARK 视觉 API 请求超时 (60s): model={model}, "
                f"image={image_path}"
            )
        except httpx.HTTPStatusError as e:
            raise RuntimeError(
                f"ARK 视觉 API HTTP 错误 ({e.response.status_code}): "
                f"model={model}, body={e.response.text[:500]}"
            )
        except httpx.RequestError as e:
            raise RuntimeError(
                f"ARK 视觉 API 网络错误: model={model}, error={e}"
            )

        try:
            data = resp.json()
        except (json.JSONDecodeError, ValueError) as e:
            raise RuntimeError(
                f"ARK 视觉 API 响应 JSON 解析失败: model={model}, error={e}"
            )

        # 统计 token 用量
        usage = data.get("usage", {})
        self.cost_tracker.record(
            model=model,
            input_tokens=usage.get("prompt_tokens", 0),
            output_tokens=usage.get("completion_tokens", 0),
        )

        return data["choices"][0]["message"]["content"]

    async def stream_chat(
        self,
        model: str,
        messages: list[dict],
        max_tokens: int = 4096,
        temperature: float = 0.7,
    ) -> AsyncGenerator[str, None]:
        """流式对话（逐 token 返回）。

        Args:
            model: 模型 ID。
            messages: OpenAI 格式消息列表。
            max_tokens: 最大输出 token 数。
            temperature: 采样温度。

        Yields:
            str: 逐段返回的模型回复文本。

        Raises:
            RuntimeError: HTTP 错误 / 超时。
        """
        client = await self._get_client()

        try:
            async with client.stream(
                "POST",
                "/chat/completions",
                json={
                    "model": model,
                    "messages": messages,
                    "max_tokens": max_tokens,
                    "temperature": temperature,
                    "stream": True,
                },
            ) as resp:
                resp.raise_for_status()
                async for line in resp.aiter_lines():
                    if not line.startswith("data: "):
                        continue
                    data_str = line[6:]
                    if data_str.strip() == "[DONE]":
                        break
                    try:
                        chunk = json.loads(data_str)
                        delta = chunk["choices"][0].get("delta", {})
                        content = delta.get("content")
                        if content:
                            yield content
                    except (json.JSONDecodeError, KeyError, IndexError):
                        continue
        except httpx.TimeoutException:
            raise RuntimeError(
                f"ARK 流式 API 请求超时 (60s): model={model}"
            )
        except httpx.HTTPStatusError as e:
            raise RuntimeError(
                f"ARK 流式 API HTTP 错误 ({e.response.status_code}): "
                f"model={model}"
            )
        except httpx.RequestError as e:
            raise RuntimeError(
                f"ARK 流式 API 网络错误: model={model}, error={e}"
            )

    async def close(self) -> None:
        """关闭底层 httpx 客户端，释放连接池资源。"""
        if self._client and not self._client.is_closed:
            await self._client.aclose()
