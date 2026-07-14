"""截图器（X11 环境）- spec 4.4.2。

使用 ImageMagick import 或 xwd 截取 X11 窗口截图，
用于客户端界面表现验证的视觉复核环节。
"""

from __future__ import annotations

import os
import subprocess
from datetime import datetime
from pathlib import Path


class ScreenshotCapture:
    """X11 窗口截图器。

    优先使用 ImageMagick import，备选 xwd + convert。
    支持按窗口 ID 或窗口标题截图。
    """

    def __init__(self, display: str = ":0") -> None:
        """初始化截图器。

        Args:
            display: X11 DISPLAY 环境变量值。
        """
        self.display = display
        self.screenshot_dir = Path("screenshots")
        self.screenshot_dir.mkdir(parents=True, exist_ok=True)

    def _make_output_path(self, prefix: str = "capture") -> str:
        """生成带时间戳的输出文件路径。"""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        return str(self.screenshot_dir / f"{prefix}_{timestamp}.png")

    def _get_env(self) -> dict[str, str]:
        """获取带 DISPLAY 的环境变量。"""
        env = {**os.environ}
        env["DISPLAY"] = self.display
        return env

    def capture(
        self,
        window_id: str | None = None,
        output_path: str | None = None,
    ) -> str:
        """截取指定窗口或全屏。

        Args:
            window_id: X11 窗口 ID（如 "0x3a00006"）。为 None 时截全屏。
            output_path: 输出文件路径。为 None 时自动生成。

        Returns:
            str: 截图文件路径。

        Raises:
            RuntimeError: 截图失败（工具缺失/DISPLAY 未设/命令失败）。
        """
        if output_path is None:
            output_path = self._make_output_path("window" if window_id else "screen")

        env = self._get_env()
        target = window_id if window_id else "root"

        # 方案1: ImageMagick import（最简单可靠）
        try:
            result = subprocess.run(
                ["import", "-window", target, output_path],
                capture_output=True,
                timeout=5,
                env=env,
            )
            if result.returncode == 0 and os.path.exists(output_path):
                return output_path
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass

        # 方案2: xwd -> convert (ImageMagick)
        xwd_path = output_path.replace(".png", ".xwd")
        try:
            result = subprocess.run(
                ["xwd", "-id", window_id, "-out", xwd_path]
                if window_id
                else ["xwd", "-root", "-out", xwd_path],
                capture_output=True,
                timeout=5,
                env=env,
            )
            if result.returncode == 0 and os.path.exists(xwd_path):
                subprocess.run(
                    ["convert", xwd_path, output_path],
                    capture_output=True,
                    timeout=5,
                    env=env,
                )
                if os.path.exists(xwd_path):
                    os.remove(xwd_path)
                if os.path.exists(output_path):
                    return output_path
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass

        # 方案3: Wayland 原生截图（grim / gnome-screenshot）
        for cmd in [
            ["grim", output_path],
            ["gnome-screenshot", "-f", output_path],
        ]:
            try:
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    timeout=5,
                    env=env,
                )
                if result.returncode == 0 and os.path.exists(output_path):
                    return output_path
            except (subprocess.TimeoutExpired, FileNotFoundError):
                pass

        # 全部失败，给出清晰错误信息
        missing = []
        if not _command_exists("import"):
            missing.append("import (ImageMagick)")
        if not _command_exists("xwd"):
            missing.append("xwd")
        if not _command_exists("convert"):
            missing.append("convert (ImageMagick)")

        if missing:
            raise RuntimeError(
                f"截图失败：缺少工具 {', '.join(missing)}。"
                f"请安装 imagemagick 和 x11-apps 包。"
            )

        if not env.get("DISPLAY"):
            raise RuntimeError(
                "截图失败：DISPLAY 环境变量未设置。"
                f"当前 display={self.display}，请确认 X11 会话可用。"
            )

        raise RuntimeError(
            f"截图失败：所有截图方案均失败 (target={target}, display={self.display})。"
        )

    def capture_window_by_title(
        self, title_pattern: str, output_path: str | None = None
    ) -> str:
        """按窗口标题截图。

        使用 xdotool search --name 查找窗口 ID，再截图。

        Args:
            title_pattern: 窗口标题匹配模式（xdotool 支持的正则）。
            output_path: 输出文件路径。

        Returns:
            str: 截图文件路径。

        Raises:
            RuntimeError: 找不到窗口或截图失败。
        """
        window_id = self.find_window(title_pattern)
        if not window_id:
            # Wayland 降级：找不到窗口 ID，用全屏截图
            if output_path is None:
                output_path = self._make_output_path("client")
            return self.capture(window_id=None, output_path=output_path)

        if output_path is None:
            output_path = self._make_output_path("client")

        return self.capture(window_id=window_id, output_path=output_path)

    def find_window(self, title_pattern: str) -> str | None:
        """查找窗口 ID。

        优先用 xdotool search --name，备选 xprop。

        Args:
            title_pattern: 窗口标题匹配模式。

        Returns:
            str | None: 窗口 ID（如 "0x3a00006"），未找到返回 None。
        """
        env = self._get_env()

        # 方案1: xdotool search --name
        try:
            result = subprocess.run(
                ["xdotool", "search", "--name", title_pattern],
                capture_output=True,
                text=True,
                timeout=3,
                env=env,
            )
            if result.returncode == 0 and result.stdout.strip():
                # 取第一个匹配的窗口 ID
                return result.stdout.strip().splitlines()[0]
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass

        # 方案2: xprop -name（仅精确匹配）
        try:
            result = subprocess.run(
                ["xprop", "-name", title_pattern, "_NET_WM_WINDOW_ID"],
                capture_output=True,
                text=True,
                timeout=3,
                env=env,
            )
            if result.returncode == 0:
                # 解析: _NET_WM_WINDOW_ID: window id # 0x3a00006
                for line in result.stdout.splitlines():
                    if "0x" in line:
                        parts = line.split("0x")
                        if len(parts) >= 2:
                            wid = "0x" + parts[-1].strip().split()[0]
                            return wid
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass

        # 方案3: Wayland 降级 -- 窗口查找不可用，返回 None 让调用方降级
        # Wayland 下 xdotool/xprop 无法枚举窗口，截图改用全屏模式
        return None


def _command_exists(cmd: str) -> bool:
    """检查命令是否在 PATH 中可用。"""
    try:
        result = subprocess.run(
            ["which", cmd], capture_output=True, timeout=2
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False
