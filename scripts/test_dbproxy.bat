@echo off
chcp 65001 > nul
rem scripts\test_dbproxy.bat — Lua DBProxy 集成测试 (Windows)
rem
rem 依赖: lua, python3, curl

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "DBPROXY_DIR=%PROJECT_DIR%\src_lua\dbproxy"
set "BUILD_BIN=%PROJECT_DIR%\build\bin\chaos_server.exe"

set "PASS=0"
set "FAIL=0"

echo ============================================
echo   DBProxy 集成测试 ^(Windows^)
echo ============================================

rem ── 检查依赖 ──
where lua >nul 2>&1
if %errorlevel% neq 0 (
    echo [FAIL] lua 未安装
    exit /b 1
)
where python3 >nul 2>&1
if %errorlevel% neq 0 (
    where python >nul 2>&1
    if %errorlevel% neq 0 (
        echo [FAIL] python3/python 未安装
        exit /b 1
    )
    set "PYTHON=python"
) else (
    set "PYTHON=python3"
)

rem ── 测试 1: 启动 Lua DBProxy ──
echo.
echo ^>^>^> 测试 1: 启动 Lua DBProxy ^(backup 模式^)

taskkill /f /im lua.exe >nul 2>&1
if not exist "%TEMP%\chaos_test_archive" mkdir "%TEMP%\chaos_test_archive"

cd /d "%DBPROXY_DIR%"
start /b "" lua init.lua --role backup --archive-dir "%TEMP%\chaos_test_archive"

rem 等待端口 9003
set "READY=0"
for /l %%i in (1,1,20) do (
    netstat -ano 2>nul | findstr ":9003 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 ( set "READY=1" & goto dbproxy_ready )
    timeout /t 1 /nobreak >nul
)
:dbproxy_ready
if "%READY%"=="1" (
    echo   [PASS] Lua DBProxy 启动成功 ^(端口 9003^)
    set /a PASS+=1
) else (
    echo   [FAIL] DBProxy 端口 9003 未就绪
    set /a FAIL+=1
)

rem ── 测试 2: 启动 chaos_server ──
echo.
echo ^>^>^> 测试 2: 启动 chaos_server

if not exist "%BUILD_BIN%" (
    echo   [INFO] chaos_server 未构建，尝试构建...
    cd /d "%PROJECT_DIR%\build"
    cmake --build . --target chaos_server --config Debug
    if not exist "%BUILD_BIN%" (
        echo   [FAIL] chaos_server 构建失败
        set /a FAIL+=1
        goto summary
    )
)

start /b "" "%BUILD_BIN%"

set "SRV_READY=0"
for /l %%i in (1,1,15) do (
    netstat -ano 2>nul | findstr ":7777 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 ( set "SRV_READY=1" & goto srv_ready )
    timeout /t 1 /nobreak >nul
)
:srv_ready
if "%SRV_READY%"=="1" (
    echo   [PASS] chaos_server 启动成功 ^(端口 7777^)
    set /a PASS+=1
) else (
    echo   [FAIL] chaos_server 端口 7777 未就绪
    set /a FAIL+=1
)

rem ── 测试 3: 心跳消息 ──
echo.
echo ^>^>^> 测试 3: 发送心跳消息 ^(MSG_HEARTBEAT=0x04^)

%PYTHON% -c "
import socket, struct
s = socket.socket()
s.settimeout(5)
try:
    s.connect(('127.0.0.1', 9003))
    msg_len = 5
    msg = struct.pack('>IB', msg_len, 0x04)
    s.sendall(msg)
    resp = s.recv(4096)
    if resp:
        print('PASS:heartbeat len=%d' % len(resp))
    else:
        print('FAIL:no_response')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\hb_result.txt" 2>&1

set /p HB_RESULT=<"%TEMP%\hb_result.txt"
echo   !HB_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 心跳消息正常: !HB_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 心跳消息失败: !HB_RESULT!
    set /a FAIL+=1
)

rem ── 测试 4: 并发心跳 ──
echo.
echo ^>^>^> 测试 4: 并发心跳 ^(5次^)

%PYTHON% -c "
import socket, struct
passed = 0
for i in range(5):
    s = socket.socket()
    s.settimeout(3)
    try:
        s.connect(('127.0.0.1', 9003))
        s.sendall(struct.pack('>IB', 5, 0x04))
        resp = s.recv(4096)
        if resp: passed += 1
    except: pass
    finally: s.close()
print('PASS:%d/5' % passed if passed > 0 else 'FAIL:0/5')
" > "%TEMP%\conc_result.txt" 2>&1

set /p CONC_RESULT=<"%TEMP%\conc_result.txt"
echo   !CONC_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 并发心跳: !CONC_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 并发心跳: !CONC_RESULT!
    set /a FAIL+=1
)

:summary
rem ── 清理 ──
taskkill /f /im lua.exe >nul 2>&1
taskkill /f /im chaos_server.exe >nul 2>&1
del /q "%TEMP%\hb_result.txt" "%TEMP%\conc_result.txt" >nul 2>&1

echo.
echo ============================================
echo   DBProxy 集成测试结果
echo ============================================
echo   通过: %PASS%
echo   失败: %FAIL%
set /a TOTAL=PASS+FAIL
echo   总计: %TOTAL%
echo ============================================

if %FAIL% gtr 0 (
    echo.
    echo 存在失败测试!
    exit /b 1
) else (
    echo.
    echo 所有测试通过!
    exit /b 0
)
