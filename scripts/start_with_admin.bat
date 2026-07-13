@echo off
chcp 65001 > nul
rem scripts\start_with_admin.bat — ChaosEngine + Admin Dashboard 一键启动 (Windows)
rem 自动清理旧进程，构建，启动 server + admin

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."

rem ── 0. 清理旧进程 ──
echo.
echo ^>^>^> 0/4 清理旧进程...

set "KILLED=0"

tasklist /fi "imagename eq chaos_server.exe" 2>nul | findstr /i "chaos_server.exe" >nul
if %errorlevel% equ 0 (
    echo [WARN] 终止旧的 chaos_server...
    taskkill /f /im chaos_server.exe >nul 2>&1
    set "KILLED=1"
)

rem 清理占用 9090 端口的进程
for /f "tokens=5" %%p in ('netstat -ano 2^>nul ^| findstr ":9090 " ^| findstr "LISTENING"') do (
    echo [WARN] 终止占用 9090 端口的进程 ^(PID: %%p^)...
    taskkill /f /pid %%p >nul 2>&1
    set "KILLED=1"
)

rem 清理占用 7777 端口的进程
for /f "tokens=5" %%p in ('netstat -ano 2^>nul ^| findstr ":7777 " ^| findstr "LISTENING"') do (
    echo [WARN] 终止占用 7777 端口的进程 ^(PID: %%p^)...
    taskkill /f /pid %%p >nul 2>&1
    set "KILLED=1"
)

if "%KILLED%"=="1" (
    timeout /t 2 /nobreak >nul
    echo [OK] 旧进程已清理
) else (
    echo [OK] 无旧进程需要清理
)

rem ── 1. 构建 ──
echo.
echo ^>^>^> 1/4 构建 chaos_server...

cd /d "%PROJECT_DIR%\build"
cmake --build . --target chaos_server --config Debug
if %errorlevel% neq 0 (
    echo [ERROR] chaos_server 构建失败
    exit /b 1
)
echo [OK] chaos_server 构建完成

rem ── 2. 启动 chaos_server ──
echo.
echo ^>^>^> 2/4 启动 chaos_server ^(带 admin IPC^)...

start /b "" "%PROJECT_DIR%\build\bin\chaos_server.exe" --admin

rem 等待启动（Windows 无 Unix Socket，轮询端口）
set "SERVER_READY=0"
for /l %%i in (1,1,30) do (
    netstat -ano 2>nul | findstr ":7777 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 (
        set "SERVER_READY=1"
        goto server_ready
    )
    timeout /t 1 /nobreak >nul
)

:server_ready
if "%SERVER_READY%"=="0" (
    echo [ERROR] chaos_server 启动超时
    exit /b 1
)
echo [OK] chaos_server 已启动

rem ── 3. 启动 Admin (Lapis) ──
echo.
echo ^>^>^> 3/4 启动 chaos_admin ^(Lapis Web 后台^)...

where lapis >nul 2>&1
if %errorlevel% neq 0 (
    echo [WARN] lapis 未安装，跳过 Admin 启动
    echo        请安装 OpenResty + LuaRocks 并执行: luarocks install lapis
    goto summary
)

cd /d "%PROJECT_DIR%\src_lua\admin"
start /b "" cmd /c "lapis server"

rem 等待 HTTP 就绪
set "ADMIN_READY=0"
for /l %%i in (1,1,15) do (
    curl -s http://localhost:9090/api/health >nul 2>&1
    if !errorlevel! equ 0 (
        set "ADMIN_READY=1"
        goto admin_ready
    )
    timeout /t 1 /nobreak >nul
)

:admin_ready
if "%ADMIN_READY%"=="1" (
    echo [OK] HTTP 服务就绪: http://localhost:9090
) else (
    echo [WARN] Admin HTTP 服务未就绪，请检查日志
)

:summary
rem ── 4. 打印信息 ──
echo.
echo ^>^>^> 4/4 启动完成
echo.
echo ============================================
echo   Dashboard:     http://localhost:9090
echo   API Health:    http://localhost:9090/api/health
echo   API Stats:     http://localhost:9090/api/stats
echo ============================================
echo.
echo 停止所有服务: scripts\stop_cluster.bat

exit /b 0
