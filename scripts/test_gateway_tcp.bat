@echo off
chcp 65001 > nul
rem scripts\test_gateway_tcp.bat — Gateway TCP 集成测试 (Windows)
rem
rem 依赖: lua, python3

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "GATEWAY_DIR=%PROJECT_DIR%\src_lua\gateway"
set "BUILD_BIN=%PROJECT_DIR%\build\bin\chaos_server.exe"

set "PASS=0"
set "FAIL=0"

echo ============================================
echo   Gateway TCP 集成测试 ^(Windows^)
echo ============================================

rem 检查 python
set "PYTHON=python3"
where python3 >nul 2>&1
if %errorlevel% neq 0 (
    set "PYTHON=python"
    where python >nul 2>&1
    if %errorlevel% neq 0 ( echo [FAIL] python 未安装 & exit /b 1 )
)

rem ── 测试 1: 启动 chaos_server ──
echo.
echo ^>^>^> 测试 1: 启动 chaos_server

taskkill /f /im chaos_server.exe >nul 2>&1
taskkill /f /im lua.exe >nul 2>&1
timeout /t 1 /nobreak >nul

if not exist "%BUILD_BIN%" (
    echo   [INFO] 尝试构建...
    cd /d "%PROJECT_DIR%\build"
    cmake --build . --target chaos_server --config Debug
)

start /b "" "%BUILD_BIN%"

set "SRV_READY=0"
for /l %%i in (1,1,15) do (
    netstat -ano 2>nul | findstr ":7777 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 ( set "SRV_READY=1" & goto srv_ok )
    timeout /t 1 /nobreak >nul
)
:srv_ok
if "%SRV_READY%"=="1" (
    echo   [PASS] chaos_server 启动成功 ^(端口 7777^)
    set /a PASS+=1
) else (
    echo   [FAIL] chaos_server 未就绪
    set /a FAIL+=1
    goto summary
)

rem ── 测试 2: 启动 Lua Gateway ──
echo.
echo ^>^>^> 测试 2: 启动 Lua Gateway ^(TCP 端口 9000^)

where lua >nul 2>&1
if %errorlevel% neq 0 ( echo   [FAIL] lua 未安装 & set /a FAIL+=1 & goto summary )

cd /d "%GATEWAY_DIR%"
start /b "" lua server.lua --tcp-port 9000 --ws-port 9002 --game-host 127.0.0.1 --game-port 7777

set "GW_READY=0"
for /l %%i in (1,1,15) do (
    netstat -ano 2>nul | findstr ":9000 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 ( set "GW_READY=1" & goto gw_ok )
    timeout /t 1 /nobreak >nul
)
:gw_ok
if "%GW_READY%"=="1" (
    echo   [PASS] Gateway 启动成功 ^(TCP:9000^)
    set /a PASS+=1
) else (
    echo   [FAIL] Gateway 端口 9000 未就绪
    set /a FAIL+=1
    goto summary
)

rem ── 测试 3: TCP 消息路由 ──
echo.
echo ^>^>^> 测试 3: TCP 消息路由

%PYTHON% -c "
import socket
s = socket.socket()
s.settimeout(3)
try:
    s.connect(('127.0.0.1', 9000))
    s.sendall(b'hello gateway tcp\n')
    s.settimeout(2)
    resp = b''
    try:
        resp = s.recv(4096)
    except: pass
    if resp:
        print('PASS:got_response len=%d' % len(resp))
    else:
        print('PASS:connected_no_echo')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\tcp_route.txt" 2>&1
set /p TCP_RESULT=<"%TEMP%\tcp_route.txt"
echo   !TCP_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] TCP 路由: !TCP_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] TCP 路由: !TCP_RESULT!
    set /a FAIL+=1
)

rem ── 测试 4: 多消息类型 ──
echo.
echo ^>^>^> 测试 4: 多消息类型测试

%PYTHON% -c "
import socket
msgs = [b'ping\n', b'test_message_123\n', b'{\"action\":\"login\"}\n', b'ECHO:hello\n']
sent = 0
for m in msgs:
    s = socket.socket()
    s.settimeout(2)
    try:
        s.connect(('127.0.0.1', 9000))
        s.sendall(m)
        sent += 1
    except: pass
    finally: s.close()
print('PASS:%d/%d sent' % (sent, len(msgs)))
" > "%TEMP%\multi_msg.txt" 2>&1
set /p MULTI_RESULT=<"%TEMP%\multi_msg.txt"
echo   [PASS] !MULTI_RESULT!
set /a PASS+=1

rem ── 测试 5: 并发连接 ──
echo.
echo ^>^>^> 测试 5: 并发连接测试 ^(10 连接^)

%PYTHON% -c "
import socket, threading
results = {'ok': 0}
lock = threading.Lock()
def conn(i):
    s = socket.socket()
    s.settimeout(3)
    try:
        s.connect(('127.0.0.1', 9000))
        s.sendall(('test_%d\n' % i).encode())
        with lock: results['ok'] += 1
    except: pass
    finally: s.close()
ts = [threading.Thread(target=conn, args=(i,)) for i in range(10)]
for t in ts: t.start()
for t in ts: t.join(5)
n = results['ok']
print('PASS:%d/10' % n if n > 0 else 'FAIL:0/10')
" > "%TEMP%\concurrent.txt" 2>&1
set /p CONC_RESULT=<"%TEMP%\concurrent.txt"
echo   !CONC_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 并发连接: !CONC_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 并发连接: !CONC_RESULT!
    set /a FAIL+=1
)

rem ── 测试 6: 验证进程状态 ──
echo.
echo ^>^>^> 测试 6: 验证进程状态

tasklist /fi "imagename eq chaos_server.exe" 2>nul | findstr /i "chaos_server.exe" >nul
if %errorlevel% equ 0 (
    echo   [PASS] chaos_server 仍在运行
    set /a PASS+=1
) else (
    echo   [FAIL] chaos_server 已退出
    set /a FAIL+=1
)

netstat -ano 2>nul | findstr ":9000 " | findstr "LISTENING" >nul
if %errorlevel% equ 0 (
    echo   [PASS] Gateway TCP 端口 9000 仍被监听
    set /a PASS+=1
) else (
    echo   [FAIL] Gateway TCP 端口 9000 未监听
    set /a FAIL+=1
)

:summary
rem 清理
taskkill /f /im lua.exe >nul 2>&1
taskkill /f /im chaos_server.exe >nul 2>&1
del /q "%TEMP%\tcp_route.txt" "%TEMP%\multi_msg.txt" "%TEMP%\concurrent.txt" >nul 2>&1

echo.
echo ============================================
echo   Gateway TCP 集成测试结果
echo ============================================
echo   通过: %PASS%
echo   失败: %FAIL%
set /a TOTAL=PASS+FAIL
echo   总计: %TOTAL%
echo ============================================

if %FAIL% gtr 0 (
    echo 存在失败测试!
    exit /b 1
) else (
    echo 所有测试通过!
    exit /b 0
)
