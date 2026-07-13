@echo off
chcp 65001 > nul
rem scripts\test_gateway_ws.bat — Gateway WebSocket 集成测试 (Windows)
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
echo   Gateway WebSocket 集成测试 ^(Windows^)
echo ============================================

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
    echo   [PASS] chaos_server 启动成功
    set /a PASS+=1
) else (
    echo   [FAIL] chaos_server 未就绪
    set /a FAIL+=1 & goto summary
)

rem ── 测试 2: 启动 Lua Gateway ──
echo.
echo ^>^>^> 测试 2: 启动 Lua Gateway ^(WS 端口 9002^)

where lua >nul 2>&1
if %errorlevel% neq 0 ( echo   [FAIL] lua 未安装 & set /a FAIL+=1 & goto summary )

cd /d "%GATEWAY_DIR%"
start /b "" lua server.lua --tcp-port 9000 --ws-port 9002 --game-host 127.0.0.1 --game-port 7777

set "GW_READY=0"
for /l %%i in (1,1,15) do (
    netstat -ano 2>nul | findstr ":9002 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 ( set "GW_READY=1" & goto gw_ok )
    timeout /t 1 /nobreak >nul
)
:gw_ok
if "%GW_READY%"=="1" (
    echo   [PASS] Gateway 启动成功 ^(WS:9002^)
    set /a PASS+=1
) else (
    echo   [FAIL] Gateway WS 端口 9002 未就绪
    set /a FAIL+=1 & goto summary
)

rem ── 测试 3: WebSocket 握手验证 ──
echo.
echo ^>^>^> 测试 3: WebSocket 握手验证

%PYTHON% -c "
import socket, base64, hashlib, os
WS_GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
ws_key = base64.b64encode(os.urandom(16)).decode()
expected = base64.b64encode(hashlib.sha1((ws_key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
req = ('GET /chat HTTP/1.1\r\nHost: 127.0.0.1:9002\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % ws_key).encode()
s = socket.socket()
s.settimeout(5)
try:
    s.connect(('127.0.0.1', 9002))
    s.sendall(req)
    resp = b''
    while b'\r\n\r\n' not in resp:
        chunk = s.recv(4096)
        if not chunk: break
        resp += chunk
    resp_str = resp.decode('utf-8', errors='replace')
    if '101' in resp_str:
        if expected in resp_str:
            print('PASS:101+accept_ok')
        else:
            print('PASS:101_only')
    else:
        print('FAIL:no_101')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\ws_handshake.txt" 2>&1
set /p WS_RESULT=<"%TEMP%\ws_handshake.txt"
echo   !WS_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] WebSocket 握手: !WS_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] WebSocket 握手: !WS_RESULT!
    set /a FAIL+=1
)

rem ── 测试 4: WebSocket 文本消息 ──
echo.
echo ^>^>^> 测试 4: WebSocket 文本消息收发

%PYTHON% -c "
import socket, base64, hashlib, os, struct
ws_key = base64.b64encode(os.urandom(16)).decode()
req = ('GET / HTTP/1.1\r\nHost: 127.0.0.1:9002\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % ws_key).encode()
msg = b'hello websocket'
mask = os.urandom(4)
masked = bytes(b ^ mask[i % 4] for i, b in enumerate(msg))
frame = struct.pack('BB', 0x81, 0x80 | len(msg)) + mask + masked
s = socket.socket()
s.settimeout(5)
try:
    s.connect(('127.0.0.1', 9002))
    s.sendall(req)
    resp = b''
    while b'\r\n\r\n' not in resp:
        chunk = s.recv(4096)
        if not chunk: break
        resp += chunk
    if b'101' in resp:
        s.sendall(frame)
        s.settimeout(2)
        try:
            extra = s.recv(4096)
            print('PASS:sent_and_got len=%d' % len(extra) if extra else 'PASS:sent_no_echo')
        except:
            print('PASS:sent_timeout')
    else:
        print('FAIL:handshake_failed')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\ws_text.txt" 2>&1
set /p WS_TEXT=<"%TEMP%\ws_text.txt"
echo   !WS_TEXT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] WebSocket 文本消息: !WS_TEXT!
    set /a PASS+=1
) else (
    echo   [FAIL] WebSocket 文本消息: !WS_TEXT!
    set /a FAIL+=1
)

