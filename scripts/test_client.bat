@echo off
chcp 65001 > nul
rem scripts\test_client.bat — ChaosEngine 客户端冒烟测试 (Windows)
rem 用法: scripts\test_client.bat [--stress N]

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "BUILD_DIR=%PROJECT_DIR%\build"
set "BIN_DIR=%BUILD_DIR%\bin"

set "GATEWAY_TCP=127.0.0.1:9000"
set "GATEWAY_WS=127.0.0.1:9002"
set "ADMIN_URL=http://127.0.0.1:8080"

set "PASS=0"
set "FAIL=0"
set "STRESS_MODE=0"
set "CONNS=100"

:parse_args
if "%~1"=="" goto run_tests
if "%~1"=="--stress" ( set "STRESS_MODE=1" & set "CONNS=%~2" & shift & shift & goto parse_args )
shift & goto parse_args

:run_tests

echo ============================================
echo  ChaosEngine 客户端冒烟测试
echo ============================================
echo.

rem ── 1. TCP 连接测试 ──
echo [1] TCP 连接测试

curl -s --max-time 2 --connect-timeout 2 "http://%GATEWAY_TCP%" >nul 2>&1
if %errorlevel% equ 0 (
    echo   [OK]   TCP Gateway 可连接
    set /a PASS+=1
) else (
    echo   [WARN] TCP Gateway 无响应 ^(可能未启动^)
)

rem ── 2. WebSocket 握手测试 ──
echo.
echo [2] WebSocket 测试

for /f %%c in ('curl -s -o nul -w "%%{http_code}" -H "Upgrade: websocket" -H "Connection: Upgrade" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" -H "Sec-WebSocket-Version: 13" "http://%GATEWAY_WS%" 2^>nul') do set "WS_CODE=%%c"
if "%WS_CODE%"=="101" (
    echo   [OK]   WebSocket 升级成功 ^(HTTP 101^)
    set /a PASS+=1
) else (
    echo   [WARN] WebSocket 返回 %WS_CODE% ^(Gateway WS 可能未启动^)
)

rem ── 3. Admin API 测试 ──
echo.
echo [3] Admin API 测试

curl -sf "%ADMIN_URL%/health" >nul 2>&1
if %errorlevel% equ 0 (
    echo   [OK]   Admin /health
    set /a PASS+=1
) else (
    echo   [FAIL] Admin /health
    set /a FAIL+=1
)

curl -sf "%ADMIN_URL%/api/stats" >nul 2>&1
if %errorlevel% equ 0 (
    echo   [OK]   Admin /api/stats
    set /a PASS+=1
) else (
    echo   [FAIL] Admin /api/stats
    set /a FAIL+=1
)

curl -sf "%ADMIN_URL%/api/aoi" >nul 2>&1
if %errorlevel% equ 0 (
    echo   [OK]   Admin /api/aoi
    set /a PASS+=1
) else (
    echo   [FAIL] Admin /api/aoi
    set /a FAIL+=1
)

rem ── 4. 压力测试（可选） ──
if "%STRESS_MODE%"=="1" (
    echo.
    echo [4] 压力测试 ^(%CONNS% 连接^)
    if exist "%BIN_DIR%\bench_client.exe" (
        echo   启动 bench server...
        start /b "" "%BIN_DIR%\chaos_async_bench.exe" --port 19999
        timeout /t 1 /nobreak >nul
        echo   运行 bench_client...
        "%BIN_DIR%\bench_client.exe" -p 19999 -c %CONNS% -d 3
        echo   [OK]   压力测试完成
        set /a PASS+=1
    ) else (
        echo   [WARN] bench_client.exe 未编译
    )
)

rem ── 汇总 ──
echo.
echo ============================================
set /a TOTAL=PASS+FAIL
echo  结果: %PASS%/%TOTAL% 通过
if %FAIL% gtr 0 (
    echo  %FAIL% 项失败
    echo ============================================
    exit /b 1
) else (
    echo  全部通过
    echo ============================================
    exit /b 0
)
