"""LoopEngine - 混沌引擎自动化闭环验证工具。

公共 API 导出：LoopConfig / LoopEngine / 数据模型。
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any

import yaml

from .data.models import (
    AssertionResult,
    AssertionSeverity,
    LoopContext,
    LoopExecution,
    LoopResult,
    LoopStatus,
)

# 环境变量替换正则：${VAR_NAME}
_ENV_PATTERN = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def _expand_env(value: Any) -> Any:
    """递归替换字符串中的 ${ENV_VAR} 为环境变量值。

    Args:
        value: 任意值（dict / list / str / 其他）。

    Returns:
        替换后的值。${VAR} 未找到时替换为空字符串。
    """
    if isinstance(value, str):
        return _ENV_PATTERN.sub(
            lambda m: os.environ.get(m.group(1), ""), value
        )
    if isinstance(value, dict):
        return {k: _expand_env(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand_env(item) for item in value]
    return value


@dataclass
class LoopConfig:
    """闭环引擎配置。

    从 config/loop.yaml 加载，环境变量 ${ARK_API_KEY} / ${GITEE_API_TOKEN} 自动替换。
    """

    chaos_engine_dir: str = "/home/zhongfangdao/chaos-engine"
    build_dir: str = "build"
    ark_api_key: str = ""
    ark_base_url: str = "https://ark.cn-beijing.volces.com/api/coding/v3"
    gitee_token: str = ""
    model_routing: dict[str, str] = field(default_factory=dict)
    vision_mode: str = "ark"
    vision_model: str = "glm-5-2-260617"
    cost_budget_daily: float = 10.0
    cost_per_loop: float = 2.0
    max_retries: int = 3
    report_format: str = "markdown"
    trigger_branch: str = "develop"
    gitee_repo: str = "zhong-fangdao/chaos-engine"
    models_config: dict[str, Any] = field(default_factory=dict)
    raw_config: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_yaml(cls, path: str) -> LoopConfig:
        """从 loop.yaml 加载配置。

        读取 YAML 文件，替换 ${ENV_VAR} 为环境变量值，
        映射到 LoopConfig 字段。

        Args:
            path: loop.yaml 文件路径。

        Returns:
            LoopConfig: 配置实例。
        """
        with open(path, "r", encoding="utf-8") as f:
            raw = yaml.safe_load(f) or {}

        # 环境变量替换
        data = _expand_env(raw)

        # 提取各段
        chaos_cfg = data.get("chaos_engine", {})
        ark_cfg = data.get("ark", {})
        routing = data.get("model_routing", {})
        vision_cfg = data.get("vision", {})
        cost_cfg = data.get("cost", {})
        loops_cfg = data.get("loops", {})
        ci_cfg = data.get("ci", {})

        # 尝试加载 models.yaml（与 loop.yaml 同目录）
        models_config: dict[str, Any] = {}
        models_path = os.path.join(os.path.dirname(path), "models.yaml")
        if os.path.exists(models_path):
            try:
                with open(models_path, "r", encoding="utf-8") as f:
                    models_config = _expand_env(yaml.safe_load(f) or {})
            except Exception:
                models_config = {}

        # dev 闭环的 max_retries（全局默认）
        dev_retries = loops_cfg.get("dev", {}).get("max_retries", 3)

        return cls(
            chaos_engine_dir=chaos_cfg.get("dir", "/home/zhongfangdao/chaos-engine"),
            build_dir=chaos_cfg.get("build_dir", "build"),
            ark_api_key=ark_cfg.get("api_key", ""),
            ark_base_url=ark_cfg.get("base_url", "https://ark.cn-beijing.volces.com/api/coding/v3"),
            gitee_token=ci_cfg.get("gitee_token", ""),
            model_routing=routing if isinstance(routing, dict) else {},
            vision_mode=vision_cfg.get("mode", "ark"),
            vision_model=vision_cfg.get("model", "glm-5-2-260617"),
            cost_budget_daily=cost_cfg.get("daily_budget_cny", 10.0),
            cost_per_loop=cost_cfg.get("per_loop_budget_cny", 2.0),
            max_retries=dev_retries,
            report_format="markdown",
            trigger_branch=ci_cfg.get("trigger_branch", "develop"),
            gitee_repo=ci_cfg.get("repo", "zhong-fangdao/chaos-engine"),
            models_config=models_config,
            raw_config=data,
        )


class LoopEngine:
    """闭环引擎主类 - 高层 API，封装完整四域闭环。

    用法::

        config = LoopConfig.from_yaml("config/loop.yaml")
        engine = LoopEngine(config)
        execution = asyncio.run(engine.run_full_loop("修复 AOI 同步 bug"))
    """

    def __init__(self, config: LoopConfig | None = None) -> None:
        """初始化闭环引擎。

        Args:
            config: 配置实例。为 None 时使用默认配置。
        """
        self.config = config or LoopConfig()
        self._components: dict[str, Any] | None = None

    def _ensure_components(self) -> dict[str, Any]:
        """惰性初始化所有组件（首次调用时创建）。"""
        if self._components is not None:
            return self._components

        from .adapter.admin_query import AdminQuery
        from .adapter.capture import ScreenshotCapture
        from .adapter.ci_trigger import CITrigger
        from .adapter.proc_manager import ProcessManager
        from .adapter.script_exec import ScriptExec
        from .ai.ark_client import ArkClient
        from .ai.model_router import ModelRouter
        from .assertion.structured import ClientAssertionEngine
        from .assertion.title_bar_parser import ClientDataCollector
        from .assertion.vision_review import VisionReviewer

        cfg = self.config

        # 适配层组件
        script_exec = ScriptExec(cfg.chaos_engine_dir)
        proc_manager = ProcessManager(cfg.chaos_engine_dir)
        ci_trigger = CITrigger(gitee_token=cfg.gitee_token, repo=cfg.gitee_repo)
        admin_query = AdminQuery()
        screenshot = ScreenshotCapture()

        # AI 层组件
        ark_client = ArkClient(api_key=cfg.ark_api_key, base_url=cfg.ark_base_url)
        model_router = ModelRouter(
            models_config=cfg.models_config, ark_client=ark_client
        )

        # 断言层组件
        # ClientDataCollector 需要 PID，在 client_ui 闭环执行时动态创建
        assertion_engine = ClientAssertionEngine()
        vision_reviewer = VisionReviewer(ark_client=ark_client)

        self._components = {
            "script_exec": script_exec,
            "proc_manager": proc_manager,
            "ci_trigger": ci_trigger,
            "admin_query": admin_query,
            "screenshot": screenshot,
            "ark_client": ark_client,
            "model_router": model_router,
            "assertion_engine": assertion_engine,
            "vision_reviewer": vision_reviewer,
        }
        return self._components

    def _make_context(self, loop_id: str = "") -> LoopContext:
        """构造闭环执行上下文。"""
        return LoopContext(
            chaos_engine_dir=self.config.chaos_engine_dir,
            build_dir=self.config.build_dir,
            config=self.config.raw_config,
            loop_id=loop_id,
        )

    async def run_full_loop(
        self, task: str = "", report_path: str | None = None
    ) -> LoopExecution:
        """执行完整四域闭环（dev -> test -> verify -> client_ui）。

        串行执行四个域，任一域 FAILED 则停止后续域。
        收集所有结果，存 SQLite，生成报告。

        Args:
            task: 任务描述（用于记录）。
            report_path: 报告输出路径（如 ``report.md``）。
                为 None 时不生成报告文件；有值时生成 Markdown + JSON 报告，
                并设置 ``execution.report_path``。

        Returns:
            LoopExecution: 完整执行记录。
        """
        from .data.db import init_db, save_execution
        from .orchestrator.loop_state import LoopStateMachine
        from .orchestrator.dev_loop import DevLoop
        from .orchestrator.test_loop import TestLoop
        from .orchestrator.verify_loop import VerifyLoop

        comps = self._ensure_components()
        loop_id = f"loop_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
        context = self._make_context(loop_id)

        execution = LoopExecution(
            loop_id=loop_id,
            task=task,
            started_at=datetime.now(),
            status=LoopStatus.RUNNING,
        )

        # 初始化数据库
        await init_db()

        # 存储各域的 LoopResult（供报告使用）
        domain_results: list[tuple[str, Any]] = []

        try:
            # ── 构造四域 ──
            domains: list[tuple[str, Any]] = []

            dev_loop = DevLoop(script_exec=comps["script_exec"])
            domains.append(("dev", dev_loop))

            test_loop = TestLoop(
                ci_trigger=comps["ci_trigger"],
                trigger_branch=self.config.trigger_branch,
            )
            domains.append(("test", test_loop))

            verify_loop = VerifyLoop(
                proc_manager=comps["proc_manager"],
                script_exec=comps["script_exec"],
                admin_query=comps["admin_query"],
            )
            domains.append(("verify", verify_loop))

            # client_ui 闭环需要动态创建 data_collector（需要 PID）
            # 延迟到执行时构造，这里先占位
            client_ui_loop = None  # 在下方动态创建
            domains.append(("client_ui", client_ui_loop))  # type: ignore[arg-type]

            # ── 串行执行 ──
            from .assertion.title_bar_parser import ClientDataCollector

            for domain_name, domain in domains:
                if domain is None:
                    # client_ui 闭环：动态创建
                    from .orchestrator.client_ui_loop import ClientUILoop
                    # PID=0 占位，实际在 execute 阶段由脚本启动后获取
                    data_collector = ClientDataCollector(process_pid=0)
                    domain = ClientUILoop(
                        script_exec=comps["script_exec"],
                        data_collector=data_collector,
                        assertion_engine=comps["assertion_engine"],
                        screenshot=comps["screenshot"],
                        vision_reviewer=comps["vision_reviewer"],
                        ark_client=comps["ark_client"],
                    )

                sm = LoopStateMachine(domain, max_retries=self.config.max_retries)
                result = await sm.run(context)
                domain_results.append((domain_name, result))

                # 记录域结果
                execution.domains.append({
                    "name": domain_name,
                    "status": result.status,
                    "duration_sec": round(result.duration_sec, 3),
                    "retry_count": result.retry_count,
                    "error": result.error,
                    "assertions_count": len(result.assertions),
                    "assertions_passed": sum(1 for a in result.assertions if a.passed),
                })

                # 任一域 FAILED 则停止后续
                if result.status == LoopStatus.FAILED:
                    execution.status = LoopStatus.FAILED
                    execution.finished_at = datetime.now()
                    break
            else:
                # 所有域通过
                execution.status = LoopStatus.PASSED
                execution.finished_at = datetime.now()

            # 计算总成本
            model_router = comps["model_router"]
            execution.total_cost_cny = model_router.get_daily_cost()

            # 持久化
            await save_execution(execution)

            # ── 生成报告（W4）──
            if report_path:
                from .report.json_report import JsonReport
                from .report.markdown import MarkdownReport
                from pathlib import Path

                md_report = MarkdownReport(title="LoopEngine 验证报告")
                json_report = JsonReport()

                md_report.add_execution_summary(execution)
                json_report.add_execution_summary(execution)
                for domain_name, result in domain_results:
                    md_report.add_loop_result(result, domain_name)
                    json_report.add_loop_result(result, domain_name)

                # 写 Markdown 报告
                execution.report_path = report_path
                md_report.save(report_path)

                # 写 JSON 报告（同目录，同文件名换后缀）
                json_path = str(Path(report_path).with_suffix(".json"))
                json_report.save(json_path)

                # 再次保存（更新 report_path）
                await save_execution(execution)

            return execution
        finally:
            # W5: 闭环结束后关闭 ark_client，释放连接池资源
            ark_client = comps.get("ark_client")
            if ark_client is not None:
                try:
                    await ark_client.close()
                except Exception:
                    pass

    async def run_dev_loop(self, task: str, spec_path: str | None = None) -> LoopResult:
        """仅执行开发闭环（编译验证）。

        Args:
            task: 任务描述。
            spec_path: 规格文件路径（v0.1 未使用）。

        Returns:
            LoopResult: 开发闭环结果。
        """
        from .orchestrator.dev_loop import DevLoop
        from .orchestrator.loop_state import LoopStateMachine

        comps = self._ensure_components()
        context = self._make_context()
        dev_loop = DevLoop(script_exec=comps["script_exec"])
        sm = LoopStateMachine(dev_loop, max_retries=self.config.max_retries)
        try:
            return await sm.run(context)
        finally:
            # W5: 闭环结束后关闭 ark_client
            try:
                await comps["ark_client"].close()
            except Exception:
                pass

    async def run_test_loop(self) -> LoopResult:
        """仅执行测试闭环（CI 驱动）。

        Returns:
            LoopResult: 测试闭环结果。
        """
        from .orchestrator.loop_state import LoopStateMachine
        from .orchestrator.test_loop import TestLoop

        comps = self._ensure_components()
        context = self._make_context()
        test_loop = TestLoop(
            ci_trigger=comps["ci_trigger"],
            trigger_branch=self.config.trigger_branch,
        )
        sm = LoopStateMachine(test_loop, max_retries=self.config.max_retries)
        try:
            return await sm.run(context)
        finally:
            # W5: 闭环结束后关闭 ark_client
            try:
                await comps["ark_client"].close()
            except Exception:
                pass

    async def run_verify_loop(self) -> LoopResult:
        """仅执行验证闭环（集群启动 + 冒烟 + Admin 查询）。

        Returns:
            LoopResult: 验证闭环结果。
        """
        from .orchestrator.loop_state import LoopStateMachine
        from .orchestrator.verify_loop import VerifyLoop

        comps = self._ensure_components()
        context = self._make_context()
        verify_loop = VerifyLoop(
            proc_manager=comps["proc_manager"],
            script_exec=comps["script_exec"],
            admin_query=comps["admin_query"],
        )
        sm = LoopStateMachine(verify_loop, max_retries=self.config.max_retries)
        try:
            return await sm.run(context)
        finally:
            # W5: 闭环结束后关闭 ark_client
            try:
                await comps["ark_client"].close()
            except Exception:
                pass

    async def run_client_ui_loop(self, duration: int = 10) -> LoopResult:
        """仅执行客户端界面闭环（结构化断言 + 视觉复核）。

        Args:
            duration: 采样时长（秒）。

        Returns:
            LoopResult: 客户端界面闭环结果。
        """
        from .orchestrator.client_ui_loop import ClientUILoop
        from .orchestrator.loop_state import LoopStateMachine

        comps = self._ensure_components()
        context = self._make_context()

        # 动态创建 data_collector（PID 在 execute 阶段获取）
        from .assertion.title_bar_parser import ClientDataCollector
        data_collector = ClientDataCollector(process_pid=0)

        client_ui_loop = ClientUILoop(
            script_exec=comps["script_exec"],
            data_collector=data_collector,
            assertion_engine=comps["assertion_engine"],
            screenshot=comps["screenshot"],
            vision_reviewer=comps["vision_reviewer"],
            ark_client=comps["ark_client"],
        )
        sm = LoopStateMachine(client_ui_loop, max_retries=2)
        try:
            return await sm.run(context)
        finally:
            # W5: 闭环结束后关闭 ark_client
            try:
                await comps["ark_client"].close()
            except Exception:
                pass


__all__ = [
    "LoopEngine",
    "LoopConfig",
    "LoopResult",
    "LoopStatus",
    "LoopContext",
    "AssertionResult",
    "AssertionSeverity",
    "LoopExecution",
]