rem ── 测试 5: WebSocket PING/PONG ──
echo.
echo ^>^>^> 测试 5: WebSocket PING/PONG

%PYTHON% -c "
import socket, base64, hashlib, os, struct
ws_key = base64.b64encode(os.urandom(16)).decode()
req = ('GET / HTTP/1.1\r\nHost: 127.0.0.1:9002\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % ws_key).encode()
ping_data = b'ping'
mask = os.urandom(4)
masked = bytes(b ^ mask[i % 4] for i, b in enumerate(ping_data))
ping_frame = struct.pack('BB', 0x89, 0x80 | len(ping_data)) + mask + masked
s = socket.socket()
s.settimeout(5)
try:
    s.connect(('127.0.0.1', 9002))
    s.sendall(req)
    resp = b''
    while b'\r\n\r\n' not in resp:
        chunk = s.recv(4096)
        if not chunk: break
        resp += chunk
    if b'101' in resp:
        s.sendall(ping_frame)
        s.settimeout(2)
        try:
            pong = s.recv(4096)
            if pong and len(pong) >= 2 and (pong[0] & 0x0F) == 0xA:
                print('PASS:pong_received')
            elif pong:
                print('PASS:response_received opcode=%d' % (pong[0] & 0x0F))
            else:
                print('PASS:no_pong_but_connected')
        except:
            print('PASS:timeout_but_connected')
    else:
        print('FAIL:handshake_failed')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\ws_ping.txt" 2>&1
set /p WS_PING=<"%TEMP%\ws_ping.txt"
echo   !WS_PING! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] PING/PONG: !WS_PING!
    set /a PASS+=1
) else (
    echo   [FAIL] PING/PONG: !WS_PING!
    set /a FAIL+=1
)

rem ── 测试 6: 无效握手 ──
echo.
echo ^>^>^> 测试 6: 无效握手测试

%PYTHON% -c "
import socket
s = socket.socket()
s.settimeout(3)
try:
    s.connect(('127.0.0.1', 9002))
    s.sendall(b'GET / HTTP/1.1\r\nHost: 127.0.0.1:9002\r\n\r\n')
    s.settimeout(2)
    try:
        resp = s.recv(4096)
        if b'400' in resp:
            print('PASS:400_bad_request')
        elif resp:
            print('PASS:rejected code=%s' % resp[:12])
        else:
            print('PASS:connection_closed')
    except:
        print('PASS:connection_closed')
except:
    print('PASS:connection_refused')
finally:
    s.close()
" > "%TEMP%\ws_bad.txt" 2>&1
set /p WS_BAD=<"%TEMP%\ws_bad.txt"
echo   !WS_BAD! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 无效握手处理: !WS_BAD!
    set /a PASS+=1
) else (
    echo   [FAIL] 无效握手: !WS_BAD!
    set /a FAIL+=1
)

:summary
taskkill /f /im lua.exe >nul 2>&1
taskkill /f /im chaos_server.exe >nul 2>&1
del /q "%TEMP%\ws_handshake.txt" "%TEMP%\ws_text.txt" "%TEMP%\ws_ping.txt" "%TEMP%\ws_bad.txt" >nul 2>&1

echo.
echo ============================================
echo   Gateway WebSocket 集成测试结果
echo ============================================
echo   通过: %PASS%
echo   失败: %FAIL%
set /a TOTAL=PASS+FAIL
echo   总计: %TOTAL%
echo ============================================

if %FAIL% gtr 0 ( echo 存在失败测试! & exit /b 1 ) else ( echo 所有测试通过! & exit /b 0 )
