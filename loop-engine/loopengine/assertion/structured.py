"""结构化断言引擎 - spec 4.3.2。

对 ClientDataCollector 采集的三路快照（标题栏/日志/进程）执行
11 条默认断言，输出 AssertionReport 列表。

断言分级：
  CRITICAL（4条）: 进程存活、窗口存在、Vulkan 初始化、窗口尺寸 1280x720
  WARNING（6条） : 标题栏坐标、出生点半径、网络连接、实体同步、可见实体、网络错误
  INFO（1条）    : 标题栏 keys 字段

判定逻辑：
  - CRITICAL 失败 -> overall_result = FAILED（直接判负，无需视觉复核）
  - WARNING 失败且无 CRITICAL 失败 -> 触发视觉复核
  - 全部通过 -> PASSED
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable

# 复用 data/models.py 中的 AssertionSeverity，避免定义冲突
from ..data.models import AssertionSeverity


class AssertionVerdict(Enum):
    """断言执行结果（枚举）。

    注意：与 data/models.py 中的 AssertionResult(dataclass) 同名但不同类型，
    此处重命名为 AssertionVerdict 以消除名称冲突。
    """

    PASSED = "passed"
    FAILED = "failed"
    SKIPPED = "skipped"


@dataclass
class Assertion:
    """单条断言规则。

    Attributes:
        name: 断言名称（唯一标识）。
        description: 断言描述（用于报告展示）。
        severity: 严重级别（CRITICAL / WARNING / INFO）。
        check: 判定函数，接收 context dict，返回 bool。
        extract: 提取函数，从 context 提取断言相关数据用于报告。
    """

    name: str
    description: str
    severity: AssertionSeverity
    check: Callable[[dict], bool]
    extract: Callable[[dict], Any]


@dataclass
class AssertionReport:
    """断言执行报告。

    Attributes:
        name: 断言名称。
        description: 断言描述。
        severity: 严重级别。
        result: 执行结果（PASSED / FAILED / SKIPPED）。
        actual_value: 实际采集值。
        expected: 期望值描述。
        timestamp: 执行时间戳。
    """

    name: str
    description: str
    severity: AssertionSeverity
    result: AssertionVerdict
    actual_value: Any
    expected: str
    timestamp: float


class ClientAssertionEngine:
    """客户端断言引擎。

    注册默认断言规则，对采集数据执行批量判定，
    并决定是否触发视觉复核。
    """

    def __init__(self) -> None:
        """初始化断言引擎，注册默认断言。"""
        self.assertions: list[Assertion] = []
        self._setup_default_assertions()

    def _setup_default_assertions(self) -> None:
        """注册默认断言规则（11条）。

        CRITICAL（4条）: process_alive / window_exists / vulkan_initialized / window_size
        WARNING（6条） : title_bar_has_pos / spawn_within_radius / network_connected /
                         entity_update_received / visible_entities_positive / no_network_error
        INFO（1条）    : title_bar_keys_responsive
        """

        # === CRITICAL 断言（失败直接判负） ===

        self.assertions.append(
            Assertion(
                name="process_alive",
                description="客户端进程存活",
                severity=AssertionSeverity.CRITICAL,
                check=lambda ctx: ctx["process"].process_alive,
                extract=lambda ctx: ctx["process"].process_alive,
            )
        )

        self.assertions.append(
            Assertion(
                name="window_exists",
                description="Vulkan 窗口已创建",
                severity=AssertionSeverity.CRITICAL,
                check=lambda ctx: ctx["process"].window_exists,
                extract=lambda ctx: ctx["process"].window_exists,
            )
        )

        self.assertions.append(
            Assertion(
                name="vulkan_initialized",
                description="Vulkan 设备初始化成功",
                severity=AssertionSeverity.CRITICAL,
                check=lambda ctx: ctx["log"].vulkan_initialized,
                extract=lambda ctx: ctx["log"].vulkan_initialized,
            )
        )

        self.assertions.append(
            Assertion(
                name="window_size",
                description="窗口尺寸为 1280x720",
                severity=AssertionSeverity.CRITICAL,
                check=lambda ctx: (
                    ctx["process"].window_width == 1280
                    and ctx["process"].window_height == 720
                ),
                extract=lambda ctx: (
                    f"{ctx['process'].window_width}x{ctx['process'].window_height}"
                ),
            )
        )

        # === WARNING 断言（失败触发视觉复核） ===

        self.assertions.append(
            Assertion(
                name="title_bar_has_pos",
                description="标题栏包含坐标信息",
                severity=AssertionSeverity.WARNING,
                check=lambda ctx: ctx["title"].pos_x is not None,
                extract=lambda ctx: ctx["title"].raw_text,
            )
        )

        self.assertions.append(
            Assertion(
                name="spawn_within_radius",
                description="出生点在 spawn 半径内 (r=2.5)",
                severity=AssertionSeverity.WARNING,
                check=lambda ctx: (
                    ctx["log"].spawn_pos is not None
                    and (
                        ctx["log"].spawn_pos[0] ** 2
                        + ctx["log"].spawn_pos[2] ** 2
                    )
                    <= 2.5 ** 2
                ),
                extract=lambda ctx: ctx["log"].spawn_pos,
            )
        )

        self.assertions.append(
            Assertion(
                name="network_connected",
                description="客户端已连接服务器 (标题栏非 disconnected)",
                severity=AssertionSeverity.WARNING,
                check=lambda ctx: ctx["title"].connected is True,
                extract=lambda ctx: ctx["title"].connected,
            )
        )

        self.assertions.append(
            Assertion(
                name="entity_update_received",
                description="收到实体同步消息 (ENTITY_UPDATE)",
                severity=AssertionSeverity.WARNING,
                check=lambda ctx: ctx["log"].entity_update_received,
                extract=lambda ctx: ctx["log"].entity_update_received,
            )
        )

        self.assertions.append(
            Assertion(
                name="visible_entities_positive",
                description="可见实体数 > 0 (连接后)",
                severity=AssertionSeverity.WARNING,
                check=lambda ctx: (
                    ctx["title"].connected
                    and ctx["title"].visible is not None
                    and ctx["title"].visible > 0
                ),
                extract=lambda ctx: ctx["title"].visible,
            )
        )

        self.assertions.append(
            Assertion(
                name="no_network_error",
                description="无网络错误日志",
                severity=AssertionSeverity.WARNING,
                check=lambda ctx: not ctx["log"].network_error,
                extract=lambda ctx: ctx["log"].network_error,
            )
        )

        # === INFO 断言（仅记录） ===

        self.assertions.append(
            Assertion(
                name="title_bar_keys_responsive",
                description="标题栏 keys 字段存在 (输入系统工作)",
                severity=AssertionSeverity.INFO,
                check=lambda ctx: ctx["title"].keys is not None,
                extract=lambda ctx: ctx["title"].keys,
            )
        )

    def run_all(self, context: dict) -> list[AssertionReport]:
        """执行所有断言。

        Args:
            context: 采集上下文，需包含以下键：
                - ``title``: TitleBarSnapshot
                - ``log``: LogSnapshot
                - ``process``: ProcessSnapshot

        Returns:
            list[AssertionReport]: 断言报告列表，顺序与注册顺序一致。
        """
        reports: list[AssertionReport] = []

        for assertion in self.assertions:
            try:
                passed = assertion.check(context)
                actual = assertion.extract(context)
            except Exception as e:
                # 断言执行异常，标记为失败并记录错误信息
                passed = False
                actual = f"ERROR: {e}"

            reports.append(
                AssertionReport(
                    name=assertion.name,
                    description=assertion.description,
                    severity=assertion.severity,
                    result=AssertionVerdict.PASSED if passed else AssertionVerdict.FAILED,
                    actual_value=actual,
                    expected=assertion.description,
                    timestamp=time.time(),
                )
            )

        return reports

    def should_trigger_vision_review(
        self, reports: list[AssertionReport]
    ) -> bool:
        """根据断言结果决定是否触发视觉复核。

        触发条件：
        - 无 CRITICAL 失败（CRITICAL 失败直接判负，截图无意义）
        - 有 WARNING 失败

        Args:
            reports: run_all 返回的断言报告列表。

        Returns:
            bool: 是否应触发视觉复核。
        """
        has_critical_fail = any(
            r.result == AssertionVerdict.FAILED
            and r.severity == AssertionSeverity.CRITICAL
            for r in reports
        )
        has_warning_fail = any(
            r.result == AssertionVerdict.FAILED
            and r.severity == AssertionSeverity.WARNING
            for r in reports
        )
        return has_warning_fail and not has_critical_fail

    def overall_result(self, reports: list[AssertionReport]) -> AssertionVerdict:
        """总体判定。

        CRITICAL 失败 -> FAILED；否则 -> PASSED（WARNING 失败交给视觉复核）。

        Args:
            reports: 断言报告列表。

        Returns:
            AssertionVerdict: FAILED 或 PASSED。
        """
        for r in reports:
            if r.result == AssertionVerdict.FAILED:
                if r.severity == AssertionSeverity.CRITICAL:
                    return AssertionVerdict.FAILED
        # 无 CRITICAL 失败 -> PASSED（WARNING 失败交给视觉复核决定）
        return AssertionVerdict.PASSED
