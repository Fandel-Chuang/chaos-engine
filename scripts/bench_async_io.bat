@echo off
chcp 65001 > nul
rem scripts\bench_async_io.bat — 异步 I/O 性能对比基准测试 (Windows 说明)
rem
rem 此脚本依赖 Linux 特有功能，无法在 Windows 上完整运行:
rem   - io_uring (Linux 5.1+, liburing)
rem   - fuser 命令
rem   - /proc/cpuinfo
rem
rem Windows 替代方案: 在 WSL2 或 Linux 虚拟机中运行原始 .sh 脚本

echo ============================================================
echo   ChaosEngine io_uring vs POSIX 异步 I/O 性能对比基准测试
echo ============================================================
echo.
echo   [WARN] 此测试依赖 Linux 特有功能 (io_uring, liburing)
echo          无法在 Windows 上原生运行。
echo.
echo   推荐运行方式:
echo     1. WSL2:  wsl bash scripts/bench_async_io.sh
echo     2. Linux: bash scripts/bench_async_io.sh
echo     3. Docker: docker run --rm -v .:/app ubuntu bash /app/scripts/bench_async_io.sh
echo.
echo   快速模式 (WSL2):
echo     wsl bash scripts/bench_async_io.sh --quick
echo.

exit /b 1
