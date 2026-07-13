@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "BUILD_DIR=%PROJECT_DIR%\build"
set "LOG_DIR=%PROJECT_DIR%\logs"

set "BIN_DIR=%BUILD_DIR%\bin"
if exist "%BUILD_DIR%\bin\Debug\chaos_dbproxy.exe"   set "BIN_DIR=%BUILD_DIR%\bin\Debug"
if exist "%BUILD_DIR%\bin\Release\chaos_dbproxy.exe" set "BIN_DIR=%BUILD_DIR%\bin\Release"

set "GAME_PORT=7777"
set "GATEWAY_TCP=9000"
set "ROUTER_GAME=9100"
set "ROUTER_CLUSTER=9101"
set "DBPROXY_SYNC=9001"
set "DBPROXY_DB=9003"
set "ADMIN_PORT=9090"
set "START_GAME=0"
set "START_GATEWAY=0"
set "START_ROUTER=0"
set "START_DBPROXY=0"
set "START_ADMIN=0"

if "%~1"=="" (
    set "START_GAME=1"
    set "START_GATEWAY=1"
    set "START_ROUTER=1"
    set "START_DBPROXY=1"
    set "START_ADMIN=1"
    goto start_services
)

:parse_args
if "%~1"=="" goto start_services
if "%~1"=="--game"    ( set "START_GAME=1"    & shift & goto parse_args )
if "%~1"=="--gateway" ( set "START_GATEWAY=1" & shift & goto parse_args )
if "%~1"=="--router"  ( set "START_ROUTER=1"  & shift & goto parse_args )
if "%~1"=="--dbproxy" ( set "START_DBPROXY=1" & shift & goto parse_args )
if "%~1"=="--admin"   ( set "START_ADMIN=1"   & shift & goto parse_args )
if "%~1"=="--all" (
    set "START_GAME=1"
    set "START_GATEWAY=1"
    set "START_ROUTER=1"
    set "START_DBPROXY=1"
    set "START_ADMIN=1"
    shift & goto parse_args
)
echo Unknown arg: %~1 & exit /b 1

:start_services
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

echo ============================================
echo  ChaosEngine Cluster Start
echo  BIN: %BIN_DIR%
echo ============================================

if not "%START_DBPROXY%"=="1" goto skip_dbproxy
if not exist "%BIN_DIR%\chaos_dbproxy.exe" (
    echo [ERROR] chaos_dbproxy.exe not found - run build_and_test.bat
    exit /b 1
)
echo [INFO] Starting dbproxy...
start "chaos_dbproxy" cmd /c "%BIN_DIR%\chaos_dbproxy.exe --role primary --sync-port %DBPROXY_SYNC% --db-port %DBPROXY_DB% > %LOG_DIR%\dbproxy.log 2>nul"
echo [OK] dbproxy started  [log: logs\dbproxy.log]
:skip_dbproxy

if not "%START_GAME%"=="1" goto skip_game
if not exist "%BIN_DIR%\chaos_server.exe" (
    echo [SKIP] chaos_server.exe not found ^(Linux-only target^)
    goto skip_game
)
echo [INFO] Starting game server...
start "chaos_server" cmd /c "%BIN_DIR%\chaos_server.exe --admin > %LOG_DIR%\server.log 2>nul"
echo [OK] game server started  [log: logs\server.log]
:skip_game

if not "%START_ROUTER%"=="1" goto skip_router
if not exist "%BIN_DIR%\chaos_router.exe" (
    echo [ERROR] chaos_router.exe not found - run build_and_test.bat
    exit /b 1
)
echo [INFO] Starting router...
start "chaos_router" cmd /c "%BIN_DIR%\chaos_router.exe --game-port %ROUTER_GAME% --cluster-port %ROUTER_CLUSTER% > %LOG_DIR%\router.log 2>nul"
echo [OK] router started  [log: logs\router.log]
:skip_router

if not "%START_GATEWAY%"=="1" goto skip_gateway
if not exist "%BIN_DIR%\chaos_gateway.exe" (
    echo [ERROR] chaos_gateway.exe not found - run build_and_test.bat
    exit /b 1
)
echo [INFO] Starting gateway...
start "chaos_gateway" cmd /c "%BIN_DIR%\chaos_gateway.exe --port %GATEWAY_TCP% --backend 127.0.0.1:%GAME_PORT% > %LOG_DIR%\gateway.log 2>nul"
echo [OK] gateway started  [log: logs\gateway.log]
:skip_gateway

if not "%START_ADMIN%"=="1" goto skip_admin
cmd /c "where lapis > nul 2>nul"
if not %errorlevel% equ 0 goto no_lapis
echo [INFO] Starting Admin Web (lapis)...
pushd "%PROJECT_DIR%\src_lua\admin"
start "chaos_admin" cmd /c "lapis server"
popd
echo [OK] admin started - http://localhost:%ADMIN_PORT%
goto skip_admin
:no_lapis
echo [SKIP] lapis not found
:skip_admin

echo:
echo  Cluster started. Logs in: %LOG_DIR%
echo  Game:    127.0.0.1:%GAME_PORT%
echo  Gateway: 127.0.0.1:%GATEWAY_TCP%
echo  Admin:   http://localhost:%ADMIN_PORT%
echo  Stop:    scripts\stop_cluster.bat

endlocal
exit /b 0
