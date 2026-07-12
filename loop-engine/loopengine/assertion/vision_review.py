"""视觉复核器 - spec 4.4.3。

将客户端截图发送给 ARK 视觉模型（glm-5-2-260617）判定渲染状态。
仅在结构化断言部分失败（WARNING 失败，无 CRITICAL 失败）时触发。

判定结果分类：
  NORMAL     - 渲染正常
  BLACK_SCREEN - 黑屏
  WHITE_SCREEN - 白屏
  GARBLED    - 花屏/乱码
  NO_CONTENT - 窗口存在但无渲染内容
  ERROR      - 截图失败或模型响应异常
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from enum import Enum
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..ai.ark_client import ArkClient


class VisionVerdict(Enum):
    """视觉判定结论。"""

    NORMAL = "normal"          # 渲染正常
    BLACK_SCREEN = "black"     # 黑屏
    WHITE_SCREEN = "white"     # 白屏
    GARBLED = "garbled"        # 花屏/乱码
    NO_CONTENT = "no_content"  # 窗口存在但无渲染内容
    ERROR = "error"            # 截图失败或模型异常


@dataclass
class VisionReviewResult:
    """视觉复核结果。

    Attributes:
        verdict: 判定结论。
        confidence: 置信度（0.0 ~ 1.0）。
        description: 模型给出的详细描述。
        issues: 发现的问题列表。
        screenshot_path: 截图文件路径。
    """

    verdict: VisionVerdict
    confidence: float
    description: str
    issues: list[str] = field(default_factory=list)
    screenshot_path: str = ""


class VisionReviewer:
    """视觉复核器 - 将截图送视觉模型判定。

    调用 ArkClient.vision_chat 发送截图 + prompt，
    解析模型返回的 JSON 判定结果。
    """

    # 视觉判定 prompt 模板（spec 4.4.3）
    REVIEW_PROMPT = """你是一个游戏引擎渲染验证专家。
这是 ChaosEngine Vulkan 客户端的截图。请检查以下方面并给出判定：

1. 窗口内是否有渲染内容（不是纯黑/纯白）？
2. 是否能看到三角形或球体等 3D 渲染图元？
3. 画面是否有明显的花屏/撕裂/乱码？
4. 渲染内容是否在窗口区域内（没有溢出或错位）？

请以 JSON 格式返回：
{{
  "verdict": "normal" | "black" | "white" | "garbled" | "no_content",
  "confidence": 0.0-1.0,
  "description": "简要描述你看到的内容",
  "issues": ["问题1", "问题2"]
}}

当前上下文信息（结构化断言部分失败）：
- 失败的断言: {failed_assertions}
- 标题栏数据: {title_bar_text}
- 客户端日志摘要: {log_summary}
"""

    def __init__(self, ark_client: ArkClient) -> None:
        """初始化视觉复核器。

        Args:
            ark_client: ArkClient 实例，用于调用视觉模型 API。
        """
        self.ark_client = ark_client

    async def review(
        self,
        screenshot_path: str,
        failed_assertions: list[dict],
        title_bar_text: str,
        log_summary: str,
    ) -> VisionReviewResult:
        """送截图给视觉模型复核。

        Args:
            screenshot_path: 截图文件路径。
            failed_assertions: 失败断言列表（含 name/severity/actual_value 等）。
            title_bar_text: 标题栏原始文本。
            log_summary: 客户端日志摘要。

        Returns:
            VisionReviewResult: 视觉复核结果。
                模型响应解析失败时返回 ERROR verdict。
        """
        prompt = self.REVIEW_PROMPT.format(
            failed_assertions=failed_assertions,
            title_bar_text=title_bar_text,
            log_summary=log_summary,
        )

        # 调用视觉模型（ARK API，图片 + 文本输入）
        response = await self.ark_client.vision_chat(
            image_path=screenshot_path,
            prompt=prompt,
            model="glm-5-2-260617",  # 通用旗舰模型做视觉判定
        )

        # 解析 JSON 响应
        try:
            result = json.loads(response)
            return VisionReviewResult(
                verdict=VisionVerdict(result.get("verdict", "error")),
                confidence=float(result.get("confidence", 0.0)),
                description=result.get("description", ""),
                issues=result.get("issues", []),
                screenshot_path=screenshot_path,
            )
        except (json.JSONDecodeError, ValueError) as e:
            # JSON 解析失败或 verdict 值无效，返回 ERROR
            return VisionReviewResult(
                verdict=VisionVerdict.ERROR,
                confidence=0.0,
                description=f"视觉模型响应解析失败: {e}",
                issues=[],
                screenshot_path=screenshot_path,
            )
