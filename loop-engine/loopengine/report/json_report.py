"""JSON 报告生成器 - spec 7.2。

把 LoopExecution / LoopResult 序列化为结构化 JSON 报告。
"""

from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from typing import Any

from ..data.models import LoopExecution, LoopResult, LoopStatus


def _serialize(value: Any) -> Any:
    """递归把不可 JSON 序列化的对象转为可序列化的基本类型。"""
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, LoopStatus):
        return value.value
    if hasattr(value, "value") and hasattr(value, "name"):
        # Enum
        return value.value
    if isinstance(value, datetime):
        return value.isoformat()
    if isinstance(value, dict):
        return {str(k): _serialize(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_serialize(item) for item in value]
    if hasattr(value, "__dict__"):
        return _serialize(vars(value))
    return str(value)


class JsonReport:
    """JSON 报告生成器。

    内部用 dict 累积，render 时 json.dumps(indent=2, ensure_ascii=False)。

    用法::

        report = JsonReport()
        report.add_execution_summary(execution)
        report.add_loop_result(dev_result, domain_name="dev")
        text = report.render()
        report.save("report.json")
    """

    def __init__(self) -> None:
        """初始化 JSON 报告生成器。"""
        self._data: dict[str, Any] = {
            "generated_at": datetime.now().isoformat(),
            "summary": None,
            "domains": [],
        }

    def add_loop_result(self, result: LoopResult, domain_name: str) -> None:
        """添加单个闭环域结果。

        Args:
            result: 闭环域执行结果。
            domain_name: 域名称（dev / test / verify / client_ui）。
        """
        domain_entry: dict[str, Any] = {
            "name": domain_name,
            "status": _serialize(result.status),
            "retry_count": result.retry_count,
            "duration_sec": round(result.duration_sec, 3),
            "error": result.error,
            "ai_analysis": result.ai_analysis,
            "assertions": [
                {
                    "name": a.name,
                    "severity": _serialize(a.severity),
                    "passed": a.passed,
                    "actual_value": _serialize(a.actual_value),
                    "expected_value": _serialize(a.expected_value),
                    "message": a.message,
                }
                for a in result.assertions
            ],
            "artifacts": _serialize(result.artifacts),
        }
        self._data["domains"].append(domain_entry)

    def add_execution_summary(self, execution: LoopExecution) -> None:
        """添加执行总览。

        Args:
            execution: 完整的闭环执行记录。
        """
        self._data["summary"] = {
            "loop_id": execution.loop_id,
            "task": execution.task,
            "started_at": _serialize(execution.started_at),
            "finished_at": _serialize(execution.finished_at),
            "status": _serialize(execution.status),
            "total_cost_cny": round(execution.total_cost_cny, 4),
            "report_path": execution.report_path,
            "domains": [
                {
                    "name": d.get("name", ""),
                    "status": _serialize(d.get("status")),
                    "duration_sec": round(float(d.get("duration_sec", 0.0)), 3),
                    "retry_count": d.get("retry_count", 0),
                    "error": d.get("error"),
                }
                for d in execution.domains
            ],
        }

    def render(self) -> str:
        """生成 JSON 字符串。

        Returns:
            str: JSON 格式的报告字符串。
        """
        return json.dumps(self._data, indent=2, ensure_ascii=False, default=str)

    def save(self, path: str) -> None:
        """写入文件。

        Args:
            path: 输出文件路径。
        """
        content = self.render()
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(content, encoding="utf-8")
