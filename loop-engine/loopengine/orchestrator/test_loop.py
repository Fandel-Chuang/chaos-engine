"""测试闭环 - spec 3.3（CI 驱动）。

核心原则：不在本地执行测试脚本，只通过 Gitee CI 触发/轮询/解析。
复用 .gitee-ci.yml 现有 5 job（build-and-test / release-build / lua-lint / memcheck）。
"""

from __future__ import annotations

import logging
import time
from typing import Any

from ..adapter.ci_trigger import CITrigger
from ..data.models import (
    AssertionResult,
    AssertionSeverity,
    LoopContext,
    LoopResult,
    LoopStatus,
)
from .loop_state import LoopDomain

logger = logging.getLogger(__name__)

# ctest 最少通过数（spec 要求 >=18）
_CTEST_MIN_PASS = 18


class TestLoop(LoopDomain):
    """测试闭环域 - CI 驱动测试。

    不本地执行 build_and_test.sh，只通过 CITrigger 触发 Gitee CI 流水线，
    轮询等待完成，解析各 job 结果。任一关键 job 失败则判 FAILED。
    """

    @property
    def name(self) -> str:
        return "test"

    @property
    def max_retries(self) -> int:
        return 3

    def __init__(self, ci_trigger: CITrigger, trigger_branch: str = "develop") -> None:
        """初始化测试闭环。

        Args:
            ci_trigger: CI 触发器实例。
            trigger_branch: 触发 CI 的分支名。
        """
        self.ci_trigger = ci_trigger
        self.trigger_branch = trigger_branch
        # 缓存最近一次 CI run_id，供 verify 阶段复用
        self._last_run_id: str | None = None

    async def execute(self, context: LoopContext) -> LoopResult:
        """触发 CI 流水线并轮询等待完成。

        流程:
            1. ci_trigger.trigger_pipeline(branch) 触发流水线
            2. ci_trigger.poll_until_complete(run_id, timeout=1800, interval=15) 轮询
            3. 不本地执行 build_and_test.sh

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: CI 触发和轮询结果。
        """
        start = time.time()
        logger.info("[test] 触发 CI 流水线 (branch=%s)", self.trigger_branch)

        # ── 触发 CI ──
        try:
            trigger_info = await self.ci_trigger.trigger_pipeline(self.trigger_branch)
        except RuntimeError as e:
            duration = time.time() - start
            logger.error("[test] 触发 CI 失败: %s", e)
            return LoopResult(
                status=LoopStatus.FAILED,
                retry_count=0,
                duration_sec=duration,
                artifacts={"trigger_error": str(e)},
                assertions=[],
                error=f"CI 触发失败: {e}",
            )

        run_id = trigger_info.get("run_id", "")
        if not run_id:
            duration = time.time() - start
            return LoopResult(
                status=LoopStatus.FAILED,
                retry_count=0,
                duration_sec=duration,
                artifacts={"trigger_info": trigger_info},
                assertions=[],
                error="CI 触发返回空 run_id",
            )

        self._last_run_id = run_id
        logger.info("[test] CI 已触发, run_id=%s, 轮询等待完成...", run_id)

        # ── 轮询等待完成 ──
        try:
            poll_result = await self.ci_trigger.poll_until_complete(
                run_id, timeout=1800, interval=15
            )
        except Exception as e:
            duration = time.time() - start
            logger.error("[test] CI 轮询异常: %s", e)
            return LoopResult(
                status=LoopStatus.FAILED,
                retry_count=0,
                duration_sec=duration,
                artifacts={
                    "run_id": run_id,
                    "poll_error": str(e),
                },
                assertions=[],
                error=f"CI 轮询异常: {e}",
            )

        duration = time.time() - start
        pipeline_status = poll_result.get("status", "unknown")
        jobs = poll_result.get("jobs", [])

        logger.info(
            "[test] CI 完成, status=%s, jobs=%d, duration=%.1fs",
            pipeline_status, len(jobs), duration,
        )

        # 轮询超时也算失败
        if pipeline_status == "timeout":
            return LoopResult(
                status=LoopStatus.FAILED,
                retry_count=0,
                duration_sec=duration,
                artifacts={
                    "run_id": run_id,
                    "pipeline_status": pipeline_status,
                    "jobs": jobs,
                },
                assertions=[],
                error=f"CI 轮询超时 (1800s), run_id={run_id}",
            )

        # 触发+轮询成功（不等于测试通过，需要 verify 阶段判定）
        return LoopResult(
            status=LoopStatus.PASSED,
            retry_count=0,
            duration_sec=duration,
            artifacts={
                "run_id": run_id,
                "pipeline_status": pipeline_status,
                "jobs": jobs,
                "trigger_info": trigger_info,
            },
            assertions=[],
            error=None,
        )

    async def verify(self, context: LoopContext) -> LoopResult:
        """解析 CI 测试结果，判定各 job 是否通过。

        通过 ci_trigger.parse_test_results(run_id) 解析各 job 结果，
        断言:
            - build_test job passed
            - ctest_pass >= 18
            - smoke passed
            - lua_lint passed
            - valgrind passed 或 skipped

        全 passed -> PASSED，任一 failed -> FAILED + 拉取失败 job 日志。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 验证结果。
        """
        start = time.time()
        run_id = self._last_run_id

        if not run_id:
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=0.0,
                error="无 CI run_id，无法解析结果（execute 未成功执行）",
            )

        logger.info("[test] 解析 CI 结果, run_id=%s", run_id)

        # ── 解析测试结果 ──
        try:
            results: dict[str, Any] = await self.ci_trigger.parse_test_results(run_id)
        except Exception as e:
            duration = time.time() - start
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=duration,
                artifacts={"run_id": run_id},
                error=f"解析 CI 结果失败: {e}",
            )

        duration = time.time() - start

        # 提取各 job 结果
        build_test = results.get("build_test", {})
        lua_lint = results.get("lua_lint", {})
        valgrind = results.get("valgrind", {})
        smoke = results.get("smoke", {})

        bt_status = build_test.get("status", "unknown")
        bt_ctest_pass = build_test.get("ctest_pass", 0)
        bt_ctest_fail = build_test.get("ctest_fail", 0)

        lint_status = lua_lint.get("status", "unknown")
        lint_errors = lua_lint.get("errors", 0)

        vg_status = valgrind.get("status", "unknown")
        vg_leaks = valgrind.get("leaks", 0)

        smoke_status = smoke.get("status", "unknown")

        # ── 构建断言 ──
        assertions: list[AssertionResult] = []

        # 断言1: build-and-test job 通过
        bt_passed = bt_status in ("success", "passed", "completed")
        assertions.append(AssertionResult(
            name="build_test_job",
            severity=AssertionSeverity.CRITICAL,
            passed=bt_passed,
            actual_value=bt_status,
            expected_value="success",
            message=f"build-and-test job 状态={bt_status}",
        ))

        # 断言2: ctest 通过数 >= 18
        ctest_ok = bt_ctest_pass >= _CTEST_MIN_PASS
        assertions.append(AssertionResult(
            name="ctest_pass_count",
            severity=AssertionSeverity.CRITICAL,
            passed=ctest_ok,
            actual_value=f"{bt_ctest_pass} passed, {bt_ctest_fail} failed",
            expected_value=f">= {_CTEST_MIN_PASS} passed",
            message=f"ctest: {bt_ctest_pass} passed / {bt_ctest_fail} failed (要求 >= {_CTEST_MIN_PASS})",
        ))

        # 断言3: 冒烟测试通过
        smoke_passed = smoke_status in ("success", "passed", "completed")
        assertions.append(AssertionResult(
            name="smoke_test",
            severity=AssertionSeverity.CRITICAL,
            passed=smoke_passed,
            actual_value=smoke_status,
            expected_value="success",
            message=f"冒烟测试状态={smoke_status}",
        ))

        # 断言4: lua-lint 通过
        lint_passed = lint_status in ("success", "passed", "completed") and lint_errors == 0
        assertions.append(AssertionResult(
            name="lua_lint",
            severity=AssertionSeverity.WARNING,
            passed=lint_passed,
            actual_value=f"status={lint_status}, errors={lint_errors}",
            expected_value="success, 0 errors",
            message=f"lua-lint: status={lint_status}, errors={lint_errors}",
        ))

        # 断言5: valgrind 通过或跳过
        vg_passed = vg_status in ("success", "passed", "completed", "skipped")
        assertions.append(AssertionResult(
            name="valgrind",
            severity=AssertionSeverity.WARNING,
            passed=vg_passed,
            actual_value=f"status={vg_status}, leaks={vg_leaks}",
            expected_value="success or skipped",
            message=f"valgrind: status={vg_status}, leaks={vg_leaks}",
        ))

        # ── 判定最终状态 ──
        all_passed = all(a.passed for a in assertions)

        # ── 拉取失败 job 日志 ──
        failed_job_logs: dict[str, str] = {}
        if not all_passed:
            # 对失败的 job 拉取日志
            job_name_map = {
                "build_test_job": "build-and-test",
                "smoke_test": "build-and-test",  # 冒烟在 build-and-test 内
                "lua_lint": "lua-lint",
                "valgrind": "memcheck",
            }
            for assertion in assertions:
                if not assertion.passed and assertion.name in job_name_map:
                    job_name = job_name_map[assertion.name]
                    if job_name not in failed_job_logs:
                        try:
                            log_text = await self.ci_trigger.fetch_job_logs(run_id, job_name)
                            failed_job_logs[job_name] = log_text[-3000:] if log_text else ""
                        except Exception:
                            failed_job_logs[job_name] = ""

        error_msg = None
        if not all_passed:
            failed_names = [a.name for a in assertions if not a.passed]
            error_msg = f"CI 测试失败: {failed_names}"

        return LoopResult(
            status=LoopStatus.PASSED if all_passed else LoopStatus.FAILED,
            retry_count=0,
            duration_sec=duration,
            artifacts={
                "run_id": run_id,
                "job_results": results,
                "failed_job_logs": failed_job_logs,
                "summary": {
                    "build_test": f"{bt_status} (ctest {bt_ctest_pass}/{bt_ctest_pass + bt_ctest_fail})",
                    "smoke": smoke_status,
                    "lua_lint": f"{lint_status} ({lint_errors} errors)",
                    "valgrind": f"{vg_status} ({vg_leaks} leaks)",
                },
            },
            assertions=assertions,
            error=error_msg,
        )

    async def retry(self, context: LoopContext, reason: str) -> LoopResult:
        """重试 CI。

        v0.1 简化版：直接重新触发 CI（不含 AI 分析失败原因）。

        Args:
            context: 闭环执行上下文。
            reason: 上次失败的原因。

        Returns:
            LoopResult: 重试执行结果。
        """
        logger.info("[test] 重试 CI，原因: %s", reason)
        # 重置 run_id，重新走 execute 流程
        self._last_run_id = None
        return await self.execute(context)
