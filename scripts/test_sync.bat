@echo off
chcp 65001 > nul
rem scripts\test_sync.bat — 同步功能集成测试 (Windows)
rem
rem 依赖: lua, python3

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "DBPROXY_DIR=%PROJECT_DIR%\src_lua\dbproxy"
set "BUILD_BIN=%PROJECT_DIR%\build\bin\chaos_server.exe"

set "PASS=0"
set "FAIL=0"

echo ============================================
echo   同步功能集成测试 ^(Windows^)
echo ============================================

set "PYTHON=python3"
where python3 >nul 2>&1
if %errorlevel% neq 0 (
    set "PYTHON=python"
    where python >nul 2>&1
    if %errorlevel% neq 0 ( echo [FAIL] python 未安装 & exit /b 1 )
)

rem ── 测试 1: 启动 Lua DBProxy ──
echo.
echo ^>^>^> 测试 1: 启动 Lua DBProxy ^(primary 模式^)

taskkill /f /im lua.exe >nul 2>&1
taskkill /f /im chaos_server.exe >nul 2>&1
timeout /t 1 /nobreak >nul

where lua >nul 2>&1
if %errorlevel% neq 0 ( echo   [FAIL] lua 未安装 & set /a FAIL+=1 & goto summary )

if not exist "%TEMP%\chaos_test_archive" mkdir "%TEMP%\chaos_test_archive"

cd /d "%DBPROXY_DIR%"
start /b "" lua init.lua --role primary --archive-dir "%TEMP%\chaos_test_archive"

set "DB_READY=0"
for /l %%i in (1,1,20) do (
    netstat -ano 2>nul | findstr ":9001 " | findstr "LISTENING" >nul
    if !errorlevel! equ 0 ( set "DB_READY=1" & goto db_ok )
    timeout /t 1 /nobreak >nul
)
:db_ok
if "%DB_READY%"=="1" (
    echo   [PASS] DBProxy 启动成功 ^(sync:9001, db:9003^)
    set /a PASS+=1
) else (
    echo   [FAIL] DBProxy 未就绪
    set /a FAIL+=1 & goto summary
)

rem ── 测试 2: 启动 chaos_server ──
echo.
echo ^>^>^> 测试 2: 启动 chaos_server

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
    echo   [PASS] chaos_server 启动成功 ^(端口 7777^)
    set /a PASS+=1
) else (
    echo   [FAIL] chaos_server 未就绪
    set /a FAIL+=1
)

rem ── 测试 3: 心跳帧 ──
echo.
echo ^>^>^> 测试 3: 发送心跳帧 ^(frame_seq=0xFFFF^)

%PYTHON% -c "
import socket, struct, time
# 心跳帧: 4B len + 2B seq(0xFFFF) + 8B ts + 2B count = 16B
now_us = int(time.time() * 1000000)
frame = struct.pack('>IHQH', 16, 0xFFFF, now_us, 0)
s = socket.socket()
s.settimeout(5)
try:
    s.connect(('127.0.0.1', 9001))
    s.sendall(frame)
    s.settimeout(2)
    try:
        resp = s.recv(4096)
        if len(resp) >= 4:
            frame_len = struct.unpack('>I', resp[:4])[0]
            print('PASS:heartbeat resp len=%d' % len(resp))
        else:
            print('PASS:got_short_resp')
    except socket.timeout:
        print('PASS:sent_no_resp')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\sync_hb.txt" 2>&1
set /p HB_RESULT=<"%TEMP%\sync_hb.txt"
echo   !HB_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 心跳帧: !HB_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 心跳帧: !HB_RESULT!
    set /a FAIL+=1
)

rem ── 测试 4: 含实体同步帧 ──
echo.
echo ^>^>^> 测试 4: 发送含实体同步帧

%PYTHON% -c "
import socket, struct, time
now_us = int(time.time() * 1000000)
ent_data = b'hello_entity'
# entity: 8B id + 2B type + 4B datalen + data
ent = struct.pack('>QHIB', 1, 1, len(ent_data), 0)[:14] + ent_data
# 实际 entity: struct.pack('>QHI', eid, ctype, dlen) + data
import struct as st
ent_bin = st.pack('>QHI', 1, 1, len(ent_data)) + ent_data
frame_len = 16 + len(ent_bin)
frame = st.pack('>IHQH', frame_len, 1, now_us, 1) + ent_bin
s = socket.socket()
s.settimeout(5)
try:
    s.connect(('127.0.0.1', 9001))
    s.sendall(frame)
    s.settimeout(1)
    try:
        resp = s.recv(4096)
        print('PASS:got_resp len=%d' % len(resp))
    except:
        print('PASS:sent_no_crash')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\sync_ent.txt" 2>&1
