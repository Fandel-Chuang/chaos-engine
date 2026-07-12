"""客户端界面闭环 - spec 3.5 + 4.x。

结构化断言（11 条）+ 截图视觉复核。
流程: 异步启动客户端 -> 等待窗口 -> 采样 10 秒 -> 断言判定 -> 视觉复核（按需）。
"""

from __future__ import annotations

import asyncio
import logging
import subprocess
import time
from typing import Any

from ..adapter.capture import ScreenshotCapture
from ..adapter.script_exec import ScriptExec
from ..ai.ark_client import ArkClient
from ..assertion.structured import (
    AssertionReport,
    AssertionVerdict as StructAssertionResult,
    ClientAssertionEngine,
)
from ..assertion.title_bar_parser import (
    ClientDataCollector,
    LogSnapshot,
    ProcessSnapshot,
    TitleBarSnapshot,
)
from ..assertion.vision_review import VisionReviewer, VisionVerdict
from ..data.models import (
    AssertionResult,
    AssertionSeverity,
    LoopContext,
    LoopResult,
    LoopStatus,
)
from .loop_state import LoopDomain

logger = logging.getLogger(__name__)

# 采样参数
_SAMPLE_DURATION_SEC = 10  # 采样总时长
_SAMPLE_INTERVAL_SEC = 1   # 每秒采样一次
_WINDOW_WAIT_TIMEOUT = 15  # 等待窗口出现的超时
_WINDOW_POLL_INTERVAL = 1  # 轮询窗口的间隔

# 客户端窗口标题（用于截图查找）
_CLIENT_WINDOW_TITLE = "ChaosEngine Vulkan"


