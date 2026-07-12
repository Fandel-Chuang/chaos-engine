"""LoopEngine CLI 入口 - spec 7.1。

用 argparse 实现子命令分发，标准库够用。

子命令:
    run       运行完整四域闭环
    dev       仅开发闭环（编译验证）
    test      仅测试闭环（CI 驱动）
    verify    仅验证闭环
    client-ui 仅客户端界面闭环
    status    查询最近执行状态
    ai-stats  查看 AI 调度统计
    report    从历史记录生成报告
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

import yaml

# 环境变量替换正则
_ENV_PATTERN = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def _expand_env(value: Any) -> Any:
    """递归替换字符串中的 ${ENV_VAR} 为环境变量值。"""
    if isinstance(value, str):
        return _ENV_PATTERN.sub(
            lambda m: os.environ.get(m.group(1), ""), value
        )
    if isinstance(value, dict):
        return {k: _expand_env(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand_env(item) for item in value]
    return value


def _load_config(config_path: str) -> dict[str, Any]:
    """加载 YAML 配置文件，替换 ${ENV_VAR} 为环境变量值。

    Args:
        config_path: loop.yaml 文件路径。

    Returns:
        dict: 配置字典（已替换环境变量）。
    """
    if not os.path.exists(config_path):
        print(f"错误: 配置文件不存在: {config_path}", file=sys.stderr)
        sys.exit(1)

    with open(config_path, "r", encoding="utf-8") as f:
        raw = yaml.safe_load(f) or {}

    return _expand_env(raw)


def _load_models_config(config_dir: str) -> dict[str, Any]:
    """加载 models.yaml（与 loop.yaml 同目录）。

    Args:
        config_dir: config 目录路径。

    Returns:
        dict: 模型配置字典。
    """
    models_path = os.path.join(config_dir, "models.yaml")
    if not os.path.exists(models_path):
        return {}
    try:
        with open(models_path, "r", encoding="utf-8") as f:
            return _expand_env(yaml.safe_load(f) or {})
    except Exception:
        return {}


def _init_components(config: dict[str, Any], models_config: dict[str, Any]) -> dict[str, Any]:
    """根据配置创建所有组件实例。

    Args:
        config: loop.yaml 配置字典。
        models_config: models.yaml 配置字典。

    Returns:
        dict: 组件名 -> 组件实例。
    """
    from .adapter.admin_query import AdminQuery
    from .adapter.capture import ScreenshotCapture
    from .adapter.ci_trigger import CITrigger
    from .adapter.proc_manager import ProcessManager
    from .adapter.script_exec import ScriptExec
    from .ai.ark_client import ArkClient
    from .ai.model_router import ModelRouter
    from .assertion.structured import ClientAssertionEngine
    from .assertion.vision_review import VisionReviewer

    chaos_dir = config.get("chaos_engine", {}).get("dir", "/home/zhongfangdao/chaos-engine")
    ark_cfg = config.get("ark", {})
    ci_cfg = config.get("ci", {})

    # 适配层
    script_exec = ScriptExec(chaos_dir)
    proc_manager = ProcessManager(chaos_dir)
    ci_trigger = CITrigger(
        gitee_token=ci_cfg.get("gitee_token", ""),
        repo=ci_cfg.get("repo", "zhong-fangdao/chaos-engine"),
    )
    admin_query = AdminQuery()
    screenshot = ScreenshotCapture()

    # AI 层
    ark_client = ArkClient(
        api_key=ark_cfg.get("api_key", ""),
        base_url=ark_cfg.get("base_url", "https://ark.cn-beijing.volces.com/api/coding/v3"),
    )
    model_router = ModelRouter(
        models_config=models_config, ark_client=ark_client
    )

    # 断言层
    assertion_engine = ClientAssertionEngine()
    vision_reviewer = VisionReviewer(ark_client=ark_client)

    return {
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


def _make_context(config: dict[str, Any], loop_id: str = "") -> Any:
    """构造 LoopContext。"""
    from .data.models import LoopContext

    chaos_cfg = config.get("chaos_engine", {})
    return LoopContext(
        chaos_engine_dir=chaos_cfg.get("dir", "/home/zhongfangdao/chaos-engine"),
        build_dir=chaos_cfg.get("build_dir", "build"),
        config=config,
        loop_id=loop_id,
    )


def _run_full_loop(
    config: dict[str, Any],
    report_path: str,
    models_config: dict[str, Any] | None = None,
    task: str = "",
) -> Any:
    """串行跑四域闭环，收集结果，存 SQLite，生成报告。

    Args:
        config: loop.yaml 配置字典。
        report_path: Markdown 报告输出路径。
        models_config: models.yaml 配置字典。
        task: 任务描述。

    Returns:
        LoopExecution: 完整执行记录。
    """
    return asyncio.run(
        _run_full_loop_async(config, report_path, models_config, task)
    )


async def _run_full_loop_async(
    config: dict[str, Any],
    report_path: str,
    models_config: dict[str, Any] | None = None,
    task: str = "",
) -> Any:
    """串行跑四域闭环的异步实现（单事件循环）。"""
    from .data.db import init_db, save_execution
    from .data.models import LoopExecution, LoopStatus
    from .orchestrator.dev_loop import DevLoop
    from .orchestrator.loop_state import LoopStateMachine
    from .orchestrator.test_loop import TestLoop
    from .orchestrator.verify_loop import VerifyLoop
    from .report.json_report import JsonReport
    from .report.markdown import MarkdownReport

    models_config = models_config or {}
    comps = _init_components(config, models_config)

    ci_cfg = config.get("ci", {})
    trigger_branch = ci_cfg.get("trigger_branch", "develop")
    loops_cfg = config.get("loops", {})
    max_retries = loops_cfg.get("dev", {}).get("max_retries", 3)

    loop_id = f"loop_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    context = _make_context(config, loop_id)

    execution = LoopExecution(
        loop_id=loop_id,
        task=task,
        started_at=datetime.now(),
        status=LoopStatus.RUNNING,
    )

    # 初始化数据库（同一事件循环内）
    await init_db()

    # 构造四域
    dev_loop = DevLoop(script_exec=comps["script_exec"])
    test_loop = TestLoop(
        ci_trigger=comps["ci_trigger"], trigger_branch=trigger_branch
    )
    verify_loop = VerifyLoop(
        proc_manager=comps["proc_manager"],
        script_exec=comps["script_exec"],
        admin_query=comps["admin_query"],
    )

    # 四域定义（client_ui 延迟构造）
    domain_defs: list[tuple[str, Any]] = [
        ("dev", dev_loop),
        ("test", test_loop),
        ("verify", verify_loop),
    ]

    # 报告生成器
    md_report = MarkdownReport(title="LoopEngine 验证报告")
    json_report = JsonReport()

    # 存储各域的 LoopResult（供报告使用）
    domain_results: list[tuple[str, Any]] = []

    try:
        # ── 串行执行 dev -> test -> verify ──
        for domain_name, domain in domain_defs:
            sm = LoopStateMachine(domain, max_retries=max_retries)
            result = await sm.run(context)

            domain_results.append((domain_name, result))
            execution.domains.append({
                "name": domain_name,
                "status": result.status,
                "duration_sec": round(result.duration_sec, 3),
                "retry_count": result.retry_count,
                "error": result.error,
                "assertions_count": len(result.assertions),
                "assertions_passed": sum(1 for a in result.assertions if a.passed),
            })

            if result.status == LoopStatus.FAILED:
                execution.status = LoopStatus.FAILED
                execution.finished_at = datetime.now()
                break
        else:
            # ── client_ui 闭环（需要动态创建 data_collector） ──
            from .assertion.title_bar_parser import ClientDataCollector
            from .orchestrator.client_ui_loop import ClientUILoop

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
            result = await sm.run(context)

            domain_results.append(("client_ui", result))
            execution.domains.append({
                "name": "client_ui",
                "status": result.status,
                "duration_sec": round(result.duration_sec, 3),
                "retry_count": result.retry_count,
                "error": result.error,
                "assertions_count": len(result.assertions),
                "assertions_passed": sum(1 for a in result.assertions if a.passed),
            })

            if result.status == LoopStatus.FAILED:
                execution.status = LoopStatus.FAILED
            else:
                execution.status = LoopStatus.PASSED
            execution.finished_at = datetime.now()

        # 计算总成本
        model_router = comps["model_router"]
        execution.total_cost_cny = model_router.get_daily_cost()

        # 持久化
        await save_execution(execution)

        # 生成报告
        md_report.add_execution_summary(execution)
        json_report.add_execution_summary(execution)
        for domain_name, result in domain_results:
            md_report.add_loop_result(result, domain_name)
            json_report.add_loop_result(result, domain_name)

        # 写 Markdown 报告
        if report_path:
            execution.report_path = report_path
            md_report.save(report_path)
            print(f"Markdown 报告已生成: {report_path}")

        # 写 JSON 报告（同目录，同文件名换后缀）
        json_path = str(Path(report_path).with_suffix(".json"))
        json_report.save(json_path)
        print(f"JSON 报告已生成: {json_path}")

        # 再次保存（更新 report_path）
        await save_execution(execution)

        return execution
    finally:
        # W5: 闭环结束后关闭 ark_client，释放连接池资源
        try:
            await comps["ark_client"].close()
        except Exception:
            pass


# ── 子命令处理函数 ──


def _cmd_run(args: argparse.Namespace) -> None:
    """run 子命令：运行完整四域闭环。"""
    config_path = args.config
    report_path = args.report

    config = _load_config(config_path)
    config_dir = os.path.dirname(os.path.abspath(config_path))
    models_config = _load_models_config(config_dir)

    task = args.task or ""
    print(f"=== LoopEngine 完整闭环启动 ===")
    print(f"配置: {config_path}")
    print(f"任务: {task or '(未指定)'}")
    print()

    execution = _run_full_loop(config, report_path, models_config, task)

    print()
    print(f"=== 闭环完成 ===")
    print(f"Loop ID: {execution.loop_id}")
    print(f"总状态: {execution.status.value}")
    print(f"总耗时: {sum(d.get('duration_sec', 0) for d in execution.domains):.1f}s")
    print(f"总成本: {execution.total_cost_cny:.4f} 元")
    for d in execution.domains:
        status_str = d["status"].value if hasattr(d["status"], "value") else str(d["status"])
        print(f"  {d['name']}: {status_str} ({d['duration_sec']}s, retries={d['retry_count']})")


def _cmd_dev(args: argparse.Namespace) -> None:
    """dev 子命令：仅开发闭环。"""
    from .orchestrator.dev_loop import DevLoop
    from .orchestrator.loop_state import LoopStateMachine

    config = _load_config(args.config)
    comps = _init_components(config, _load_models_config(os.path.dirname(os.path.abspath(args.config))))
    context = _make_context(config)

    task = args.task or ""
    print(f"=== 开发闭环 (编译验证) ===")
    if task:
        print(f"任务: {task}")
    print()

    dev_loop = DevLoop(script_exec=comps["script_exec"])
    max_retries = config.get("loops", {}).get("dev", {}).get("max_retries", 3)
    sm = LoopStateMachine(dev_loop, max_retries=max_retries)
    result = asyncio.run(sm.run(context))

    print(f"\n=== 开发闭环结果 ===")
    print(f"状态: {result.status.value}")
    print(f"耗时: {result.duration_sec:.1f}s")
    print(f"重试: {result.retry_count}")
    if result.error:
        print(f"错误: {result.error}")
    for a in result.assertions:
        mark = "✅" if a.passed else "❌"
        print(f"  {mark} {a.name}: {a.message}")


def _cmd_test(args: argparse.Namespace) -> None:
    """test 子命令：仅测试闭环。"""
    from .orchestrator.loop_state import LoopStateMachine
    from .orchestrator.test_loop import TestLoop

    config = _load_config(args.config)
    comps = _init_components(config, _load_models_config(os.path.dirname(os.path.abspath(args.config))))
    context = _make_context(config)

    ci_cfg = config.get("ci", {})
    trigger_branch = ci_cfg.get("trigger_branch", "develop")

    print(f"=== 测试闭环 (CI 驱动) ===")
    print(f"分支: {trigger_branch}")
    print()

    test_loop = TestLoop(
        ci_trigger=comps["ci_trigger"], trigger_branch=trigger_branch
    )
    max_retries = config.get("loops", {}).get("test", {}).get("max_retries", 3)
    sm = LoopStateMachine(test_loop, max_retries=max_retries)
    result = asyncio.run(sm.run(context))

    print(f"\n=== 测试闭环结果 ===")
    print(f"状态: {result.status.value}")
    print(f"耗时: {result.duration_sec:.1f}s")
    if result.error:
        print(f"错误: {result.error}")
    for a in result.assertions:
        mark = "✅" if a.passed else "❌"
        print(f"  {mark} {a.name}: {a.message}")


def _cmd_verify(args: argparse.Namespace) -> None:
    """verify 子命令：仅验证闭环。"""
    from .orchestrator.loop_state import LoopStateMachine
    from .orchestrator.verify_loop import VerifyLoop

    config = _load_config(args.config)
    comps = _init_components(config, _load_models_config(os.path.dirname(os.path.abspath(args.config))))
    context = _make_context(config)

    services = args.services
    print(f"=== 验证闭环 (集群 + 冒烟 + Admin) ===")
    if services != "all":
        print(f"指定服务: {services}")
    print()

    verify_loop = VerifyLoop(
        proc_manager=comps["proc_manager"],
        script_exec=comps["script_exec"],
        admin_query=comps["admin_query"],
    )
    max_retries = config.get("loops", {}).get("verify", {}).get("max_retries", 3)
    sm = LoopStateMachine(verify_loop, max_retries=max_retries)
    result = asyncio.run(sm.run(context))

    print(f"\n=== 验证闭环结果 ===")
    print(f"状态: {result.status.value}")
    print(f"耗时: {result.duration_sec:.1f}s")
    if result.error:
        print(f"错误: {result.error}")
    for a in result.assertions:
        mark = "✅" if a.passed else "❌"
        print(f"  {mark} {a.name}: {a.message}")


def _cmd_client_ui(args: argparse.Namespace) -> None:
    """client-ui 子命令：仅客户端界面闭环。"""
    from .assertion.title_bar_parser import ClientDataCollector
    from .orchestrator.client_ui_loop import ClientUILoop
    from .orchestrator.loop_state import LoopStateMachine

    config = _load_config(args.config)
    comps = _init_components(config, _load_models_config(os.path.dirname(os.path.abspath(args.config))))
    context = _make_context(config)

    duration = args.duration
    screenshot = args.screenshot

    print(f"=== 客户端界面闭环 (结构化断言 + 视觉复核) ===")
    print(f"采样时长: {duration}s")
    print(f"截图: {'是' if screenshot else '否'}")
    print()

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
    result = asyncio.run(sm.run(context))

    print(f"\n=== 客户端界面闭环结果 ===")
    print(f"状态: {result.status.value}")
    print(f"耗时: {result.duration_sec:.1f}s")
    if result.error:
        print(f"错误: {result.error}")
    if result.ai_analysis:
        print(f"AI 分析: {result.ai_analysis}")
    for a in result.assertions:
        mark = "✅" if a.passed else "❌"
        print(f"  {mark} {a.name}: {a.message}")


def _cmd_status(args: argparse.Namespace) -> None:
    """status 子命令：查询最近执行状态。"""
    from .data.db import init_db, list_executions

    limit = args.limit

    async def _run() -> list:
        await init_db()
        return await list_executions(limit=limit)

    executions = asyncio.run(_run())

    if not executions:
        print("无执行记录。")
        return

    print(f"最近 {len(executions)} 条执行记录:")
    print("-" * 80)
    print(f"{'Loop ID':<28} {'任务':<20} {'状态':<8} {'开始时间':<20} {'成本':<10}")
    print("-" * 80)
    for ex in executions:
        task_short = (ex.task or "")[:18]
        status_str = ex.status.value
        started = ex.started_at.strftime("%Y-%m-%d %H:%M") if ex.started_at else "未知"
        cost = f"{ex.total_cost_cny:.4f}"
        print(f"{ex.loop_id:<28} {task_short:<20} {status_str:<8} {started:<20} {cost:<10}")


def _cmd_ai_stats(args: argparse.Namespace) -> None:
    """ai-stats 子命令：查看 AI 调度统计。"""
    config = _load_config(args.config)
    config_dir = os.path.dirname(os.path.abspath(args.config))
    models_config = _load_models_config(config_dir)
    comps = _init_components(config, models_config)

    model_router = comps["model_router"]
    usage = model_router.get_daily_usage()
    daily_cost = model_router.get_daily_cost()

    print("=== AI 调度统计（今日）===")
    print(f"今日总成本: {daily_cost:.4f} 元")
    print()

    if not usage:
        print("无调用记录。")
        return

    print(f"{'模型':<30} {'调用次数':<10} {'输入tokens':<14} {'输出tokens':<14} {'成本(元)':<10}")
    print("-" * 80)
    for model_id, stats in usage.items():
        print(
            f"{model_id:<30} {stats['calls']:<10} "
            f"{stats['input_tokens']:<14} {stats['output_tokens']:<14} "
            f"{stats['cost_cny']:<10.4f}"
        )

    # 预算检查
    cost_cfg = config.get("cost", {})
    daily_budget = cost_cfg.get("daily_budget_cny", 10.0)
    print()
    print(f"每日预算: {daily_budget} 元")
    if daily_cost >= daily_budget:
        print("⚠️  已超出每日预算！")
    else:
        print(f"剩余预算: {daily_budget - daily_cost:.4f} 元")


def _cmd_report(args: argparse.Namespace) -> None:
    """report 子命令：从历史记录生成报告。"""
    from .data.db import get_execution, init_db, list_executions
    from .data.models import LoopExecution
    from .report.json_report import JsonReport
    from .report.markdown import MarkdownReport

    output_path = args.output
    fmt = args.format
    loop_id = args.loop_id

    async def _fetch_execution() -> LoopExecution | None:
        await init_db()
        if loop_id:
            return await get_execution(loop_id)
        else:
            executions = await list_executions(limit=1)
            return executions[0] if executions else None

    execution = asyncio.run(_fetch_execution())

    if not execution:
        if loop_id:
            print(f"错误: 未找到 loop_id={loop_id}", file=sys.stderr)
        else:
            print("无执行记录，无法生成报告。", file=sys.stderr)
        sys.exit(1)

    if not loop_id:
        print(f"使用最近一条记录: {execution.loop_id}")

    # 从数据库恢复各域结果（domains 里只有摘要，无完整 LoopResult）
    # 这里用 domains 摘要生成简化报告
    if fmt == "markdown":
        report = MarkdownReport(title=f"LoopEngine 报告 - {execution.loop_id}")
        report.add_execution_summary(execution)
        for d in execution.domains:
            from .data.models import LoopResult

            status_val = d.get("status", "")
            # 兼容 status_val 可能是 str（从数据库恢复）或 LoopStatus enum
            if not hasattr(status_val, "value"):
                from .data.models import LoopStatus
                try:
                    status_val = LoopStatus(status_val)
                except (ValueError, TypeError):
                    pass

            result = LoopResult(
                status=status_val if hasattr(status_val, "value") else LoopStatus.PENDING,  # type: ignore[arg-type]
                retry_count=d.get("retry_count", 0),
                duration_sec=d.get("duration_sec", 0.0),
                error=d.get("error"),
            )
            report.add_loop_result(result, d.get("name", "unknown"))

        if output_path:
            report.save(output_path)
            print(f"Markdown 报告已生成: {output_path}")
        else:
            print(report.render())

    elif fmt == "json":
        report = JsonReport()
        report.add_execution_summary(execution)

        if output_path:
            report.save(output_path)
            print(f"JSON 报告已生成: {output_path}")
        else:
            print(report.render())

    else:
        print(f"错误: 不支持的格式: {fmt}", file=sys.stderr)
        sys.exit(1)


# ── 参数解析 ──


def _build_parser() -> argparse.ArgumentParser:
    """构建 argparse 解析器。"""
    parser = argparse.ArgumentParser(
        prog="loopengine",
        description="LoopEngine - 混沌引擎自动化闭环验证工具",
    )
    subparsers = parser.add_subparsers(dest="command", help="子命令")

    # ── run ──
    run_parser = subparsers.add_parser("run", help="运行完整四域闭环")
    run_parser.add_argument(
        "--config", default="config/loop.yaml", help="配置文件路径 (默认: config/loop.yaml)"
    )
    run_parser.add_argument(
        "--report", default="report.md", help="Markdown 报告输出路径 (默认: report.md)"
    )
    run_parser.add_argument("--task", default="", help="任务描述")
    run_parser.set_defaults(func=_cmd_run)

    # ── dev ──
    dev_parser = subparsers.add_parser("dev", help="仅开发闭环 (编译验证)")
    dev_parser.add_argument("--config", default="config/loop.yaml", help="配置文件路径")
    dev_parser.add_argument("--task", default="", help="任务描述")
    dev_parser.add_argument("--spec", default=None, help="规格文件路径 (v0.1 未使用)")
    dev_parser.set_defaults(func=_cmd_dev)

    # ── test ──
    test_parser = subparsers.add_parser("test", help="仅测试闭环 (CI 驱动)")
    test_parser.add_argument("--config", default="config/loop.yaml", help="配置文件路径")
    test_parser.add_argument("--filter", default=None, help="测试过滤 (v0.1 未使用)")
    test_parser.set_defaults(func=_cmd_test)

    # ── verify ──
    verify_parser = subparsers.add_parser("verify", help="仅验证闭环")
    verify_parser.add_argument("--config", default="config/loop.yaml", help="配置文件路径")
    verify_parser.add_argument(
        "--services", default="all", help="指定服务 (默认: all)"
    )
    verify_parser.set_defaults(func=_cmd_verify)

    # ── client-ui ──
    client_parser = subparsers.add_parser("client-ui", help="仅客户端界面闭环")
    client_parser.add_argument("--config", default="config/loop.yaml", help="配置文件路径")
    client_parser.add_argument(
        "--duration", type=int, default=10, help="采样时长秒数 (默认: 10)"
    )
    client_parser.add_argument(
        "--screenshot", action="store_true", help="是否截图"
    )
    client_parser.set_defaults(func=_cmd_client_ui)

    # ── status ──
    status_parser = subparsers.add_parser("status", help="查询最近执行状态")
    status_parser.add_argument("--limit", type=int, default=20, help="显示条数 (默认: 20)")
    status_parser.set_defaults(func=_cmd_status)

    # ── ai-stats ──
    ai_stats_parser = subparsers.add_parser("ai-stats", help="查看 AI 调度统计")
    ai_stats_parser.add_argument("--config", default="config/loop.yaml", help="配置文件路径")
    ai_stats_parser.set_defaults(func=_cmd_ai_stats)

    # ── report ──
    report_parser = subparsers.add_parser("report", help="从历史记录生成报告")
    report_parser.add_argument(
        "--format",
        choices=["markdown", "json"],
        default="markdown",
        help="报告格式 (默认: markdown)",
    )
    report_parser.add_argument("--output", default=None, help="输出文件路径 (不指定则打印到 stdout)")
    report_parser.add_argument("--loop-id", default=None, help="指定 loop_id (不指定则用最近一条)")
    report_parser.set_defaults(func=_cmd_report)

    return parser


def main(argv: list[str] | None = None) -> None:
    """CLI 入口函数。

    Args:
        argv: 命令行参数列表。为 None 时用 sys.argv。
    """
    parser = _build_parser()
    args = parser.parse_args(argv)

    if not hasattr(args, "func"):
        parser.print_help()
        sys.exit(1)

    args.func(args)


if __name__ == "__main__":
    main()
