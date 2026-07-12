"""标题栏解析器 - spec 4.2.1 / 4.3.1。

采集 ChaosEngine Vulkan 客户端的三路信号：
1. 窗口标题栏（xprop -name WM_NAME），解析 pos / visible / disconnected / keys
2. 客户端 stdout 日志，提取 Vulkan 初始化 / 出生点 / 实体同步 / 网络错误 / 本地坐标
3. 进程与窗口状态（kill -0 + xwininfo）

标题栏格式（来自 ce_client_main.c 源码）：
  已连接: ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | visible=%d | keys=%s
  未连接: ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | disconnected | keys=%s
"""

from __future__ import annotations

import re
import subprocess
import time
from dataclasses import dataclass, field
from typing import Optional


# ── 数据快照 ──


@dataclass
class TitleBarSnapshot:
    """标题栏快照 - 单次 xprop 采集结果。"""

    timestamp: float
    raw_text: str
    pos_x: Optional[float] = None
    pos_y: Optional[float] = None
    pos_z: Optional[float] = None
    visible: Optional[int] = None
    connected: bool = False
    keys: str = "----"


@dataclass
class LogSnapshot:
    """日志快照 - 从 stdout 日志行列表提取的信号集合。"""

    timestamp: float
    lines: list[str]
    vulkan_initialized: bool = False
    spawn_pos: Optional[tuple[float, float, float]] = None
    entity_update_received: bool = False
    network_error: bool = False
    local_pos_entries: list[dict] = field(default_factory=list)


@dataclass
class ProcessSnapshot:
    """进程/窗口快照 - kill -0 + xwininfo 采集结果。"""

    timestamp: float
    process_alive: bool = False
    window_exists: bool = False
    window_id: Optional[str] = None
    window_width: Optional[int] = None
    window_height: Optional[int] = None


# ── 数据采集器 ──


