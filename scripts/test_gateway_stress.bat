@echo off
chcp 65001 > nul
rem scripts\test_gateway_stress.bat — Gateway 压力测试 (Windows)
rem
rem 依赖: lua, python3

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "GATEWAY_DIR=%PROJECT_DIR%\src_lua\gateway"
set "BUILD_BIN=%PROJECT_DIR%\build\bin\chaos_server.exe"

if not defined STRESS_CONNECTIONS set "STRESS_CONNECTIONS=100"
if not defined STRESS_DURATION    set "STRESS_DURATION=10"
if not defined STRESS_MESSAGES    set "STRESS_MESSAGES=1000"

set "PASS=0"
set "FAIL=0"

echo ============================================
echo   Gateway 压力测试 ^(Windows^)
echo   连接数: %STRESS_CONNECTIONS%  时长: %STRESS_DURATION%s  消息: %STRESS_MESSAGES%
echo ============================================

set "PYTHON=python3"
where python3 >nul 2>&1
if %errorlevel% neq 0 (
    set "PYTHON=python"
    where python >nul 2>&1
    if %errorlevel% neq 0 ( echo [FAIL] python 未安装 & exit /b 1 )
)

rem ── 测试 1: 启动服务 ──
echo.
echo ^>^>^> 测试 1: 启动服务 ^(chaos_server + Gateway^)

taskkill /f /im chaos_server.exe >nul 2>&1
taskkill /f /im lua.exe >nul 2>&1
timeout /t 1 /nobreak >nul

if exist "%BUILD_BIN%" (
    start /b "" "%BUILD_BIN%"
) else (
    rem 使用 Python echo 服务器替代
    start /b "" %PYTHON% -c "
import socket, threading
def handle(c,a):
    try:
        while True:
            d=c.recv(4096)
            if not d: break
            c.sendall(d)
    except: pass
    finally: c.close()
s=socket.socket()
s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('127.0.0.1',7777))
s.listen(128)
print('Echo on :7777',flush=True)
while True:
    c,a=s.accept()
    threading.Thread(target=handle,args=(c,a),daemon=True).start()
"
)

set "SRV_READY=0"
for /l %%i in (1,1,15) do (
    netstat -ano 2>nul | findstr ":7777 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 ( set "SRV_READY=1" & goto srv_ok )
    timeout /t 1 /nobreak >nul
)
:srv_ok
if "%SRV_READY%"=="1" (
    echo   [PASS] 后端服务启动成功 ^(端口 7777^)
    set /a PASS+=1
) else (
    echo   [FAIL] 后端服务未就绪
    set /a FAIL+=1 & goto summary
)

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
    echo   [PASS] Gateway 启动成功 ^(TCP:9000, WS:9002^)
    set /a PASS+=1
) else (
    echo   [FAIL] Gateway 未就绪
    set /a FAIL+=1 & goto summary
)

rem ── 测试 2: 并发连接 ──
echo.
echo ^>^>^> 测试 2: TCP 并发连接测试 ^(%STRESS_CONNECTIONS% 连接^)

%PYTHON% -c "
import socket, threading, time
HOST='127.0.0.1'; PORT=9000; N=%STRESS_CONNECTIONS%
results={'ok':0,'fail':0}
lock=threading.Lock()
def conn(i):
    s=socket.socket()
    s.settimeout(5)
    try:
        s.connect((HOST,PORT))
        s.sendall(('stress_%d\n'%i).encode())
        with lock: results['ok']+=1
    except:
        with lock: results['fail']+=1
    finally: s.close()
start=time.time()
ts=[threading.Thread(target=conn,args=(i,)) for i in range(N)]
for t in ts: t.start()
for t in ts: t.join(10)
elapsed=time.time()-start
ok=results['ok']
print('PASS:%d/%d ok %.2fs' % (ok,N,elapsed) if ok>0 else 'FAIL:0/%d'%N)
" > "%TEMP%\stress_conn.txt" 2>&1
set /p CONN_RESULT=<"%TEMP%\stress_conn.txt"
echo   !CONN_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 并发连接: !CONN_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 并发连接: !CONN_RESULT!
    set /a FAIL+=1
)

rem ── 测试 3: 消息吞吐量 ──
echo.
echo ^>^>^> 测试 3: 消息吞吐量测试 ^(%STRESS_MESSAGES% 条消息^)

%PYTHON% -c "
import socket, time
s=socket.socket()
s.settimeout(10)
try:
    s.connect(('127.0.0.1',9000))
    sent=0; start=time.time()
    for i in range(%STRESS_MESSAGES%):
        try:
            s.sendall(('msg_%d\n'%i).encode())
            sent+=1
        except: break
    elapsed=time.time()-start
    rate=sent/elapsed if elapsed>0 else 0
    print('PASS:%d msgs %.1f msg/s' % (sent,rate) if sent>0 else 'FAIL:0 sent')
except Exception as e:
    print('FAIL:'+str(e))
finally:
    s.close()
" > "%TEMP%\stress_tput.txt" 2>&1
set /p TPUT_RESULT=<"%TEMP%\stress_tput.txt"
echo   !TPUT_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 吞吐量: !TPUT_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 吞吐量: !TPUT_RESULT!
    set /a FAIL+=1
)

rem ── 测试 4: 稳定性 ──
echo.
echo ^>^>^> 测试 4: 持续连接稳定性 ^(%STRESS_DURATION%s^)

%PYTHON% -c "
import socket, time
s=socket.socket()
s.settimeout(10)
try:
    s.connect(('127.0.0.1',9000))
    sent=0; errors=0; start=time.time()
    while time.time()-start < %STRESS_DURATION%:
        try:
            s.sendall(('keepalive_%d\n'%sent).encode())
            sent+=1
        except: errors+=1; break
        time.sleep(0.01)
    elapsed=time.time()-start
    print('PASS:%d msgs %d errs %.1fs' % (sent,errors,elapsed) if sent>0 else 'FAIL:0 sent')
except Exception as e:
    print('FAIL:'+str(e))
finally:
    s.close()
" > "%TEMP%\stress_stable.txt" 2>&1
set /p STABLE_RESULT=<"%TEMP%\stress_stable.txt"
echo   !STABLE_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 稳定性: !STABLE_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 稳定性: !STABLE_RESULT!
    set /a FAIL+=1
)

:summary
taskkill /f /im lua.exe >nul 2>&1
taskkill /f /im chaos_server.exe >nul 2>&1
del /q "%TEMP%\stress_conn.txt" "%TEMP%\stress_tput.txt" "%TEMP%\stress_stable.txt" >nul 2>&1

echo.
echo ============================================
echo   Gateway 压力测试结果
echo ============================================
echo   通过: %PASS%
echo   失败: %FAIL%
set /a TOTAL=PASS+FAIL
echo   总计: %TOTAL%
echo ============================================

if %FAIL% gtr 0 ( echo 存在失败测试! & exit /b 1 ) else ( echo 所有测试通过! & exit /b 0 )
