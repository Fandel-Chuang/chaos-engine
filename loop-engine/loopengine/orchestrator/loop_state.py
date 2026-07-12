"""闭环状态机 - spec 3.1。

定义闭环域（LoopDomain）抽象基类和状态机（LoopStateMachine），
管理单个闭环域从 PENDING -> RUNNING -> VERIFYING -> PASSED/FAILED/RETRYING 的流转。
"""

from __future__ import annotations

import logging
import time
from abc import ABC, abstractmethod

from ..data.models import LoopContext, LoopResult, LoopStatus

logger = logging.getLogger(__name__)


class LoopDomain(ABC):
    """闭环域抽象基类。

    每个具体的闭环域（dev / test / verify / client_ui）继承此类，
    实现 execute / verify / retry 三个阶段。
    """

    @property
    @abstractmethod
    def name(self) -> str:
        """闭环域名称（如 "dev" / "test" / "verify" / "client_ui"）。"""

    @property
    @abstractmethod
    def max_retries(self) -> int:
        """最大重试次数。"""

    @abstractmethod
    async def execute(self, context: LoopContext) -> LoopResult:
        """执行阶段 - 运行主要工作流程。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 执行结果（status 通常为 PASSED 或 FAILED）。
        """

    @abstractmethod
    async def verify(self, context: LoopContext) -> LoopResult:
        """验证阶段 - 检查执行结果是否满足断言。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 验证结果（status 为 PASSED 或 FAILED）。
        """

    @abstractmethod
    async def retry(self, context: LoopContext, reason: str) -> LoopResult:
        """重试阶段 - 重新执行闭环。

        Args:
            context: 闭环执行上下文。
            reason: 上次失败的原因。

        Returns:
            LoopResult: 重试执行结果。
        """


class LoopStateMachine:
    """闭环状态机 - 管理单个闭环域的状态流转。

    状态流转逻辑::

        PENDING -> RUNNING(execute) -> VERIFYING(verify)
          -> PASSED   (验证通过，返回)
          -> FAILED   (验证失败)
             -> retry_count < max_retries: RETRYING -> RUNNING(retry) -> ...
             -> retry_count >= max_retries: FAILED(final)

    每次重试记录原因和耗时，最终 LoopResult 汇总所有信息。
    """

    def __init__(self, domain: LoopDomain, max_retries: int = 3) -> None:
        """初始化状态机。

        Args:
            domain: 要管理的闭环域。
            max_retries: 最大重试次数（覆盖 domain.max_retries）。
        """
        self.domain = domain
        self.max_retries = max_retries
        # 重试历史记录：每次元素为 {"reason": str, "duration_sec": float}
        self._retry_history: list[dict] = []

    async def run(self, context: LoopContext) -> LoopResult:
        """执行完整闭环流程。

        依次执行 execute -> verify，根据验证结果决定 PASSED / FAILED / RETRYING。
        失败时在重试限额内自动重跑，记录每次重试的原因和耗时。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 最终执行结果，包含状态、重试次数、总耗时、断言列表等。
        """
        total_start = time.time()
        retry_count = 0
        last_result: LoopResult | None = None
        last_error: str | None = None

        while True:
            # ── 执行阶段 ──
            logger.info("[%s] 执行阶段 (retry=%d/%d)", self.domain.name, retry_count, self.max_retries)

            try:
                if retry_count == 0:
                    exec_result = await self.domain.execute(context)
                else:
                    # 记录本次 retry() 调用前的时间戳，用于更新对应历史记录的真实耗时
                    retry_start = time.time()
                    exec_result = await self.domain.retry(context, last_error or "未知原因")
                    retry_duration = time.time() - retry_start

                    # 更新最近一条重试历史记录的真实耗时（该记录在进入重试时已追加）
                    if self._retry_history:
                        self._retry_history[-1]["duration_sec"] = round(retry_duration, 3)
            except Exception as e:
                # W2: 顶层 try-except，防止 execute()/verify()/retry() 抛未处理异常导致 run() 崩溃
                logger.exception("[%s] 闭环执行异常: %s", self.domain.name, e)
                total_duration = time.time() - total_start
                return LoopResult(
                    status=LoopStatus.FAILED,
                    retry_count=retry_count,
                    duration_sec=total_duration,
                    artifacts={
                        "retry_history": list(self._retry_history),
                    },
                    error=f"闭环执行异常: {e}",
                )

            if exec_result.status == LoopStatus.PASSED:
                # 执行成功，进入验证阶段
                logger.info("[%s] 执行通过，进入验证阶段", self.domain.name)
                try:
                    verify_result = await self.domain.verify(context)
                except Exception as e:
                    # W2: verify() 异常同样捕获，返回 FAILED 而非崩溃
                    logger.exception("[%s] 验证阶段异常: %s", self.domain.name, e)
                    total_duration = time.time() - total_start
                    return LoopResult(
                        status=LoopStatus.FAILED,
                        retry_count=retry_count,
                        duration_sec=total_duration,
                        artifacts={
                            "exec_artifacts": exec_result.artifacts,
                            "retry_history": list(self._retry_history),
                        },
                        assertions=exec_result.assertions,
                        error=f"验证阶段异常: {e}",
                    )

                if verify_result.status == LoopStatus.PASSED:
                    # 验证通过，闭环成功
                    total_duration = time.time() - total_start
                    return LoopResult(
                        status=LoopStatus.PASSED,
                        retry_count=retry_count,
                        duration_sec=total_duration,
                        artifacts={
                            "exec_artifacts": exec_result.artifacts,
                            "verify_artifacts": verify_result.artifacts,
                            "retry_history": list(self._retry_history),
                        },
                        assertions=verify_result.assertions or exec_result.assertions,
                        ai_analysis=verify_result.ai_analysis or exec_result.ai_analysis,
                        error=None,
                    )
                else:
                    # 验证失败
                    last_result = verify_result
                    last_error = verify_result.error or "验证阶段失败"
                    logger.warning("[%s] 验证失败: %s", self.domain.name, last_error)
            else:
                # 执行阶段就失败了
                last_result = exec_result
                last_error = exec_result.error or "执行阶段失败"
                logger.warning("[%s] 执行失败: %s", self.domain.name, last_error)

            # ── 失败处理：判断是否重试 ──
            if retry_count < self.max_retries:
                retry_count += 1
                retry_reason = last_error
                logger.info("[%s] 进入重试 (第 %d 次)，原因: %s", self.domain.name, retry_count, retry_reason)

                # 记录重试原因（真实耗时在下一次循环 retry() 调用后更新）
                self._retry_history.append({
                    "reason": retry_reason,
                    "attempt": retry_count,
                    "duration_sec": 0.0,  # 占位，retry() 执行后更新
                })

                continue
            else:
                # 达到最大重试次数，返回 FAILED
                total_duration = time.time() - total_start
                logger.error(
                    "[%s] 达到最大重试次数 %d，最终失败", self.domain.name, self.max_retries
                )

                return LoopResult(
                    status=LoopStatus.FAILED,
                    retry_count=retry_count,
                    duration_sec=total_duration,
                    artifacts={
                        "last_result_artifacts": last_result.artifacts if last_result else {},
                        "retry_history": list(self._retry_history),
                    },
                    assertions=last_result.assertions if last_result else [],
                    ai_analysis=last_result.ai_analysis if last_result else None,
                    error=last_error,
                )
