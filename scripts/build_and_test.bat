@echo off
chcp 65001 > nul
rem scripts\build_and_test.bat — ChaosEngine 一键构建 + 测试 (Windows)
rem 用法: scripts\build_and_test.bat [--release] [--no-test] [--no-bpf]

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "BUILD_DIR=%PROJECT_DIR%\build"
set "BUILD_TYPE=Debug"
set "RUN_TESTS=1"
set "BUILD_BPF=1"

rem 解析参数
:parse_args
if "%~1"=="" goto done_args
if "%~1"=="--release" (
    set "BUILD_TYPE=Release"
    shift & goto parse_args
)
if "%~1"=="--no-test" (
    set "RUN_TESTS=0"
    shift & goto parse_args
)
if "%~1"=="--no-bpf" (
    set "BUILD_BPF=0"
    shift & goto parse_args
)
echo 未知参数: %~1
exit /b 1

:done_args

rem 获取 CPU 核心数
set "JOBS=%NUMBER_OF_PROCESSORS%"
if "%JOBS%"=="" set "JOBS=4"

if "%RUN_TESTS%"=="1" (set "TESTS_STR=是") else (set "TESTS_STR=否")
if "%BUILD_BPF%"=="1" (set "BPF_STR=是") else (set "BPF_STR=否")

echo ============================================
echo  ChaosEngine 构建 ^& 测试
echo  构建类型: %BUILD_TYPE%
echo  并行数:   %JOBS%
echo  测试:     %TESTS_STR%
echo  BPF:      %BPF_STR%
echo ============================================

rem ── Step 1: CMake 配置 ──
echo.
echo [1/4] CMake 配置...
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

set "CMAKE_ARGS=-DCMAKE_BUILD_TYPE=%BUILD_TYPE%"
if "%BUILD_BPF%"=="0" set "CMAKE_ARGS=%CMAKE_ARGS% -DCHAOS_HAS_EBPF=OFF"

cmake "%PROJECT_DIR%" %CMAKE_ARGS%
if %errorlevel% neq 0 (
    echo CMake 配置失败
    exit /b 1
)

rem ── Step 2: 编译 ──
echo.
echo [2/4] 编译...

cmake --build . --config %BUILD_TYPE% -- /m:%JOBS%
if %errorlevel% neq 0 (
    echo.
    echo 编译失败
    exit /b 1
)
echo.
echo 编译成功

rem ── Step 3: 运行测试 ──
if "%RUN_TESTS%"=="1" (
    echo.
    echo [3/4] 运行测试...
    ctest --output-on-failure -j%JOBS% -C %BUILD_TYPE%
    if %errorlevel% neq 0 (
        echo.
        echo 测试失败
        exit /b 1
    )
    echo.
    echo 全部测试通过
) else (
    echo.
    echo [3/4] 跳过测试
)

rem ── Step 4: 冒烟测试 ──
if "%RUN_TESTS%"=="1" (
    echo.
    echo [4/4] 冒烟测试...

    set "SMOKE_PASS=0"
    set "SMOKE_FAIL=0"

    rem 4.1 验证二进制文件存在
    for %%b in (chaos_server chaos_router chaos_client chaos_headless) do (
        if exist "%BUILD_DIR%\bin\%%b.exe" (
            echo   [OK] %%b.exe 存在
            set /a SMOKE_PASS+=1
        ) else (
            echo   [WARN] %%b.exe 不存在（可能未构建此目标）
        )
    )

    rem 4.2 验证关键测试通过
    for %%t in (math memory ecs network aoi cell net_base) do (
        ctest -R "^%%t$" --output-on-failure -C %BUILD_TYPE% 2>nul | findstr /i "Passed" >nul 2>&1
        if !errorlevel! equ 0 (
            echo   [OK] test_%%t 通过
            set /a SMOKE_PASS+=1
        ) else (
            echo   [FAIL] test_%%t 失败
            set /a SMOKE_FAIL+=1
        )
    )

    echo.
    echo   冒烟测试: %SMOKE_PASS% 通过, %SMOKE_FAIL% 失败
    if %SMOKE_FAIL% gtr 0 exit /b 1
)

rem ── 汇总 ──
echo.
echo ============================================
echo  ChaosEngine 构建 ^& 测试全部通过
echo ============================================

exit /b 0
