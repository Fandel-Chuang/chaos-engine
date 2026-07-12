"""数据模型 — spec 3.1 节定义。"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from typing import Any


class LoopStatus(Enum):
    """闭环执行状态。"""

    PENDING = "pending"
    RUNNING = "running"
    VERIFYING = "verifying"
    PASSED = "passed"
    FAILED = "failed"
    RETRYING = "retrying"


class AssertionSeverity(Enum):
    """断言严重级别。"""

    CRITICAL = "critical"
    WARNING = "warning"
    INFO = "info"


@dataclass
class LoopResult:
    """单次循环执行结果。"""

    status: LoopStatus = LoopStatus.PENDING
    retry_count: int = 0
    duration_sec: float = 0.0
    artifacts: dict[str, Any] = field(default_factory=dict)
    assertions: list[AssertionResult] = field(default_factory=list)
    ai_analysis: str | None = None
    error: str | None = None


@dataclass
class LoopContext:
    """单次循环执行上下文。"""

    chaos_engine_dir: str
    build_dir: str
    config: dict[str, Any] = field(default_factory=dict)
    loop_id: str = ""
    timestamp: datetime = field(default_factory=datetime.now)


@dataclass
class AssertionResult:
    """单条断言结果。"""

    name: str
    severity: AssertionSeverity = AssertionSeverity.INFO
    passed: bool = False
    actual_value: Any = None
    expected_value: Any = None
    message: str = ""


@dataclass
class LoopExecution:
    """一次完整的闭环执行记录（持久化单元）。"""

    loop_id: str
    task: str
    started_at: datetime = field(default_factory=datetime.now)
    finished_at: datetime | None = None
    status: LoopStatus = LoopStatus.PENDING
    domains: list[dict[str, Any]] = field(default_factory=list)
    total_cost_cny: float = 0.0
    report_path: str = ""
