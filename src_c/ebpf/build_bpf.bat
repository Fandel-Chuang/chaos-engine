@echo off
rem src_c\ebpf\build_bpf.bat — BPF 编译 (Windows 说明)
rem
rem BPF 编译依赖 Linux 内核头文件和 clang -target bpf，无法在 Windows 原生运行。

echo === ChaosEngine BPF Compiler ===
echo.
echo [WARN] BPF 编译依赖 Linux 内核头文件和 clang -target bpf
echo        无法在 Windows 上原生运行。
echo.
echo 推荐运行方式:
echo   1. WSL2:  wsl bash src_c/ebpf/build_bpf.sh
echo   2. Linux: bash src_c/ebpf/build_bpf.sh
echo   3. Docker:
echo        docker run --rm -v .:/app ubuntu:22.04 bash -c ^
echo          "apt install -y clang libbpf-dev linux-headers-generic ^&^& bash /app/src_c/ebpf/build_bpf.sh"
echo.
echo 依赖:
echo   apt install clang libbpf-dev linux-headers-$(uname -r)
echo.

exit /b 1
