"""任务拆解器 - spec 5.2。

将复杂任务拆解为可独立执行的子任务列表。
调用 AI（glm-5-2）分析任务，为每个子任务标注 task_type、complexity 和依赖关系。

子任务的 task_type 对应 ModelRouter.ROUTING_TABLE 中的任务类型，
供后续的子 agent 编排器按依赖关系并行执行。
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .ark_client import ArkClient
    from .model_router import ModelRouter


@dataclass
class SubTask:
    """子任务。

    Attributes:
        id: 子任务序号（从 0 开始）。
        description: 子任务描述。
        task_type: 任务类型（对应 ModelRouter 的 task_type，如 "code_generation"）。
        complexity: 复杂度标签（SIMPLE / MODERATE / COMPLEX）。
        dependencies: 依赖的子任务 id 列表。
        status: 执行状态（pending / running / done / failed）。
    """

    id: int
    description: str
    task_type: str
    complexity: str = "MODERATE"
    dependencies: list[int] = field(default_factory=list)
    status: str = "pending"


class TaskSplitter:
    """任务拆解器。

    调用 AI（glm-5-2）将复杂任务拆解为子任务列表，
    每个子任务标注 task_type 和 complexity，并识别依赖关系。

    Usage:
        splitter = TaskSplitter(ark_client, model_router)
        subtasks = await splitter.split("为 ChaosEngine 添加 AOI 范围查询功能")
    """

    # 任务拆解 prompt 模板
    SPLIT_PROMPT = """你是一个游戏引擎开发任务拆解专家。
请将以下任务拆解为可独立执行的子任务列表。

任务描述: {task_description}

上下文:
- 项目: ChaosEngine（纯C内核 + Lua脚本的游戏引擎）
- 相关 spec: {spec_path}

要求:
1. 每个子任务可以在一个 AI agent session 内完成
2. 标注每个子任务的类型，从以下选项中选择:
   - code_generation: 代码生成（新功能实现）
   - simple_edit: 简单修改（单行/局部修改）
   - compile_error_analysis: 编译错误分析
   - failure_analysis: 测试失败分析
   - lua_lint_fix: Lua 语法修复
   - requirement_analysis: 需求分析
   - cluster_startup_analysis: 集群启动分析
   - data_consistency: 数据一致性验证
   - api_validation: API 校验
   - render_error_analysis: 渲染错误分析
3. 标注每个子任务的复杂度: simple / moderate / complex
4. 标注子任务之间的依赖关系（用序号引用）
5. 子任务粒度适中（不过细也不过粗）

