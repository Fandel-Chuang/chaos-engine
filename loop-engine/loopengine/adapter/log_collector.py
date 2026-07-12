"""日志采集器 - 采集 ChaosEngine 各服务的日志文件。

日志文件位于 chaos-engine/logs/ 目录下，格式为 {service}.log。
客户端日志通常通过 stdout 重定向采集。
"""

from __future__ import annotations

import re
from pathlib import Path


class LogCollector:
    """ChaosEngine 日志采集器。

    采集 logs/ 目录下的服务日志，并提供正则搜索和 tail 功能。
    """

    # 服务日志文件名模式：logs/{service}.log
    SERVICE_LOG_PATTERN = "*.log"

    def __init__(self, chaos_engine_dir: str) -> None:
        """初始化日志采集器。

        Args:
            chaos_engine_dir: ChaosEngine 项目根目录。
        """
        self.project_dir = Path(chaos_engine_dir)
        self.logs_dir = self.project_dir / "logs"

    def collect_service_logs(self) -> dict[str, str]:
        """采集 logs/ 目录下所有服务日志。

        Returns:
            dict[str, str]: {服务名: 日志内容}。文件不存在时值为空字符串。
        """
        result: dict[str, str] = {}
        if not self.logs_dir.exists():
            return result

        for log_file in self.logs_dir.glob(self.SERVICE_LOG_PATTERN):
            try:
                result[log_file.stem] = log_file.read_text(
                    encoding="utf-8", errors="replace"
                )
            except OSError:
                result[log_file.stem] = ""

        return result

    def collect_client_log(self) -> str:
        """采集客户端 stdout 日志。

        客户端日志通常保存在 logs/client.log，也可能由外部进程捕获。
        依次尝试 client.log / client_stdout.log。

        Returns:
            str: 客户端日志内容，文件不存在时返回空字符串。
        """
        if not self.logs_dir.exists():
            return ""

        # 尝试常见的客户端日志文件名
        for name in ("client.log", "client_stdout.log", "chaos_client.log"):
            log_path = self.logs_dir / name
            if log_path.exists():
                try:
                    return log_path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    return ""

        return ""

    def grep_logs(
        self, log_text: str, patterns: list[str]
    ) -> list[dict]:
        """在日志文本中按正则搜索匹配行。

        Args:
            log_text: 日志文本。
            patterns: 正则表达式列表，任一匹配即记录。

        Returns:
            list[dict]: 匹配结果列表，每项含:
                - line_no: 行号（1-indexed）
                - line: 行内容
                - pattern: 匹配到的正则
        """
        compiled = [re.compile(p) for p in patterns]
        matches: list[dict] = []

        for line_no, line in enumerate(log_text.splitlines(), start=1):
            for regex in compiled:
                if regex.search(line):
                    matches.append(
                        {
                            "line_no": line_no,
                            "line": line,
                            "pattern": regex.pattern,
                        }
                    )
                    break  # 一行只记录一次

        return matches

    def tail_log(self, file_path: str, lines: int = 50) -> str:
        """读取日志文件的最后 N 行。

        Args:
            file_path: 日志文件路径（相对路径基于项目根目录）。
            lines: 要读取的行数。

        Returns:
            str: 最后 N 行内容，文件不存在时返回空字符串。
        """
        path = Path(file_path)
        if not path.is_absolute():
            path = self.project_dir / file_path

        if not path.exists():
            return ""

        try:
            all_lines = path.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()
        except OSError:
            return ""

        if len(all_lines) <= lines:
            return "\n".join(all_lines)

        return "\n".join(all_lines[-lines:])
