@echo off
chcp 65001 > nul
rem ci\build.bat — ChaosEngine 本地 CI 构建脚本 (Windows)
rem
rem 一键构建流程：
rem   1. 运行 CI 规则检查（跨目录引用、C++ 语法侵入）
rem   2. CMake 配置
rem   3. 编译
rem   4. 运行测试（如果启用）

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
set "BUILD_DIR=%PROJECT_ROOT%\build"

rem ============================================================
rem Step 1: CI 规则检查
rem ============================================================
echo.
echo ============================================================
echo   Step 1: CI Rule Checks
echo ============================================================
echo.

set "RULES_DIR=%PROJECT_ROOT%\ci\rules"
set "ALL_RULES_PASSED=true"

echo   [INFO] Running check_cross_ref.py...
python "%RULES_DIR%\check_cross_ref.py"
if %errorlevel% neq 0 (
    echo   [FAIL] check_cross_ref.py
    set "ALL_RULES_PASSED=false"
) else (
    echo   [PASS] check_cross_ref.py
)

echo   [INFO] Running check_cpp_in_c.py...
python "%RULES_DIR%\check_cpp_in_c.py"
if %errorlevel% neq 0 (
    echo   [FAIL] check_cpp_in_c.py
    set "ALL_RULES_PASSED=false"
) else (
    echo   [PASS] check_cpp_in_c.py
)

if "%ALL_RULES_PASSED%"=="false" (
    echo.
    echo   [FAIL] CI rules check failed. Aborting build.
    exit /b 1
)

echo.
echo   [PASS] All CI rules passed!

rem ============================================================
rem Step 2: CMake 配置
rem ============================================================
echo.
echo ============================================================
echo   Step 2: CMake Configuration
echo ============================================================
echo.

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

echo   [INFO] Running cmake .. -DCHAOS_BUILD_EDITOR=ON -DCHAOS_BUILD_TESTS=ON
cmake .. -DCHAOS_BUILD_EDITOR=ON -DCHAOS_BUILD_TESTS=ON
if %errorlevel% neq 0 (
    echo   [FAIL] CMake configuration
    exit /b 1
)
echo   [PASS] CMake configuration

rem ============================================================
rem Step 3: 编译
rem ============================================================
echo.
echo ============================================================
echo   Step 3: Build
echo ============================================================
echo.

echo   [INFO] Running cmake --build .
cmake --build . --config Debug
if %errorlevel% neq 0 (
    echo   [FAIL] Build failed
    exit /b 1
)
echo   [PASS] Build succeeded

echo.
echo   [INFO] Build artifacts:
if exist "%BUILD_DIR%\bin\" (dir /b "%BUILD_DIR%\bin\") else (echo   [INFO] No binaries in bin/)
if exist "%BUILD_DIR%\lib\" (dir /b "%BUILD_DIR%\lib\") else (echo   [INFO] No libraries in lib/)

rem ============================================================
rem Step 4: 运行测试
rem ============================================================
echo.
echo ============================================================
echo   Step 4: Run Tests
echo ============================================================
echo.

if exist "%BUILD_DIR%\tests\CTestTestfile.cmake" (
    echo   [INFO] Running ctest...
    cd /d "%BUILD_DIR%"
    ctest --output-on-failure
    if %errorlevel% neq 0 (
        echo   [FAIL] Some tests failed
        exit /b 1
    )
    echo   [PASS] All tests passed
) else (
    echo   [INFO] No tests configured ^(CHAOS_BUILD_TESTS may be OFF^)
)

rem ============================================================
rem 完成
rem ============================================================
echo.
echo ============================================================
echo   CI Build Complete
echo ============================================================
echo   All steps completed successfully!
echo.

exit /b 0
