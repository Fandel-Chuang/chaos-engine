"""ChaosEngine 集群进程管理器 - spec 6.4。

封装集群启动/停止/状态查询，通过调用 scripts/ 下的 shell 脚本实现。
"""

from __future__ import annotations

import json
import os
import subprocess
import time
from pathlib import Path


class ProcessManager:
    """ChaosEngine 集群进程管理器。

    通过 start_cluster.sh / stop_cluster_server.sh / status.sh 管理集群生命周期。
    """

    # 服务定义（与 status.sh / start_cluster.sh 一致）
    SERVICES = [
        {"name": "dbproxy", "port": 9003, "pid_file": ".pids/dbproxy.pid"},
        {"name": "game", "port": 7777, "pid_file": ".pids/game.pid"},
        {"name": "router", "port": 9100, "pid_file": ".pids/router.pid"},
        {"name": "gateway", "port": 9000, "pid_file": ".pids/gateway.pid"},
        {"name": "admin", "port": 9090, "pid_file": ".pids/admin.pid"},
    ]

    def __init__(self, chaos_engine_dir: str) -> None:
        """初始化进程管理器。

        Args:
            chaos_engine_dir: ChaosEngine 项目根目录。
        """
        self.project_dir = Path(chaos_engine_dir)

    def start_cluster(self, services: list[str] | None = None) -> bool:
        """启动集群。

        Args:
            services: 要启动的服务名列表（如 ["game", "gateway"]）。
                      为 None 时启动全部服务。

        Returns:
            bool: 启动脚本是否执行成功（exit_code == 0）。
        """
        script = self.project_dir / "scripts" / "start_cluster.sh"
        args: list[str] = []
        if services:
            for svc in services:
                args.append(f"--{svc}")

        try:
            result = subprocess.run(
                ["bash", str(script)] + args,
                capture_output=True,
                text=True,
                timeout=30,
                cwd=str(self.project_dir),
            )
            return result.returncode == 0
        except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
            return False

    def stop_cluster(self) -> bool:
        """停止集群。

        Returns:
            bool: 停止脚本是否执行成功。
        """
        script = self.project_dir / "scripts" / "stop_cluster_server.sh"
        try:
            result = subprocess.run(
                ["bash", str(script)],
                capture_output=True,
                text=True,
                timeout=15,
                cwd=str(self.project_dir),
            )
            return result.returncode == 0
        except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
            return False

    def get_status(self) -> dict:
        """获取集群状态（JSON 格式）。

        调用 status.sh --json，解析返回的 JSON。

        Returns:
            dict: 状态信息，格式为 {"timestamp": "...", "services": [...]}。
                  解析失败或脚本报错时返回空 dict。
        """
        script = self.project_dir / "scripts" / "status.sh"
        try:
            result = subprocess.run(
                ["bash", str(script), "--json"],
                capture_output=True,
                text=True,
                timeout=5,
                cwd=str(self.project_dir),
            )
        except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
            return {}
        if result.returncode == 0:
            try:
                return json.loads(result.stdout)
            except json.JSONDecodeError:
                return {}
        return {}

    def wait_for_services(
        self, timeout: int = 30, services: list[str] | None = None
    ) -> bool:
        """轮询 status.sh 直到所有目标服务处于 running 状态。

        Args:
            timeout: 最大等待时间（秒）。
            services: 目标服务名列表，为 None 时检查全部 SERVICES。

        Returns:
            bool: 超时前所有服务是否就绪。
        """
        target_services = services or [s["name"] for s in self.SERVICES]

        start = time.time()
        while time.time() - start < timeout:
            status = self.get_status()
            svc_list = status.get("services", [])
            if not svc_list:
                time.sleep(1)
                continue

            all_ready = True
            for svc in svc_list:
                if svc["name"] in target_services:
                    if svc["status"] == "stopped":
                        all_ready = False
                        break

            if all_ready:
                return True

            time.sleep(1)

        return False

    def is_process_alive(self, pid_file: str) -> bool:
        """检查 pid 文件对应的进程是否存活。

        Args:
            pid_file: pid 文件路径（相对或绝对）。相对路径基于项目根目录。

        Returns:
            bool: 进程是否存活。
        """
        pid_path = Path(pid_file)
        if not pid_path.is_absolute():
            pid_path = self.project_dir / pid_path

        if not pid_path.exists():
            return False

        try:
            pid = int(pid_path.read_text().strip())
        except (ValueError, OSError):
            return False

        # kill -0 只检查进程是否存在，不发送信号
        try:
            os.kill(pid, 0)
            return True
        except (ProcessLookupError, PermissionError):
            return False