以 JSON 数组格式返回:
[
  {{"description": "...", "task_type": "...", "complexity": "...", "depends_on": [0, 2]}}
]
"""

    def __init__(self, ark_client: ArkClient, model_router: ModelRouter) -> None:
        """初始化任务拆解器。

        Args:
            ark_client: ArkClient 实例，用于调用 AI 模型。
            model_router: ModelRouter 实例，用于查询路由规则。
        """
        self.ark_client = ark_client
        self.model_router = model_router

    async def split(
        self,
        task_description: str,
        spec_path: str | None = None,
    ) -> list[SubTask]:
        """拆解任务为子任务列表。

        调用 AI（glm-5-2）分析任务，拆解为子任务。
        AI 返回 JSON 数组，每个元素含 description / task_type / complexity / depends_on。

        Args:
            task_description: 任务描述文本。
            spec_path: 相关 spec 文件路径（可选）。

        Returns:
            list[SubTask]: 子任务列表。AI 解析失败时降级为单任务。

        Example:
            >>> subtasks = await splitter.split("实现 AOI 范围查询")
            >>> [t.description for t in subtasks]
            ['修改 ce_aoi.h 添加声明', '实现查询函数', '添加测试用例']
        """
        spec_display = spec_path or "N/A"

        prompt = self.SPLIT_PROMPT.format(
            task_description=task_description,
            spec_path=spec_display,
        )

        # 调用 AI 拆解任务（使用 requirement_analysis 路由 -> glm）
        response = await self.ark_client.chat(
            model=self.model_router.get_model_id("glm"),
            messages=[{"role": "user", "content": prompt}],
            max_tokens=4096,
            temperature=0.3,
        )

        return self._parse_subtasks(response)

    async def analyze_spec(self, spec_path: str) -> dict:
        """读取 spec 文件，提取验收标准。

        Args:
            spec_path: spec 文件路径（Markdown 格式）。

        Returns:
            dict: 验收标准信息，格式为:
                {
                    "path": str,
                    "acceptance_criteria": list[str],
                    "line_count": int,
                }
            文件不存在时返回空验收标准。
        """
        path = Path(spec_path)
        if not path.exists():
            return {
                "path": spec_path,
                "acceptance_criteria": [],
                "line_count": 0,
            }

        try:
            content = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return {
                "path": spec_path,
                "acceptance_criteria": [],
                "line_count": 0,
            }

        lines = content.splitlines()

        # 提取验收标准：匹配 "验收标准" / "Acceptance Criteria" 标题后的列表项
        criteria: list[str] = []
        in_criteria_section = False

        for line in lines:
            stripped = line.strip()

            # 检测验收标准章节标题
            if (
                "验收标准" in stripped
                or "acceptance criteria" in stripped.lower()
            ) and stripped.startswith("#"):
                in_criteria_section = True
                continue

            # 检测下一个章节标题，退出验收标准章节
            if in_criteria_section and stripped.startswith("#"):
                in_criteria_section = False
                continue

            # 收集列表项
            if in_criteria_section and stripped.startswith(("-", "*", "+")):
                criteria.append(stripped.lstrip("-*+ ").strip())

        return {
            "path": spec_path,
            "acceptance_criteria": criteria,
            "line_count": len(lines),
        }

    def _parse_subtasks(self, ai_response: str) -> list[SubTask]:
        """解析 AI 返回的 JSON 子任务列表。

        Args:
            ai_response: AI 返回的文本（应为 JSON 数组格式）。

        Returns:
            list[SubTask]: 解析出的子任务列表。
                JSON 解析失败时降级为单个 code_generation 子任务。
        """
        # 尝试从响应中提取 JSON 数组（AI 可能在 JSON 前后附加文字）
        json_str = self._extract_json_array(ai_response)

        try:
            tasks_data = json.loads(json_str)
        except (json.JSONDecodeError, ValueError):
            # 降级：返回单个子任务
            return [
                SubTask(
                    id=0,
                    description="执行原始任务",
                    task_type="code_generation",
                    complexity="COMPLEX",
                    dependencies=[],
                    status="pending",
                )
            ]

        subtasks: list[SubTask] = []
        for idx, t in enumerate(tasks_data):
            if not isinstance(t, dict):
                continue

            description = t.get("description", "").strip()
            if not description:
                continue

            task_type = t.get("task_type", "code_generation").strip()
            complexity = t.get("complexity", "moderate").strip().upper()
            depends_on = t.get("depends_on", [])

            # 确保 depends_on 是 int 列表
            if not isinstance(depends_on, list):
                depends_on = []

            subtasks.append(
                SubTask(
                    id=idx,
                    description=description,
                    task_type=task_type,
                    complexity=complexity,
                    dependencies=[int(d) for d in depends_on if isinstance(d, (int, float))],
                    status="pending",
                )
            )

        # 如果解析结果为空，降级为单任务
        if not subtasks:
            subtasks.append(
                SubTask(
                    id=0,
                    description="执行原始任务",
                    task_type="code_generation",
                    complexity="COMPLEX",
                    dependencies=[],
                    status="pending",
                )
            )

        return subtasks

    @staticmethod
    def _extract_json_array(text: str) -> str:
        """从文本中提取 JSON 数组片段。

        AI 返回的内容可能包含 markdown 代码块标记或额外说明文字，
        此函数定位第一个 ``[`` 到最后一个 ``]`` 的内容。

        Args:
            text: AI 返回的原始文本。

        Returns:
            str: 提取出的 JSON 数组字符串。未找到时返回原文本。
        """
        # 优先匹配 markdown 代码块中的 JSON
        if "```" in text:
            lines = text.splitlines()
            in_block = False
            block_lines: list[str] = []
            for line in lines:
                if line.strip().startswith("```"):
                    if in_block:
                        break  # 代码块结束
                    in_block = True
                    continue
                if in_block:
                    block_lines.append(line)
            if block_lines:
                candidate = "\n".join(block_lines).strip()
                if candidate.startswith("["):
                    return candidate

        # 回退：找第一个 [ 到最后一个 ]
        first_bracket = text.find("[")
        last_bracket = text.rfind("]")
        if first_bracket != -1 and last_bracket != -1 and last_bracket > first_bracket:
            return text[first_bracket : last_bracket + 1]

        return text
