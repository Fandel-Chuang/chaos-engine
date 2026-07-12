"""ChaosEngine 脚本执行器 - spec 6.1.2。

封装 scripts/ 目录下脚本的同步/异步执行，统一返回 ScriptResult。
"""

from __future__ import annotations

import os
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass
class ScriptResult:
    """脚本执行结果。"""

    exit_code: int
    stdout: str
    stderr: str
    duration_sec: float
    success: bool


class ScriptExec:
    """ChaosEngine 脚本执行器。

    通过 bash 执行 scripts/ 目录下的脚本，捕获 stdout/stderr/exit code。
    """

    def __init__(self, chaos_engine_dir: str) -> None:
        """初始化脚本执行器。

        Args:
            chaos_engine_dir: ChaosEngine 项目根目录。
        """
        self.project_dir = Path(chaos_engine_dir)
        self.scripts_dir = self.project_dir / "scripts"

    def run(
        self,
        script_name: str,
        args: list[str] | None = None,
        timeout: int = 300,
        env_extra: dict[str, str] | None = None,
    ) -> ScriptResult:
        """同步执行 bash 脚本，等待完成。

        Args:
            script_name: 脚本文件名（如 "build_and_test.sh"）。
            args: 传给脚本的参数列表。
            timeout: 超时时间（秒）。
            env_extra: 额外环境变量。

        Returns:
            ScriptResult: 执行结果。
        """
        script_path = self.scripts_dir / script_name
        if not script_path.exists():
            return ScriptResult(
                exit_code=-1,
                stdout="",
                stderr=f"脚本不存在: {script_path}",
                duration_sec=0.0,
                success=False,
            )

        cmd = ["bash", str(script_path)] + (args or [])

        # 合并环境变量，确保 DISPLAY=:0（X11 客户端脚本需要）
        env = {**os.environ, **(env_extra or {})}
        env.setdefault("DISPLAY", ":0")

        start = time.time()
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
                cwd=str(self.project_dir),
            )
            duration = time.time() - start
            return ScriptResult(
                exit_code=result.returncode,
                stdout=result.stdout,
                stderr=result.stderr,
                duration_sec=duration,
                success=result.returncode == 0,
            )
        except subprocess.TimeoutExpired as e:
            duration = time.time() - start
            # TimeoutExpired.stdout/stderr 可能是 bytes 或 str，统一转 str
            out = e.stdout
            if isinstance(out, bytes):
                out = out.decode("utf-8", errors="replace")
            elif not isinstance(out, str):
                out = ""
            err = e.stderr
            if isinstance(err, bytes):
                err = err.decode("utf-8", errors="replace")
            elif not isinstance(err, str):
                err = ""
            return ScriptResult(
                exit_code=-1,
                stdout=out,
                stderr=f"超时({timeout}s): {err}",
                duration_sec=duration,
                success=False,
            )

    def run_async(
        self,
        script_name: str,
        args: list[str] | None = None,
        env_extra: dict[str, str] | None = None,
    ) -> subprocess.Popen:
        """异步启动脚本（不等待完成）。

        用于启动集群/客户端等需要后台运行的进程。

        Args:
            script_name: 脚本文件名。
            args: 传给脚本的参数列表。
            env_extra: 额外环境变量。

        Returns:
            subprocess.Popen: 进程对象，调用方可读取 stdout/stderr 或等待。
        """
        script_path = self.scripts_dir / script_name
        cmd = ["bash", str(script_path)] + (args or [])

        env = {**os.environ, **(env_extra or {})}
        env.setdefault("DISPLAY", ":0")

        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            cwd=str(self.project_dir),
        )
