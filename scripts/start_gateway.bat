@echo off
chcp 65001 > nul
rem scripts\start_gateway.bat — ChaosEngine Gateway 启动脚本 (Windows)
rem
rem 用法:
rem   scripts\start_gateway.bat
rem   scripts\start_gateway.bat --port 9000
rem   scripts\start_gateway.bat --backend 127.0.0.1:7777
rem   scripts\start_gateway.bat --log-level debug

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "GATEWAY_DIR=%PROJECT_DIR%\src_lua\gateway"

set "GATEWAY_PORT=9000"
set "BACKEND=127.0.0.1:7777"
set "MAX_CONNECTIONS=10000"
set "LOG_LEVEL=info"
set "LOG_DIR=%PROJECT_DIR%\logs"
set "LOG_FILE=%LOG_DIR%\gateway.log"

:parse_args
if "%~1"=="" goto start
if "%~1"=="--port"            ( set "GATEWAY_PORT=%~2" & shift & shift & goto parse_args )
if "%~1"=="--backend"         ( set "BACKEND=%~2"      & shift & shift & goto parse_args )
if "%~1"=="--max-connections" ( set "MAX_CONNECTIONS=%~2" & shift & shift & goto parse_args )
if "%~1"=="--log-level"       ( set "LOG_LEVEL=%~2"    & shift & shift & goto parse_args )
if "%~1"=="--help" goto usage
if "%~1"=="-h" goto usage
echo [WARN] 未知参数: %~1 & shift & goto parse_args

:usage
echo Usage: %~nx0 [OPTIONS]
echo.
echo Options:
echo   --port PORT             TCP listen port (default: 9000)
echo   --backend HOST:PORT     Backend Game service (default: 127.0.0.1:7777)
echo   --max-connections N     Max client connections (default: 10000)
echo   --log-level LEVEL       Log level: trace, debug, info, warn, error (default: info)
echo   --help, -h              Show this help
exit /b 0

:start
rem 检查环境
echo.
echo ^>^>^> 检查环境...

where lua5.4 >nul 2>&1
if %errorlevel% equ 0 (
    set "LUA_BIN=lua5.4"
    goto lua_found
)
where lua >nul 2>&1
if %errorlevel% equ 0 (
    set "LUA_BIN=lua"
    goto lua_found
)
echo [ERROR] 未找到 Lua，请安装 Lua 5.4 并配置 PATH
exit /b 1

:lua_found
echo [OK] Lua: %LUA_BIN%

rem 准备目录
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

rem 检查端口占用
netstat -ano | findstr ":%GATEWAY_PORT% " | findstr "LISTENING" >nul 2>&1
if %errorlevel% equ 0 (
    echo [WARN] 端口 %GATEWAY_PORT% 已被占用
    rem 尝试找到并终止占用进程
    for /f "tokens=5" %%p in ('netstat -ano ^| findstr ":%GATEWAY_PORT% " ^| findstr "LISTENING"') do (
        echo [WARN] 终止 PID %%p...
        taskkill /f /pid %%p >nul 2>&1
    )
    timeout /t 1 /nobreak >nul
)

rem 启动 Gateway
echo.
echo ^>^>^> 启动 Gateway...

cd /d "%GATEWAY_DIR%"

echo ============================================
echo   Gateway 配置
echo   TCP 端口:    %GATEWAY_PORT%
echo   后端服务:    %BACKEND%
echo   最大连接数:  %MAX_CONNECTIONS%
echo   日志:        %LOG_FILE%
echo ============================================
echo.

start /b "" %LUA_BIN% init.lua ^
    --port "%GATEWAY_PORT%" ^
    --backend "%BACKEND%" ^
    --max-connections "%MAX_CONNECTIONS%" ^
    --log-level "%LOG_LEVEL%" >> "%LOG_FILE%" 2>&1

echo [OK] Gateway 已启动
echo.
echo 停止 Gateway:
echo   taskkill /f /im lua.exe
echo.
echo 查看日志:
echo   type %LOG_FILE%
echo.

exit /b 0
