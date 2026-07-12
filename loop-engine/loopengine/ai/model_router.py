"""模型路由器 - spec 5.1.2 / 5.1.3 / config loop.yaml。

根据任务类型（task_type）路由到合适的模型，
按 config/models.yaml 的价格估算成本，并做预算检查。

路由表（12 种任务类型，与 config/loop.yaml model_routing 一致）：
  simple_edit / lua_lint_fix / title_bar_analysis / api_validation  -> flash
  requirement_analysis / compile_error_analysis / failure_analysis /
    cluster_startup_analysis / vision_review                        -> glm
  code_generation / data_consistency / render_error_analysis        -> pro
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .ark_client import ArkClient


class TaskComplexity(Enum):
    """任务复杂度分级。"""

    SIMPLE = "simple"        # 简单编辑、lint 修复、标题栏分析
    MODERATE = "moderate"    # 需求分析、编译错误分析、失败分析、集群分析、API 校验、视觉复核
    COMPLEX = "complex"      # 代码生成、数据一致性、渲染错误分析


@dataclass
class RoutingRule:
    """路由规则。

    Attributes:
        complexity: 任务复杂度。
        model_alias: 主模型别名（flash / glm / pro / qwen）。
        fallback_alias: 降级模型别名。
        max_tokens: 最大输出 token 数。
        temperature: 采样温度。
    """

    complexity: TaskComplexity
    model_alias: str
    fallback_alias: str
    max_tokens: int
    temperature: float


class ModelRouter:
    """模型路由器。

    根据任务类型路由到合适的模型，支持成本估算和预算检查。

    路由表与 config/loop.yaml model_routing 配置一致：
        simple_edit / lua_lint_fix / title_bar_analysis / api_validation -> flash
        requirement_analysis / compile_error_analysis / failure_analysis /
          cluster_startup_analysis / vision_review                      -> glm
        code_generation / data_consistency / render_error_analysis      -> pro
    """

    # 路由表：task_type -> RoutingRule
    ROUTING_TABLE: dict[str, RoutingRule] = {
        # ── SIMPLE（flash） ──
        "simple_edit": RoutingRule(
            TaskComplexity.SIMPLE, "flash", "glm", 2048, 0.1
        ),
        "lua_lint_fix": RoutingRule(
            TaskComplexity.SIMPLE, "flash", "glm", 1024, 0.1
        ),
        "title_bar_analysis": RoutingRule(
            TaskComplexity.SIMPLE, "flash", "glm", 1024, 0.1
        ),
        "api_validation": RoutingRule(
            TaskComplexity.SIMPLE, "flash", "glm", 2048, 0.1
        ),
        # ── MODERATE（glm） ──
        "requirement_analysis": RoutingRule(
            TaskComplexity.MODERATE, "glm", "pro", 4096, 0.2
        ),
        "compile_error_analysis": RoutingRule(
            TaskComplexity.MODERATE, "glm", "pro", 4096, 0.2
        ),
        "failure_analysis": RoutingRule(
            TaskComplexity.MODERATE, "glm", "pro", 4096, 0.2
        ),
        "cluster_startup_analysis": RoutingRule(
            TaskComplexity.MODERATE, "glm", "pro", 4096, 0.2
        ),
        "vision_review": RoutingRule(
            TaskComplexity.MODERATE, "glm", "pro", 1024, 0.1
        ),
        # ── COMPLEX（pro） ──
        "code_generation": RoutingRule(
            TaskComplexity.COMPLEX, "pro", "glm", 8192, 0.2
        ),
        "data_consistency": RoutingRule(
            TaskComplexity.COMPLEX, "pro", "glm", 4096, 0.2
        ),
        "render_error_analysis": RoutingRule(
            TaskComplexity.COMPLEX, "pro", "glm", 4096, 0.2
        ),
    }

    # 默认路由规则（task_type 未命中时使用）
    DEFAULT_RULE = RoutingRule(
        TaskComplexity.MODERATE, "glm", "pro", 4096, 0.2
    )

    def __init__(
        self,
        models_config: dict,
        ark_client: ArkClient | None = None,
    ) -> None:
        """初始化模型路由器。

        Args:
            models_config: 从 config/models.yaml 加载的模型配置，
                格式为 {"models": {"flash": {"id": ..., "input_price": ..., "output_price": ...}, ...}}。
            ark_client: ArkClient 实例（可选，用于查询已记录的用量）。
        """
        self.models_config = models_config or {}
        self.ark_client = ark_client

        # 预解析模型信息：alias -> {id, input_price, output_price}
        self._models: dict[str, dict] = {}
        raw_models = self.models_config.get("models", self.models_config)
        for alias, info in raw_models.items():
            self._models[alias] = {
                "id": info.get("id", alias),
                "input_price": info.get("input_price") or 0.0,
                "output_price": info.get("output_price") or 0.0,
            }

    def resolve(self, task_type: str) -> RoutingRule:
        """根据任务类型解析路由规则。

        Args:
            task_type: 任务类型（如 "code_generation"、"vision_review"）。

        Returns:
            RoutingRule: 路由规则。未命中时返回默认规则（glm）。
        """
        return self.ROUTING_TABLE.get(task_type, self.DEFAULT_RULE)

    def get_model_id(self, alias: str) -> str:
        """模型别名 -> 实际模型 ID。

        Args:
            alias: 模型别名（flash / glm / pro / qwen）。

        Returns:
            str: 实际模型 ID（如 "glm-5-2-260617"）。
                 未知别名时原样返回。
        """
        if alias in self._models:
            return self._models[alias]["id"]
        return alias

    def estimate_cost(
        self,
        model_alias: str,
        input_tokens: int,
        output_tokens: int,
    ) -> float:
        """估算单次调用的成本（元）。

        价格单位：元/百万 tokens（与 config/models.yaml 一致）。

        Args:
            model_alias: 模型别名。
            input_tokens: 输入 token 数。
            output_tokens: 输出 token 数。

        Returns:
            float: 成本（元）。未知模型按 glm 价格估算。
        """
        model_info = self._models.get(model_alias)
        if model_info is None:
            # 未知模型，按 glm 价格兜底
            model_info = self._models.get(
                "glm", {"input_price": 8.0, "output_price": 28.0}
            )

        in_price = model_info.get("input_price", 0.0) or 0.0
        out_price = model_info.get("output_price", 0.0) or 0.0

        return (
            input_tokens / 1_000_000 * in_price
            + output_tokens / 1_000_000 * out_price
        )

    def get_daily_usage(self) -> dict:
        """获取今日各模型用量和成本汇总。

        从 ArkClient 的 CostTracker 获取数据。

        Returns:
            dict: {model_id: {calls, input_tokens, output_tokens, cost_cny}}。
                  无 ark_client 时返回空 dict。
        """
        if self.ark_client is None:
            return {}
        return self.ark_client.cost_tracker.get_daily_usage()

    def get_daily_cost(self) -> float:
        """获取今日总成本（元）。

        Returns:
            float: 今日总成本。无 ark_client 时返回 0.0。
        """
        if self.ark_client is None:
            return 0.0
        return self.ark_client.cost_tracker.get_daily_cost()

    def check_budget(
        self,
        daily_budget: float = 10.0,
        per_loop_budget: float = 2.0,
    ) -> bool:
        """检查是否超出预算。

        Args:
            daily_budget: 每日预算上限（元），默认 10.0。
            per_loop_budget: 单次闭环预算上限（元），默认 2.0。

        Returns:
            bool: True 表示在预算内（可以继续消费），False 表示已超预算。
        """
        daily_cost = self.get_daily_cost()
        if daily_cost >= daily_budget:
            return False
        # per_loop_budget 是单次闭环预算，此处仅检查每日总量
        # 单次闭环预算由调用方在闭环开始时检查
        return True