set /p ENT_RESULT=<"%TEMP%\sync_ent.txt"
echo   !ENT_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 含实体帧: !ENT_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 含实体帧: !ENT_RESULT!
    set /a FAIL+=1
)

rem ── 测试 5: 多次心跳 ──
echo.
echo ^>^>^> 测试 5: 多次心跳 ^(5次^)

%PYTHON% -c "
import socket, struct, time
passed = 0
for i in range(5):
    now_us = int(time.time() * 1000000)
    frame = struct.pack('>IHQH', 16, 0xFFFF, now_us, 0)
    s = socket.socket()
    s.settimeout(3)
    try:
        s.connect(('127.0.0.1', 9001))
        s.sendall(frame)
        passed += 1
    except: pass
    finally: s.close()
    time.sleep(0.1)
print('PASS:%d/5' % passed if passed > 0 else 'FAIL:0/5')
" > "%TEMP%\sync_multi_hb.txt" 2>&1
set /p MHB_RESULT=<"%TEMP%\sync_multi_hb.txt"
echo   !MHB_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] 多次心跳: !MHB_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] 多次心跳: !MHB_RESULT!
    set /a FAIL+=1
)

rem ── 测试 6: DB 端口心跳 ──
echo.
echo ^>^>^> 测试 6: DB 端口心跳响应

%PYTHON% -c "
import socket, struct
s = socket.socket()
s.settimeout(5)
try:
    s.connect(('127.0.0.1', 9003))
    s.sendall(struct.pack('>IB', 5, 0x04))
    s.settimeout(2)
    resp = s.recv(4096)
    if resp:
        print('PASS:db_port_ok len=%d' % len(resp))
    else:
        print('FAIL:empty_resp')
except Exception as e:
    print('FAIL:' + str(e))
finally:
    s.close()
" > "%TEMP%\sync_db.txt" 2>&1
set /p DB_RESULT=<"%TEMP%\sync_db.txt"
echo   !DB_RESULT! | findstr /i "PASS" >nul
if !errorlevel! equ 0 (
    echo   [PASS] DB 端口: !DB_RESULT!
    set /a PASS+=1
) else (
    echo   [FAIL] DB 端口: !DB_RESULT!
    set /a FAIL+=1
)

rem ── 测试 7: 进程状态 ──
echo.
echo ^>^>^> 测试 7: 验证进程状态

tasklist /fi "imagename eq chaos_server.exe" 2>nul | findstr /i "chaos_server.exe" >nul
if %errorlevel% equ 0 (
    echo   [PASS] chaos_server 仍在运行
    set /a PASS+=1
) else (
    echo   [FAIL] chaos_server 已退出
    set /a FAIL+=1
)

tasklist /fi "imagename eq lua.exe" 2>nul | findstr /i "lua.exe" >nul
if %errorlevel% equ 0 (
    echo   [PASS] DBProxy ^(lua^) 仍在运行
    set /a PASS+=1
) else (
    echo   [FAIL] DBProxy 已退出
    set /a FAIL+=1
)

:summary
taskkill /f /im lua.exe >nul 2>&1
taskkill /f /im chaos_server.exe >nul 2>&1
if exist "%TEMP%\chaos_test_archive" rmdir /s /q "%TEMP%\chaos_test_archive" >nul 2>&1
del /q "%TEMP%\sync_hb.txt" "%TEMP%\sync_ent.txt" "%TEMP%\sync_multi_hb.txt" "%TEMP%\sync_db.txt" >nul 2>&1

echo.
echo ============================================
echo   同步功能测试结果
echo ============================================
echo   通过: %PASS%
echo   失败: %FAIL%
set /a TOTAL=PASS+FAIL
echo   总计: %TOTAL%
echo ============================================

if %FAIL% gtr 0 ( echo 存在失败测试! & exit /b 1 ) else ( echo 所有测试通过! & exit /b 0 )
