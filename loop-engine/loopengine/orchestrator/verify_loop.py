"""验证闭环 - spec 3.4。

启动全集群 + 冒烟测试 + Admin API 查询，验证引擎端到端运行正常。
执行结束后（无论成功失败）调 stop_cluster() 清理。
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any

from ..adapter.admin_query import AdminQuery
from ..adapter.proc_manager import ProcessManager
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

# 集群应有的 5 个服务
_REQUIRED_SERVICES = ["dbproxy", "game", "router", "gateway", "admin"]


class VerifyLoop(LoopDomain):
    """验证闭环域 - 集群启动 + 冒烟测试 + Admin 查询。

    流程:
        1. start_cluster() 启动全集群
        2. wait_for_services(timeout=30) 等待就绪
        3. test_client.sh TCP/WS/Admin 冒烟
        4. verify_client_sync.sh 客户端同步验证
        5. admin_query.get_stats()/get_aoi()/get_cell() 查询数据一致性
    执行结束后调 stop_cluster() 清理。
    """

    @property
    def name(self) -> str:
        return "verify"

    @property
    def max_retries(self) -> int:
        return 3

    def __init__(
        self,
        proc_manager: ProcessManager,
        script_exec: ScriptExec,
        admin_query: AdminQuery,
    ) -> None:
        """初始化验证闭环。

        Args:
            proc_manager: 集群进程管理器。
            script_exec: 脚本执行器。
            admin_query: Admin API 查询器。
        """
        self.proc_manager = proc_manager
        self.script_exec = script_exec
        self.admin_query = admin_query
        # W6: 标记集群是否由 execute() 启动且仍在运行，供 verify() 复用
        self._cluster_running: bool = False

    async def execute(self, context: LoopContext) -> LoopResult:
        """启动集群并执行冒烟测试。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 执行结果。
        """
        start = time.time()
        logger.info("[verify] 启动集群并执行冒烟测试")

        # ── 启动集群 ──
        cluster_started = await asyncio.to_thread(self.proc_manager.start_cluster)
        if not cluster_started:
            duration = time.time() - start
            await self._cleanup()
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=duration,
                artifacts={"cluster_started": False},
                error="启动集群脚本失败 (start_cluster.sh 返回非零)",
            )

        # 集群已启动，标记为运行中（供 verify() 复用）
        self._cluster_running = True

        # ── 等待服务就绪 ──
        services_ready = await asyncio.to_thread(
            self.proc_manager.wait_for_services, 30
        )
        if not services_ready:
            duration = time.time() - start
            status = await asyncio.to_thread(self.proc_manager.get_status)
            # W6: 失败路径清理集群，成功路径保留供 verify() 使用
            await self._cleanup()
            self._cluster_running = False
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=duration,
                artifacts={
                    "services_ready": False,
                    "cluster_status": status,
                },
                error="集群服务 30s 内未就绪",
            )

        logger.info("[verify] 集群已就绪，执行冒烟测试")

        try:
            # ── 冒烟测试 ──
            smoke_result: ScriptResult = await asyncio.to_thread(
                self.script_exec.run, "test_client.sh", timeout=60
            )

            # ── 客户端同步验证 ──
            sync_result: ScriptResult = await asyncio.to_thread(
                self.script_exec.run, "verify_client_sync.sh", timeout=60
            )

            # ── Admin API 查询 ──
            stats = await self.admin_query.get_stats()
            aoi = await self.admin_query.get_aoi()
            cell = await self.admin_query.get_cell()

            duration = time.time() - start

            # 判定状态：冒烟测试和同步验证都通过
            smoke_ok = smoke_result.success
            sync_ok = sync_result.success
            all_ok = smoke_ok and sync_ok

            # W6: 执行失败时清理集群，成功时保留供 verify() 使用
            if not all_ok:
                await self._cleanup()
                self._cluster_running = False

            return LoopResult(
                status=LoopStatus.PASSED if all_ok else LoopStatus.FAILED,
                duration_sec=duration,
                artifacts={
                    "cluster_started": True,
                    "services_ready": True,
                    "smoke_exit_code": smoke_result.exit_code,
                    "smoke_stdout": smoke_result.stdout[-1000:],
                    "smoke_stderr": smoke_result.stderr[-1000:],
                    "sync_exit_code": sync_result.exit_code,
                    "sync_stdout": sync_result.stdout[-1000:],
                    "sync_stderr": sync_result.stderr[-1000:],
                    "admin_stats": stats,
                    "admin_aoi": aoi,
                    "admin_cell": cell,
                },
                error=None if all_ok else (
                    f"冒烟测试{'通过' if smoke_ok else '失败'} "
                    f"同步验证{'通过' if sync_ok else '失败'}"
                ),
            )
        except Exception as e:
            duration = time.time() - start
            logger.error("[verify] 执行阶段异常: %s", e)
            # W6: 异常路径清理集群
            await self._cleanup()
            self._cluster_running = False
            return LoopResult(
                status=LoopStatus.FAILED,
                duration_sec=duration,
                artifacts={"cluster_started": True, "exec_error": str(e)},
                error=f"执行阶段异常: {e}",
            )
        # W6: 移除 finally 中的无条件 _cleanup()，成功时保留集群供 verify() 使用

    async def verify(self, context: LoopContext) -> LoopResult:
        """验证集群状态和数据一致性。

        断言:
            - 集群 5 服务全 running
            - 冒烟测试通过
            - Admin API 返回有效数据
            - 实体同步正常

        W6: execute() 成功后集群仍在运行，verify() 直接复用已运行的集群，
        不再重新 start_cluster()。仅在集群未运行时才启动。
        验证结束或异常时在 finally 中清理集群。

        Args:
            context: 闭环执行上下文。

        Returns:
            LoopResult: 验证结果。
        """
        start = time.time()
        logger.info("[verify] 验证集群状态和数据一致性")

        # W6: 复用 execute() 已启动的集群，仅在未运行时才启动
        if not self._cluster_running:
            cluster_started = await asyncio.to_thread(self.proc_manager.start_cluster)
            if not cluster_started:
                duration = time.time() - start
                return LoopResult(
                    status=LoopStatus.FAILED,
                    duration_sec=duration,
                    error="验证阶段启动集群失败",
                )
            self._cluster_running = True

        services_ready = await asyncio.to_thread(
            self.proc_manager.wait_for_services, 30
        )

        try:
            # 获取集群状态
            cluster_status = await asyncio.to_thread(self.proc_manager.get_status)
            services_list = cluster_status.get("services", [])

            # 构建 {service_name: status} 映射
            svc_status_map: dict[str, str] = {}
            for svc in services_list:
                svc_status_map[svc.get("name", "")] = svc.get("status", "unknown")

            # Admin API 查询
            stats = await self.admin_query.get_stats()
            aoi = await self.admin_query.get_aoi()
            cell = await self.admin_query.get_cell()

            duration = time.time() - start

            # ── 构建断言 ──
            assertions: list[AssertionResult] = []

            # 断言1: 5 个服务全 running
            all_running = all(
                svc_status_map.get(svc, "unknown") == "running"
                for svc in _REQUIRED_SERVICES
            )
            assertions.append(AssertionResult(
                name="cluster_all_running",
                severity=AssertionSeverity.CRITICAL,
                passed=all_running and services_ready,
                actual_value=svc_status_map,
                expected_value={svc: "running" for svc in _REQUIRED_SERVICES},
                message=(
                    "所有 5 服务 running" if all_running
                    else f"服务状态: {svc_status_map}"
                ),
            ))

            # 断言2: 冒烟测试通过（重新执行）
            smoke_result: ScriptResult = await asyncio.to_thread(
                self.script_exec.run, "test_client.sh", timeout=60
            )
            assertions.append(AssertionResult(
                name="smoke_test_passed",
                severity=AssertionSeverity.CRITICAL,
                passed=smoke_result.success,
                actual_value=smoke_result.exit_code,
                expected_value=0,
                message=f"冒烟测试 exit_code={smoke_result.exit_code}",
            ))

            # 断言3: Admin API 返回有效数据
            stats_valid = bool(stats) and "fps" in stats
            assertions.append(AssertionResult(
                name="admin_api_valid",
                severity=AssertionSeverity.CRITICAL,
                passed=stats_valid,
                actual_value={"has_stats": bool(stats), "keys": list(stats.keys())[:10]},
                expected_value="非空 dict 且包含 fps 字段",
                message=f"Admin API stats: {'有效' if stats_valid else '无效'}",
            ))

            # 断言4: 实体同步正常（AOI 有实体数据）
            entity_sync_ok = bool(aoi) and (
                aoi.get("entity_count", 0) > 0
                or len(aoi.get("entities", [])) > 0
                or aoi.get("total", 0) > 0
            )
            assertions.append(AssertionResult(
                name="entity_sync_normal",
                severity=AssertionSeverity.WARNING,
                passed=entity_sync_ok,
                actual_value={"aoi": aoi, "cell": cell},
                expected_value="AOI 返回有效实体数据",
                message=f"实体同步: {'正常' if entity_sync_ok else '异常 (无实体数据)'}",
            ))

            all_passed = all(a.passed for a in assertions)
            failed_names = [a.name for a in assertions if not a.passed]

            return LoopResult(
                status=LoopStatus.PASSED if all_passed else LoopStatus.FAILED,
                duration_sec=duration,
                artifacts={
                    "cluster_status": cluster_status,
                    "svc_status_map": svc_status_map,
                    "admin_stats": stats,
                    "admin_aoi": aoi,
                    "admin_cell": cell,
                    "smoke_exit_code": smoke_result.exit_code,
                },
                assertions=assertions,
                error=None if all_passed else f"验证失败: {failed_names}",
            )
        finally:
            # W6: 无论验证结果如何，清理集群并重置标志
            await self._cleanup()
            self._cluster_running = False

    async def retry(self, context: LoopContext, reason: str) -> LoopResult:
        """重试验证 - 重启集群 + 重跑。

        Args:
            context: 闭环执行上下文。
            reason: 上次失败的原因。

        Returns:
            LoopResult: 重试执行结果。
        """
        logger.info("[verify] 重试验证，原因: %s", reason)
        # 确保旧集群已清理
        await self._cleanup()
        self._cluster_running = False
        return await self.execute(context)

    async def _cleanup(self) -> None:
        """清理集群资源。"""
        try:
            await asyncio.to_thread(self.proc_manager.stop_cluster)
        except Exception as e:
            logger.warning("[verify] stop_cluster 异常: %s", e)