class ClientUILoop(LoopDomain):
    """客户端界面闭环域 - 结构化断言 + 截图视觉复核。

    流程:
        1. 异步启动客户端 (start_client.sh --vulkan)
        2. 等待窗口出现（轮询 collect_process() 直到 window_exists）
        3. 采样 10 秒：每秒 collect_title_bar() + collect_log() + collect_process()
        4. 构造 context dict 给 assertion_engine.run_all()
    验证判定:
        - CRITICAL 失败 -> 直接 FAILED
        - 全通过 -> PASSED（不调视觉模型）
        - WARNING 失败 -> 触发视觉复核
            * 视觉 NORMAL -> PASSED（误报修正）
            * 视觉异常 -> FAILED
    execute 结束后停客户端进程。
    """

    @property
    def name(self) -> str:
        return "client_ui"

    @property
    def max_retries(self) -> int:
        return 2

    def __init__(
        self,
        script_exec: ScriptExec,
        data_collector: ClientDataCollector,
        assertion_engine: ClientAssertionEngine,
        screenshot: ScreenshotCapture,
        vision_reviewer: VisionReviewer,
        ark_client: ArkClient,
    ) -> None:
        """初始化客户端界面闭环。

        Args:
            script_exec: 脚本执行器，用于启动客户端。
            data_collector: 客户端数据采集器。
            assertion_engine: 结构化断言引擎。
            screenshot: 截图器。
            vision_reviewer: 视觉复核器。
            ark_client: ARK API 客户端（供 vision_reviewer 内部使用）。
        """
        self.script_exec = script_exec
        self.data_collector = data_collector
        self.assertion_engine = assertion_engine
        self.screenshot = screenshot
        self.vision_reviewer = vision_reviewer
        self.ark_client = ark_client
        # 缓存最近一次采样的断言报告和原始数据，供 verify 阶段使用
        self._last_reports: list[AssertionReport] | None = None
        self._last_title_text: str = ""
        self._last_log_summary: str = ""
        self._client_proc: subprocess.Popen | None = None
        # W7: 缓存 execute() 阶段的截图路径，供 verify() 视觉复核直接使用，避免重启客户端
        self._execute_screenshot_path: str = ""

    async def execute(self, context: LoopContext) -> LoopResult:
        """启动客户端，采样数据，执行结构化断言。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 执行结果（包含断言报告，但不做最终判定，判定在 verify 阶段）。
        """
        start = time.time()
        logger.info("[client_ui] 启动客户端并采样")

        # ── 异步启动客户端 ──
        try:
            self._client_proc = await asyncio.to_thread(
                self.script_exec.run_async,
                "start_client.sh",
                args=["--vulkan"],
            )
            # 更新 data_collector 的 PID，使 collect_process() 能正确检查进程存活
            if self._client_proc is not None and self._client_proc.pid:
                self.data_collector.process_pid = self._client_proc.pid
                logger.info("[client_ui] 客户端 PID=%d", self._client_proc.pid)
        except Exception as e:
            duration = time.time() - start
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=duration,
                error=f"启动客户端失败: {e}",
            )

        try:
            # ── 等待窗口出现 ──
            window_found = await self._wait_for_window(_WINDOW_WAIT_TIMEOUT)
            if not window_found:
                duration = time.time() - start
                return LoopResult(
                    status=LoopStatus.FAILED,
                    duration_sec=duration,
                    artifacts={"window_found": False},
                    error=f"客户端窗口 {_WINDOW_WAIT_TIMEOUT}s 内未出现",
                )

            logger.info("[client_ui] 窗口已出现，开始 %d 秒采样", _SAMPLE_DURATION_SEC)

            # ── 采样 10 秒 ──
            title_snapshots: list[TitleBarSnapshot] = []
            log_snapshots: list[LogSnapshot] = []
            process_snapshots: list[ProcessSnapshot] = []
            all_log_lines: list[str] = []

            for i in range(_SAMPLE_DURATION_SEC):
                sample_start = time.time()

                # 采集三路信号（同步调用，放线程池）
                title_snap = await asyncio.to_thread(self.data_collector.collect_title_bar)
                proc_snap = await asyncio.to_thread(self.data_collector.collect_process)

                # 读取客户端 stdout 日志行
                log_lines = await self._read_client_stdout()
                log_snap = await asyncio.to_thread(
                    self.data_collector.collect_log, log_lines
                )

                title_snapshots.append(title_snap)
                log_snapshots.append(log_snap)
                process_snapshots.append(proc_snap)
                all_log_lines.extend(log_lines)

                logger.debug(
                    "[client_ui] 采样 %d/%d: alive=%s window=%s connected=%s visible=%s",
                    i + 1, _SAMPLE_DURATION_SEC,
                    proc_snap.process_alive, proc_snap.window_exists,
                    title_snap.connected, title_snap.visible,
                )

                # 等待到下一秒
                elapsed = time.time() - sample_start
                if elapsed < _SAMPLE_INTERVAL_SEC:
                    await asyncio.sleep(_SAMPLE_INTERVAL_SEC - elapsed)

            duration = time.time() - start

            # ── 构造 context dict，用最后一次采样做断言 ──
            last_title = title_snapshots[-1] if title_snapshots else TitleBarSnapshot(timestamp=time.time(), raw_text="")
            last_log = log_snapshots[-1] if log_snapshots else LogSnapshot(timestamp=time.time(), lines=[])
            last_proc = process_snapshots[-1] if process_snapshots else ProcessSnapshot(timestamp=time.time())

            assertion_context = {
                "title": last_title,
                "log": last_log,
                "process": last_proc,
            }

            # 执行结构化断言
            reports = await asyncio.to_thread(
                self.assertion_engine.run_all, assertion_context
            )

            # 缓存供 verify 阶段使用
            self._last_reports = reports
            self._last_title_text = last_title.raw_text
            self._last_log_summary = "\n".join(all_log_lines[-20:])  # 最后 20 行作为摘要

            # 统计断言结果
            passed_count = sum(1 for r in reports if r.result == StructAssertionResult.PASSED)
            failed_count = sum(1 for r in reports if r.result == StructAssertionResult.FAILED)

            logger.info(
                "[client_ui] 断言完成: %d passed, %d failed (共 %d 条)",
                passed_count, failed_count, len(reports),
            )

            # 执行阶段不做最终判定，交由 verify 阶段决定 PASSED/FAILED
            # 但如果 CRITICAL 断言全通过，可标记为 PASSED 进入 verify
            overall = self.assertion_engine.overall_result(reports)
            exec_status = LoopStatus.PASSED if overall == StructAssertionResult.PASSED else LoopStatus.FAILED

            # W7: 采样阶段结束后、停止客户端前，保存一次截图路径供 verify() 视觉复核使用
            # 避免 verify() 阶段重新启动客户端截图
            if last_proc.window_exists:
                try:
                    self._execute_screenshot_path = await asyncio.to_thread(
                        self.screenshot.capture_window_by_title,
                        _CLIENT_WINDOW_TITLE,
                    )
                except Exception as e:
                    logger.warning("[client_ui] execute 阶段截图失败（将在 verify 阶段重试）: %s", e)
                    self._execute_screenshot_path = ""

            return LoopResult(
                status=exec_status,
                duration_sec=duration,
                artifacts={
                    "window_found": True,
                    "sample_count": len(title_snapshots),
                    "assertion_passed": passed_count,
                    "assertion_failed": failed_count,
                    "last_title_text": self._last_title_text,
                    "last_process": {
                        "process_alive": last_proc.process_alive,
                        "window_exists": last_proc.window_exists,
                        "window_id": last_proc.window_id,
                        "window_size": f"{last_proc.window_width}x{last_proc.window_height}",
                    },
                    "title_snapshots": [
                        {
                            "timestamp": s.timestamp,
                            "raw_text": s.raw_text,
                            "connected": s.connected,
                            "visible": s.visible,
                            "pos": (s.pos_x, s.pos_y, s.pos_z) if s.pos_x is not None else None,
                        }
                        for s in title_snapshots
                    ],
                },
                assertions=self._convert_reports(reports),
                error=None if exec_status == LoopStatus.PASSED else f"结构化断言: {failed_count} 条失败",
            )

        finally:
            # ── 停止客户端进程 ──
            await self._stop_client()

    async def verify(self, context: LoopContext) -> LoopResult:
        """根据断言结果做最终判定，必要时触发视觉复核。

        判定逻辑:
            - CRITICAL 失败 -> 直接 FAILED
            - 全通过 -> PASSED（不调视觉模型）
            - WARNING 失败 -> 触发视觉复核
                * 视觉 NORMAL -> PASSED（误报修正）
                * 视觉异常 -> FAILED

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 最终验证结果。
        """
        start = time.time()
        logger.info("[client_ui] 验证断言结果")

        reports = self._last_reports
        if reports is None:
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=0.0,
                error="无断言报告（execute 未成功执行）",
            )

        # 判定是否需要视觉复核
        need_vision = self.assertion_engine.should_trigger_vision_review(reports)

        if not need_vision:
            # 不需要视觉复核，直接按 overall_result 判定
            overall = self.assertion_engine.overall_result(reports)
            duration = time.time() - start

            if overall == StructAssertionResult.PASSED:
                logger.info("[client_ui] 全部断言通过，无需视觉复核 -> PASSED")
                return LoopResult(
                    status=LoopStatus.PASSED,
                    duration_sec=duration,
                    artifacts={
                        "assertion_reports": self._reports_to_dicts(reports),
                        "vision_review_triggered": False,
                    },
                    assertions=self._convert_reports(reports),
                    error=None,
                )
            else:
                # CRITICAL 失败，直接 FAILED
                critical_fails = [
                    r.name for r in reports
                    if r.result == StructAssertionResult.FAILED
                    and r.severity == AssertionSeverity.CRITICAL
                ]
                logger.warning("[client_ui] CRITICAL 断言失败 -> FAILED: %s", critical_fails)
                return LoopResult(
                    status=LoopStatus.FAILED,
                    duration_sec=duration,
                    artifacts={
                        "assertion_reports": self._reports_to_dicts(reports),
                        "vision_review_triggered": False,
                        "critical_failures": critical_fails,
                    },
                    assertions=self._convert_reports(reports),
                    error=f"CRITICAL 断言失败: {critical_fails}",
                )

        # ── 触发视觉复核 ──
        logger.info("[client_ui] WARNING 断言失败，触发视觉复核")

        # W7: 优先使用 execute() 阶段已保存的截图，避免重启客户端
        # 仅在 execute 截图不可用时才回退到 _capture_screenshot()（重启客户端）
        screenshot_path = self._execute_screenshot_path
        vision_result = None

        try:
            # 如果 execute 阶段截图不可用，回退到重启客户端截图
            if not screenshot_path:
                screenshot_path = await self._capture_screenshot()
            if not screenshot_path:
                duration = time.time() - start
                return LoopResult(
                    status=LoopStatus.FAILED,
                    duration_sec=duration,
                    artifacts={
                        "assertion_reports": self._reports_to_dicts(reports),
                        "vision_review_triggered": True,
                        "screenshot_path": "",
                        "vision_error": "截图失败",
                    },
                    assertions=self._convert_reports(reports),
                    error="视觉复核阶段截图失败",
                )

            # 构造失败断言信息给视觉模型
            failed_assertions = [
                {
                    "name": r.name,
                    "description": r.description,
                    "severity": r.severity.value,
                    "actual_value": str(r.actual_value),
                }
                for r in reports
                if r.result == StructAssertionResult.FAILED
            ]

            vision_result = await self.vision_reviewer.review(
                screenshot_path=screenshot_path,
                failed_assertions=failed_assertions,
                title_bar_text=self._last_title_text,
                log_summary=self._last_log_summary,
            )

            duration = time.time() - start

            # 根据视觉判定决定最终状态
            if vision_result.verdict == VisionVerdict.NORMAL:
                logger.info("[client_ui] 视觉复核判定 NORMAL -> PASSED（误报修正）")
                return LoopResult(
                    status=LoopStatus.PASSED,
                    duration_sec=duration,
                    artifacts={
                        "assertion_reports": self._reports_to_dicts(reports),
                        "vision_review_triggered": True,
                        "screenshot_path": screenshot_path,
                        "vision_verdict": vision_result.verdict.value,
                        "vision_confidence": vision_result.confidence,
                        "vision_description": vision_result.description,
                        "vision_issues": vision_result.issues,
                        "corrected": True,  # 误报修正
                    },
                    assertions=self._convert_reports(reports),
                    ai_analysis=f"视觉复核: {vision_result.description}",
                    error=None,
                )
            else:
                logger.warning(
                    "[client_ui] 视觉复核判定 %s -> FAILED",
                    vision_result.verdict.value,
                )
                return LoopResult(
                    status=LoopStatus.FAILED,
                    duration_sec=duration,
                    artifacts={
                        "assertion_reports": self._reports_to_dicts(reports),
                        "vision_review_triggered": True,
                        "screenshot_path": screenshot_path,
                        "vision_verdict": vision_result.verdict.value,
                        "vision_confidence": vision_result.confidence,
                        "vision_description": vision_result.description,
                        "vision_issues": vision_result.issues,
                    },
                    assertions=self._convert_reports(reports),
                    ai_analysis=f"视觉复核: {vision_result.description}",
                    error=f"视觉复核异常: {vision_result.verdict.value} - {vision_result.description}",
                )

        except Exception as e:
            duration = time.time() - start
            logger.error("[client_ui] 视觉复核异常: %s", e)
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=duration,
                artifacts={
                    "assertion_reports": self._reports_to_dicts(reports),
                    "vision_review_triggered": True,
                    "screenshot_path": screenshot_path,
                    "vision_error": str(e),
                },
                assertions=self._convert_reports(reports),
                error=f"视觉复核异常: {e}",
            )

    async def retry(self, context: LoopContext, reason: str) -> LoopResult:
        """重试 - 重启客户端重采。

        Args:
            context: 闭环执行上下文。
            reason: 上次失败的原因。

        Returns:
            LoopResult: 重试执行结果。
        """
        logger.info("[client_ui] 重试，原因: %s", reason)
        # 确保旧客户端已停止
        await self._stop_client()
        # 清除缓存
        self._last_reports = None
        self._last_title_text = ""
        self._last_log_summary = ""
        self._execute_screenshot_path = ""  # W7: 清除截图缓存
        return await self.execute(context)

    # ── 内部辅助方法 ──

    async def _wait_for_window(self, timeout: int) -> bool:
        """轮询 collect_process() 直到窗口出现或超时。

        Args:
            timeout: 最大等待秒数。

        Returns:
            bool: 窗口是否在超时前出现。
        """
        start = time.time()
        while time.time() - start < timeout:
            proc_snap = await asyncio.to_thread(self.data_collector.collect_process)
            if proc_snap.window_exists:
                return True
            await asyncio.sleep(_WINDOW_POLL_INTERVAL)
        return False

    async def _read_client_stdout(self) -> list[str]:
        """读取客户端进程的 stdout 日志行。

        从 _client_proc 的 stdout 非阻塞读取可用行。

        Returns:
            list[str]: 本次读取到的日志行。
        """
        if self._client_proc is None or self._client_proc.stdout is None:
            return []

        def _read() -> list[str]:
            lines: list[str] = []
            try:
                while True:
                    line = self._client_proc.stdout.readline()  # type: ignore[union-attr]
                    if not line:
                        break
                    lines.append(line.rstrip("\n"))
                    # 限制单次读取行数，避免阻塞太久
                    if len(lines) >= 50:
                        break
            except Exception:
                pass
            return lines

        return await asyncio.to_thread(_read)

    async def _capture_screenshot(self) -> str:
        """启动客户端并截图。

        视觉复核需要客户端窗口存在，因此短暂启动客户端截图后停止。

        Returns:
            str: 截图文件路径，失败返回空字符串。
        """
        proc = None
        try:
            # 启动客户端
            proc = await asyncio.to_thread(
                self.script_exec.run_async,
                "start_client.sh",
                args=["--vulkan"],
            )

            # 等待窗口出现
            window_found = await self._wait_for_window(_WINDOW_WAIT_TIMEOUT)
            if not window_found:
                return ""

            # 截图
            screenshot_path = await asyncio.to_thread(
                self.screenshot.capture_window_by_title,
                _CLIENT_WINDOW_TITLE,
            )
            return screenshot_path

        except Exception as e:
            logger.warning("[client_ui] 截图失败: %s", e)
            return ""
        finally:
            # 停止客户端
            if proc is not None:
                await self._stop_proc(proc)

    async def _stop_client(self) -> None:
        """停止主客户端进程。"""
        if self._client_proc is not None:
            await self._stop_proc(self._client_proc)
            self._client_proc = None

    @staticmethod
    async def _stop_proc(proc: subprocess.Popen) -> None:
        """停止一个 Popen 进程。"""
        def _kill() -> None:
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except Exception:
                try:
                    proc.kill()
                    proc.wait(timeout=3)
                except Exception:
                    pass

        await asyncio.to_thread(_kill)

    @staticmethod
    def _convert_reports(reports: list[AssertionReport]) -> list[AssertionResult]:
        """将 structured.AssertionReport 转为 data.models.AssertionResult。

        保留 name / severity / passed / actual_value / message。

        Args:
            reports: 结构化断言报告列表。

        Returns:
            list[AssertionResult]: 转换后的断言结果列表。
        """
        return [
            AssertionResult(
                name=r.name,
                severity=r.severity,
                passed=(r.result == StructAssertionResult.PASSED),
                actual_value=r.actual_value,
                expected_value=r.expected,
                message=f"{r.description} -> {r.result.value}",
            )
            for r in reports
        ]

    @staticmethod
    def _reports_to_dicts(reports: list[AssertionReport]) -> list[dict[str, Any]]:
        """将断言报告转为可序列化的 dict 列表（存入 artifacts）。"""
        return [
            {
                "name": r.name,
                "description": r.description,
                "severity": r.severity.value,
                "result": r.result.value,
                "actual_value": str(r.actual_value),
                "expected": r.expected,
                "timestamp": r.timestamp,
            }
            for r in reports
        ]
