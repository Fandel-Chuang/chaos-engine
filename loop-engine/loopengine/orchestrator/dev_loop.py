"""开发闭环 - spec 3.2。

v0.1 简化版：仅编译验证，不含 AI 写码。
通过 ScriptExec 执行 build_and_test.sh --no-test，验证编译产物生成。
"""

from __future__ import annotations

import asyncio
import logging
import time
from pathlib import Path

from ..adapter.script_exec import ScriptExec, ScriptResult
from ..data.models import (
    AssertionResult,
    AssertionSeverity,
    LoopContext,
    LoopResult,
    LoopStatus,
)
from .loop_state import LoopDomain

logger = logging.getLogger(__name__)

# 预期编译产出的二进制文件列表（相对 chaos_engine_dir）
_EXPECTED_BINARIES = [
    "build/bin/chaos_server",
    "build/bin/chaos_client",
    "build/bin/chaos_headless",
    "build/bin/chaos_gateway",
    "build/bin/chaos_router",
    "build/bin/chaos_dbproxy",
]


class DevLoop(LoopDomain):
    """开发闭环域 - 编译验证。

    v0.1 简化版：执行 build_and_test.sh --no-test 完成编译，
    验证 6 个二进制文件生成。编译失败直接返回 FAILED + stderr，不含 AI 写码。
    """

    @property
    def name(self) -> str:
        return "dev"

    @property
    def max_retries(self) -> int:
        return 3

    def __init__(self, script_exec: ScriptExec) -> None:
        """初始化开发闭环。

        Args:
            script_exec: 脚本执行器，用于运行 build_and_test.sh。
        """
        self.script_exec = script_exec

    async def execute(self, context: LoopContext) -> LoopResult:
        """执行编译。

        调用 build_and_test.sh --no-test，判定 exit_code==0。
        断言：compile_exit_code==0、6 个二进制文件均存在。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 编译执行结果。
        """
        start = time.time()
        logger.info("[dev] 开始编译 (build_and_test.sh --no-test)")

        # ScriptExec.run 是同步阻塞调用，放到线程池执行
        result: ScriptResult = await asyncio.to_thread(
            self.script_exec.run,
            "build_and_test.sh",
            args=["--no-test"],
            timeout=300,
        )

        duration = time.time() - start

        # 构建断言结果
        assertions: list[AssertionResult] = []

        # 断言1: 编译退出码为 0
        assertions.append(AssertionResult(
            name="compile_exit_code",
            severity=AssertionSeverity.CRITICAL,
            passed=result.exit_code == 0,
            actual_value=result.exit_code,
            expected_value=0,
            message=f"build_and_test.sh 退出码={result.exit_code}",
        ))

        # 断言2: 检查二进制文件是否存在
        chaos_dir = Path(context.chaos_engine_dir)
        missing_binaries = []
        for binary_path in _EXPECTED_BINARIES:
            full_path = chaos_dir / binary_path
            if not full_path.exists():
                missing_binaries.append(binary_path)

        all_binaries_exist = len(missing_binaries) == 0
        assertions.append(AssertionResult(
            name="binary_exists",
            severity=AssertionSeverity.CRITICAL,
            passed=all_binaries_exist,
            actual_value={
                "missing": missing_binaries,
                "checked": _EXPECTED_BINARIES,
            },
            expected_value="所有 6 个二进制文件存在",
            message=(
                "所有二进制文件存在" if all_binaries_exist
                else f"缺失二进制: {missing_binaries}"
            ),
        ))

        # 判定最终状态
        all_passed = result.success and all_binaries_exist

        return LoopResult(
            status=LoopStatus.PASSED if all_passed else LoopStatus.FAILED,
            retry_count=0,
            duration_sec=duration,
            artifacts={
                "exit_code": result.exit_code,
                "stdout": result.stdout[-2000:] if result.stdout else "",  # 截断防止过大
                "stderr": result.stderr[-2000:] if result.stderr else "",
                "missing_binaries": missing_binaries,
                "expected_binaries": _EXPECTED_BINARIES,
            },
            assertions=assertions,
            error=None if all_passed else f"编译失败 (exit={result.exit_code}): {result.stderr[:500]}",
        )

    async def verify(self, context: LoopContext) -> LoopResult:
        """验证编译产物是否真的生成。

        独立检查 6 个二进制文件是否存在于 chaos_engine_dir/build/ 下，
        不依赖 execute 阶段的缓存结果。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 验证结果。
        """
        start = time.time()
        logger.info("[dev] 验证编译产物")

        chaos_dir = Path(context.chaos_engine_dir)
        missing_binaries = []
        binary_sizes: dict[str, int] = {}

        for binary_path in _EXPECTED_BINARIES:
            full_path = chaos_dir / binary_path
            if full_path.exists():
                binary_sizes[binary_path] = full_path.stat().st_size
            else:
                missing_binaries.append(binary_path)

        duration = time.time() - start
        all_exist = len(missing_binaries) == 0

        assertions: list[AssertionResult] = []
        assertions.append(AssertionResult(
            name="verify_binary_exists",
            severity=AssertionSeverity.CRITICAL,
            passed=all_exist,
            actual_value={
                "missing": missing_binaries,
                "sizes": binary_sizes,
            },
            expected_value="所有 6 个二进制文件存在且大小 > 0",
            message=(
                "所有二进制文件验证通过" if all_exist
                else f"验证失败，缺失: {missing_binaries}"
            ),
        ))

        # 检查文件大小 > 0（空文件也算异常）
        zero_size = [p for p, s in binary_sizes.items() if s == 0]
        if zero_size:
            assertions.append(AssertionResult(
                name="binary_size_nonzero",
                severity=AssertionSeverity.WARNING,
                passed=False,
                actual_value=zero_size,
                expected_value="所有二进制文件大小 > 0",
                message=f"大小为 0 的二进制: {zero_size}",
            ))
            all_exist = False

        return LoopResult(
            status=LoopStatus.PASSED if all_exist else LoopStatus.FAILED,
            retry_count=0,
            duration_sec=duration,
            artifacts={
                "missing_binaries": missing_binaries,
                "binary_sizes": binary_sizes,
            },
            assertions=assertions,
            error=None if all_exist else f"验证失败，缺失二进制: {missing_binaries}",
        )

    async def retry(self, context: LoopContext, reason: str) -> LoopResult:
        """重试编译。

        v0.1 简化版：直接重新执行 execute（不含 AI 写码修复）。

        Args:
            context: 闭环执行上下文。
            reason: 上次失败的原因。

        Returns:
            LoopResult: 重试执行结果。
        """
        logger.info("[dev] 重试编译，原因: %s", reason)
        return await self.execute(context)
