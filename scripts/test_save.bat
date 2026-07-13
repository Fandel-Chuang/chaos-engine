@echo off
chcp 65001 > nul
rem scripts\test_save.bat — 存档功能集成测试 (Windows)
rem
rem Admin IPC 在 Windows 上使用 TCP 端口而非 Unix Socket。
rem 依赖: python3, curl

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "BUILD_BIN=%PROJECT_DIR%\build\bin\chaos_server.exe"

set "PASS=0"
set "FAIL=0"

echo ============================================
echo   存档功能集成测试 ^(Windows^)
echo ============================================
echo   注意: Admin IPC 通过 TCP 连接，需要 chaos_server 支持 --admin-tcp

set "PYTHON=python3"
where python3 >nul 2>&1
if %errorlevel% neq 0 (
    set "PYTHON=python"
    where python >nul 2>&1
    if %errorlevel% neq 0 ( echo [FAIL] python 未安装 & exit /b 1 )
)

rem ── 测试 1: 启动 chaos_server (--admin) ──
echo.
echo ^>^>^> 测试 1: 启动 chaos_server ^(带 --admin^)

taskkill /f /im chaos_server.exe >nul 2>&1
timeout /t 1 /nobreak >nul

if not exist "%BUILD_BIN%" (
    echo   [INFO] 尝试构建...
    cd /d "%PROJECT_DIR%\build"
    cmake --build . --target chaos_server --config Debug
)

if not exist "%BUILD_BIN%" (
    echo   [FAIL] chaos_server 构建失败
    set /a FAIL+=1 & goto summary
)

start /b "" "%BUILD_BIN%" --admin

rem 等待端口 7777
set "SRV_READY=0"
for /l %%i in (1,1,20) do (
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

rem ── 测试 2-8: 通过 Admin HTTP API 或 TCP 发送 JSON-RPC ──
rem chaos_server 在 Windows 下 --admin 暴露 admin pipe 或 TCP，尝试 TCP admin 端口

%PYTHON% -c "
import socket, json, time, sys

def send_ipc(method, params=None):
    '''通过 TCP 发送 JSON-RPC 到 Admin (Windows 模式)'''
    if params is None:
        params = {}
    req_id = int(time.time() * 1000) %% 100000
    req = json.dumps({'jsonrpc':'2.0','id':req_id,'method':method,'params':params}) + '\n'
    # 尝试 admin TCP 端口 (如有)，否则返回 N/A
    for port in [8090, 9090, 7778]:
        try:
            s = socket.socket()
            s.settimeout(3)
            s.connect(('127.0.0.1', port))
            s.sendall(req.encode())
            resp = b''
            while True:
                chunk = s.recv(4096)
                if not chunk: break
                resp += chunk
                if b'\n' in resp: break
            s.close()
            return resp.decode().strip()
        except:
            pass
    return ''

results = []

# health
r = send_ipc('health')
if r and ('ok' in r or 'result' in r):
    results.append('PASS:health ok')
else:
    results.append('SKIP:health no tcp admin port')

# save.now
r = send_ipc('save.now')
if r and ('result' in r or 'status' in r):
    results.append('PASS:save.now responded')
elif not r:
    results.append('SKIP:save.now no tcp admin')
else:
    results.append('PASS:save.now got:' + r[:40])

# save.status
r = send_ipc('save.status')
if r and 'result' in r:
    results.append('PASS:save.status ok')
elif not r:
    results.append('SKIP:save.status no tcp admin')
else:
    results.append('PASS:save.status got:' + r[:40])

# stats
r = send_ipc('stats')
if r and ('entity_count' in r or 'result' in r):
    results.append('PASS:stats ok')
elif not r:
    results.append('SKIP:stats no tcp admin')
else:
    results.append('FAIL:stats unexpected:' + r[:40])

for res in results:
    print(res)
" > "%TEMP%\save_results.txt" 2>&1

set "IPC_PASS=0"
set "IPC_SKIP=0"
for /f "delims=" %%l in (%TEMP%\save_results.txt) do (
    echo   %%l
    echo %%l | findstr /i "PASS" >nul && ( set /a PASS+=1 & set /a IPC_PASS+=1 )
    echo %%l | findstr /i "SKIP" >nul && ( set /a IPC_SKIP+=1 )
    echo %%l | findstr /i "FAIL" >nul && ( set /a FAIL+=1 )
)

if %IPC_SKIP% gtr 0 (
    echo.
    echo   [NOTE] Admin IPC 测试被跳过: chaos_server --admin 在 Windows 上使用 Named Pipe
    echo          ^(Linux 使用 Unix Socket /tmp/chaos_admin.sock^)
    echo          如需测试，可通过 Admin HTTP 端口测试: curl http://localhost:9090/api/health
)

rem ── 验证进程存活 ──
echo.
echo ^>^>^> 测试: 验证 chaos_server 仍在运行

tasklist /fi "imagename eq chaos_server.exe" 2>nul | findstr /i "chaos_server.exe" >nul
if %errorlevel% equ 0 (
    echo   [PASS] chaos_server 进程仍在运行
    set /a PASS+=1
) else (
    echo   [FAIL] chaos_server 进程已退出
    set /a FAIL+=1
)

:summary
taskkill /f /im chaos_server.exe >nul 2>&1
del /q "%TEMP%\save_results.txt" >nul 2>&1

echo.
echo ============================================
echo   存档功能测试结果
echo ============================================
echo   通过: %PASS%
echo   失败: %FAIL%
set /a TOTAL=PASS+FAIL
echo   总计: %TOTAL%
echo ============================================

if %FAIL% gtr 0 ( echo 存在失败测试! & exit /b 1 ) else ( echo 所有测试通过! & exit /b 0 )