class ClientDataCollector:
    """客户端数据采集器。

    通过 xprop / kill -0 / xwininfo 采集客户端运行时信号，
    供结构化断言引擎（ClientAssertionEngine）判定。
    """

    # 标题栏正则（与 ce_client_main.c 的 snprintf 格式对应）
    TITLE_BAR_PATTERNS: dict[str, re.Pattern[str]] = {
        # pos=(1.23, 0.00, -2.50)
        "pos": re.compile(r"pos=\(([0-9.\-]+), ([0-9.\-]+), ([0-9.\-]+)\)"),
        # visible=3
        "visible": re.compile(r"visible=(\d+)"),
        # disconnected（未连接标记）
        "disconnected": re.compile(r"disconnected"),
        # keys=WS-- （W/S/A/D 四键状态，- 表示未按）
        "keys": re.compile(r"keys=([WSAD\-]{4})"),
    }

    # 日志正则（与客户端 stdout 输出格式对应）
    LOG_PATTERNS: dict[str, re.Pattern[str]] = {
        # Vulkan device created successfully
        "vulkan_init": re.compile(r"Vulkan device created"),
        # [Client] Spawn pos: (x, y, z) within r=2.50
        "spawn_pos": re.compile(
            r"Spawn pos: \(([0-9.\-]+), ([0-9.\-]+), ([0-9.\-]+)\) within r=([0-9.]+)"
        ),
        # Received ENTITY_UPDATE
        "entity_update": re.compile(r"Received ENTITY_UPDATE"),
        # [Client] Network error, continuing without network
        "network_error": re.compile(r"Network error"),
        # [Client] Local pos: (x, y, z), visible=N
        "local_pos": re.compile(
            r"Local pos: \(([0-9.\-]+), ([0-9.\-]+), ([0-9.\-]+)\), visible=(\d+)"
        ),
    }

    def __init__(self, process_pid: int, window_name: str = "ChaosEngine Vulkan") -> None:
        """初始化数据采集器。

        Args:
            process_pid: 客户端进程 PID。
            window_name: 窗口标题名（用于 xprop / xwininfo 查找）。
        """
        self.process_pid = process_pid
        self.window_name = window_name

    # ── 标题栏采集 ──

    def collect_title_bar(self) -> TitleBarSnapshot:
        """采集窗口标题栏（通过 xprop -name WM_NAME）。

        执行 ``xprop -name <window_name> WM_NAME``，从输出中
        解析 ``WM_NAME(STRING) = "..."``，再用正则提取各字段。

        Returns:
            TitleBarSnapshot: 标题栏快照。采集失败时返回空快照。
        """
        try:
            result = subprocess.run(
                ["xprop", "-name", self.window_name, "WM_NAME"],
                capture_output=True,
                text=True,
                timeout=2,
            )
            raw = result.stdout

            # 解析: WM_NAME(STRING) = "ChaosEngine Vulkan | pos=(...) | ..."
            match = re.search(r'WM_NAME\([^)]*\) = "([^"]*)"', raw)
            title_text = match.group(1) if match else ""

            snapshot = TitleBarSnapshot(
                timestamp=time.time(),
                raw_text=title_text,
            )

            # 解析坐标 pos=(x, y, z)
            if pos_match := self.TITLE_BAR_PATTERNS["pos"].search(title_text):
                snapshot.pos_x = float(pos_match.group(1))
                snapshot.pos_y = float(pos_match.group(2))
                snapshot.pos_z = float(pos_match.group(3))

            # 解析连接状态：有 visible= 则已连接，有 disconnected 则未连接
            if vis_match := self.TITLE_BAR_PATTERNS["visible"].search(title_text):
                snapshot.visible = int(vis_match.group(1))
                snapshot.connected = True
            elif self.TITLE_BAR_PATTERNS["disconnected"].search(title_text):
                snapshot.connected = False

            # 解析按键状态 keys=WSAD
            if keys_match := self.TITLE_BAR_PATTERNS["keys"].search(title_text):
                snapshot.keys = keys_match.group(1)

            return snapshot

        except Exception:
            # 采集异常时返回空快照，不阻断断言流程
            return TitleBarSnapshot(
                timestamp=time.time(),
                raw_text="",
            )

    # ── 日志采集 ──

    def collect_log(self, log_lines: list[str]) -> LogSnapshot:
        """从日志行列表采集信号。

        遍历所有行，用 LOG_PATTERNS 匹配关键信号：
        Vulkan 初始化、出生点、实体同步、网络错误、本地坐标。

        Args:
            log_lines: 客户端 stdout 日志行列表。

        Returns:
            LogSnapshot: 日志快照。
        """
        snapshot = LogSnapshot(
            timestamp=time.time(),
            lines=log_lines,
        )

        for line in log_lines:
            # Vulkan 设备初始化成功
            if self.LOG_PATTERNS["vulkan_init"].search(line):
                snapshot.vulkan_initialized = True

            # 出生点坐标
            if m := self.LOG_PATTERNS["spawn_pos"].search(line):
                snapshot.spawn_pos = (
                    float(m.group(1)),
                    float(m.group(2)),
                    float(m.group(3)),
                )

            # 实体同步消息
            if self.LOG_PATTERNS["entity_update"].search(line):
                snapshot.entity_update_received = True

            # 网络错误
            if self.LOG_PATTERNS["network_error"].search(line):
                snapshot.network_error = True

            # 本地坐标条目（每 60 帧输出一次）
            if m := self.LOG_PATTERNS["local_pos"].search(line):
                snapshot.local_pos_entries.append(
                    {
                        "x": float(m.group(1)),
                        "y": float(m.group(2)),
                        "z": float(m.group(3)),
                        "visible": int(m.group(4)),
                    }
                )

        return snapshot

    # ── 进程/窗口采集 ──

    def collect_process(self) -> ProcessSnapshot:
        """采集进程/窗口状态。

        通过 ``kill -0 <pid>`` 检查进程存活，
        通过 ``xwininfo -name <window_name>`` 检查窗口存在性并解析窗口 ID 和尺寸。

        Returns:
            ProcessSnapshot: 进程/窗口快照。
        """
        snapshot = ProcessSnapshot(timestamp=time.time())

        # 进程存活检查
        try:
            subprocess.run(
                ["kill", "-0", str(self.process_pid)],
                check=True,
                capture_output=True,
            )
            snapshot.process_alive = True
        except subprocess.CalledProcessError:
            snapshot.process_alive = False

        # 窗口存在检查 + 尺寸解析
        try:
            result = subprocess.run(
                ["xwininfo", "-name", self.window_name],
                capture_output=True,
                text=True,
                timeout=2,
            )
            if result.returncode == 0:
                snapshot.window_exists = True

                # 解析窗口 ID: "Window id: 0x3a00006"
                if id_match := re.search(r"Window id: (0x[0-9a-fA-F]+)", result.stdout):
                    snapshot.window_id = id_match.group(1)

                # 解析宽度: "  Width: 1280"
                if w_match := re.search(r"Width: (\d+)", result.stdout):
                    snapshot.window_width = int(w_match.group(1))

                # 解析高度: "  Height: 720"
                if h_match := re.search(r"Height: (\d+)", result.stdout):
                    snapshot.window_height = int(h_match.group(1))
        except Exception:
            pass

        return snapshot
