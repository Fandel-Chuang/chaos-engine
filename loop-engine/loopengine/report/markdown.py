"""Markdown 报告生成器 - spec 7.2。

把 LoopExecution / LoopResult 渲染为可读的 Markdown 报告。
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
from typing import Any

from ..data.models import LoopExecution, LoopResult, LoopStatus


def _status_emoji(status: LoopStatus) -> str:
    """状态 -> emoji 标记。"""
    return {
        LoopStatus.PASSED: "✅",
        LoopStatus.FAILED: "❌",
        LoopStatus.RUNNING: "🔄",
        LoopStatus.VERIFYING: "🔍",
        LoopStatus.RETRYING: "🔁",
        LoopStatus.PENDING: "⏳",
    }.get(status, "❓")


def _status_label(status: LoopStatus | str) -> str:
    """状态 -> 中文标签。"""
    if isinstance(status, str):
        return status
    return {
        LoopStatus.PASSED: "通过",
        LoopStatus.FAILED: "失败",
        LoopStatus.RUNNING: "运行中",
        LoopStatus.VERIFYING: "验证中",
        LoopStatus.RETRYING: "重试中",
        LoopStatus.PENDING: "等待中",
    }.get(status, str(status.value))


def _fmt_duration(sec: float) -> str:
    """格式化耗时。"""
    if sec < 60:
        return f"{sec:.1f}s"
    m, s = divmod(int(sec), 60)
    return f"{m}m{s}s"


class MarkdownReport:
    """Markdown 报告生成器。

    用法::

        report = MarkdownReport(title="LoopEngine 验证报告")
        report.add_execution_summary(execution)
        report.add_loop_result(dev_result, domain_name="dev")
        text = report.render()
        report.save("report.md")
    """

    def __init__(self, title: str = "LoopEngine 验证报告") -> None:
        """初始化报告生成器。

        Args:
            title: 报告主标题。
        """
        self.title = title
        self._lines: list[str] = []

    # ── 基础构建方法 ──

    def add_header(self, title: str, level: int = 1) -> None:
        """添加标题。

        Args:
            title: 标题文本。
            level: 标题层级（1~6）。
        """
        level = max(1, min(6, level))
        self._lines.append(f"{'#' * level} {title}")
        self._lines.append("")

    def add_text(self, text: str) -> None:
        """添加段落文本。"""
        self._lines.append(text)
        self._lines.append("")

    def add_table(self, headers: list[str], rows: list[list[str]]) -> None:
        """添加表格。

        Args:
            headers: 表头列表。
            rows: 行数据列表（每行是字符串列表）。
        """
        if not headers:
            return

        # 表头
        self._lines.append("| " + " | ".join(headers) + " |")
        self._lines.append("| " + " | ".join(["---"] * len(headers)) + " |")

        # 数据行
        for row in rows:
            # 补齐列数
            padded = list(row) + [""] * (len(headers) - len(row))
            cells = [str(c).replace("|", "\\|").replace("\n", " ") for c in padded[:len(headers)]]
            self._lines.append("| " + " | ".join(cells) + " |")

        self._lines.append("")

    def add_code_block(self, code: str, lang: str = "") -> None:
        """添加代码块。"""
        self._lines.append(f"```{lang}")
        self._lines.append(code)
        self._lines.append("```")
        self._lines.append("")

    def add_image(self, path: str, alt: str = "截图") -> None:
        """添加图片引用。"""
        self._lines.append(f"![{alt}]({path})")
        self._lines.append("")

    # ── 高级内容方法 ──

    def add_loop_result(self, result: LoopResult, domain_name: str) -> None:
        """添加单个闭环域结果。

        包含状态、耗时、重试次数、断言详情表格、截图（如有）、AI 分析（如有）。

        Args:
            result: 闭环域执行结果。
            domain_name: 域名称（dev / test / verify / client_ui）。
        """
        emoji = _status_emoji(result.status)
        label = _status_label(result.status)
        self.add_header(f"{emoji} {domain_name} 闭环 - {label}", level=2)

        # 基本信息
        self.add_table(
            ["项目", "值"],
            [
                ["状态", f"{emoji} {label}"],
                ["耗时", _fmt_duration(result.duration_sec)],
                ["重试次数", str(result.retry_count)],
                ["错误信息", result.error or "无"],
            ],
        )

        # 断言详情
        if result.assertions:
            self.add_header("断言详情", level=3)
            rows: list[list[str]] = []
            for a in result.assertions:
                passed_str = "✅ 通过" if a.passed else "❌ 失败"
                severity_str = a.severity.value if hasattr(a.severity, "value") else str(a.severity)
                actual_str = str(a.actual_value).replace("\n", " ")
                if len(actual_str) > 80:
                    actual_str = actual_str[:77] + "..."
                rows.append([
                    a.name,
                    severity_str,
                    passed_str,
                    actual_str,
                    a.message or "",
                ])
            self.add_table(
                ["断言名称", "级别", "结果", "实际值", "说明"],
                rows,
            )

        # 截图（如有）
        artifacts = result.artifacts or {}
        screenshot_path = artifacts.get("screenshot_path", "")
        if screenshot_path and isinstance(screenshot_path, str) and Path(screenshot_path).exists():
            self.add_header("截图", level=3)
            self.add_image(screenshot_path, alt=f"{domain_name} 截图")

        # AI 分析（如有）
        if result.ai_analysis:
            self.add_header("AI 分析", level=3)
            self.add_text(result.ai_analysis)

        # 失败日志（如有）
        if result.error and result.status == LoopStatus.FAILED:
            # 尝试从 artifacts 中提取 stderr / 失败日志
            stderr = artifacts.get("stderr", "")
            failed_logs = artifacts.get("failed_job_logs", {})
            log_sections: list[str] = []
            if stderr:
                log_sections.append(f"**stderr:**\n```\n{stderr[:2000]}\n```")
            if isinstance(failed_logs, dict):
                for job_name, log_text in failed_logs.items():
                    if log_text:
                        log_sections.append(f"**{job_name} 日志:**\n```\n{str(log_text)[:2000]}\n```")
            if log_sections:
                self.add_header("失败日志", level=3)
                for section in log_sections:
                    self._lines.append(section)
                    self._lines.append("")

    def add_execution_summary(self, execution: LoopExecution) -> None:
        """添加执行总览。

        包含任务、时间、总状态、各域状态表、总成本。

        Args:
            execution: 完整的闭环执行记录。
        """
        self.add_header("执行总览", level=2)

        emoji = _status_emoji(execution.status)
        label = _status_label(execution.status)

        started = execution.started_at.strftime("%Y-%m-%d %H:%M:%S") if execution.started_at else "未知"
        finished = execution.finished_at.strftime("%Y-%m-%d %H:%M:%S") if execution.finished_at else "未完成"

        self.add_table(
            ["项目", "值"],
            [
                ["Loop ID", execution.loop_id],
                ["任务", execution.task or "(未指定)"],
                ["开始时间", started],
                ["结束时间", finished],
                ["总状态", f"{emoji} {label}"],
                ["总成本 (元)", f"{execution.total_cost_cny:.4f}"],
                ["报告路径", execution.report_path or "(未生成)"],
            ],
        )

        # 各域状态表
        if execution.domains:
            self.add_header("各域执行状态", level=3)
            rows: list[list[str]] = []
            for domain in execution.domains:
                name = domain.get("name", "")
                status_val = domain.get("status", "")
                # 兼容 enum 和 str
                if hasattr(status_val, "value"):
                    status_val = status_val.value
                duration = domain.get("duration_sec", 0.0)
                retry = domain.get("retry_count", 0)
                error = domain.get("error", "")
                if error and len(str(error)) > 60:
                    error = str(error)[:57] + "..."
                # 状态 -> emoji + 中文标签
                try:
                    status_enum = LoopStatus(status_val)
                    status_display = f"{_status_emoji(status_enum)} {_status_label(status_enum)}"
                except (ValueError, TypeError):
                    status_display = str(status_val)
                rows.append([
                    name,
                    status_display,
                    _fmt_duration(float(duration)),
                    str(retry),
                    str(error or ""),
                ])
            self.add_table(
                ["域", "状态", "耗时", "重试", "错误"],
                rows,
            )

    # ── 输出方法 ──

    def render(self) -> str:
        """生成完整 Markdown 文本。

        Returns:
            str: Markdown 格式的报告文本。
        """
        # 在最前面插入主标题和生成时间
        header_lines: list[str] = [
            f"# {self.title}",
            "",
            f"> 生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            "",
        ]
        return "\n".join(header_lines + self._lines).rstrip() + "\n"

    def save(self, path: str) -> None:
        """写入文件。

        Args:
            path: 输出文件路径。
        """
        content = self.render()
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(content, encoding="utf-8")
