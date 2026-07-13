@echo off
chcp 65001 > nul
rem scripts\start_dbproxy.bat — ChaosEngine DBProxy 启动脚本 (Windows)
rem
rem 用法:
rem   scripts\start_dbproxy.bat
rem   scripts\start_dbproxy.bat --role backup
rem   scripts\start_dbproxy.bat --role primary --peer-host 192.168.1.10 --peer-port 9101

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "DBPROXY_DIR=%PROJECT_DIR%\src_lua\dbproxy"

set "ROLE=primary"
set "PEER_HOST=127.0.0.1"
set "PEER_PORT=9101"
set "MONGO_URI=mongodb://127.0.0.1:27017"
set "MONGO_DB=chaos_engine"
set "ARCHIVE_DIR=%TEMP%\chaos_dbproxy\archive"
set "LOG_FILE=%TEMP%\chaos_dbproxy.log"

:parse_args
if "%~1"=="" goto validate
if "%~1"=="--role"        ( set "ROLE=%~2"        & shift & shift & goto parse_args )
if "%~1"=="--peer-host"   ( set "PEER_HOST=%~2"   & shift & shift & goto parse_args )
if "%~1"=="--peer-port"   ( set "PEER_PORT=%~2"   & shift & shift & goto parse_args )
if "%~1"=="--mongo-uri"   ( set "MONGO_URI=%~2"   & shift & shift & goto parse_args )
if "%~1"=="--mongo-db"    ( set "MONGO_DB=%~2"    & shift & shift & goto parse_args )
if "%~1"=="--archive-dir" ( set "ARCHIVE_DIR=%~2" & shift & shift & goto parse_args )
if "%~1"=="--log-file"    ( set "LOG_FILE=%~2"    & shift & shift & goto parse_args )
if "%~1"=="--help" goto usage
if "%~1"=="-h" goto usage
echo [ERROR] 未知参数: %~1
echo 使用 --help 查看帮助
exit /b 1

:usage
echo 用法: %~nx0 [选项]
echo.
echo 选项:
echo   --role ROLE           角色: primary 或 backup (默认: primary)
echo   --peer-host HOST      对端 DBProxy 地址 (默认: 127.0.0.1)
echo   --peer-port PORT      对端 DBProxy 端口 (默认: 9101)
echo   --mongo-uri URI       MongoDB 连接 URI
echo   --mongo-db DB         MongoDB 数据库名 (默认: chaos_engine)
echo   --archive-dir DIR     状态镜像存档目录
echo   --log-file FILE       日志文件路径
echo   --help, -h            显示此帮助
exit /b 0

:validate
if not "%ROLE%"=="primary" if not "%ROLE%"=="backup" (
    echo [ERROR] 无效的角色: %ROLE% ^(必须是 primary 或 backup^)
    exit /b 1
)
if not exist "%DBPROXY_DIR%\" (
    echo [ERROR] DBProxy 目录不存在: %DBPROXY_DIR%
    exit /b 1
)
if not exist "%DBPROXY_DIR%\init.lua" (
    echo [ERROR] 找不到 init.lua: %DBPROXY_DIR%\init.lua
    exit /b 1
)

rem 检查 Lua
set "LUA_BIN="
where lua5.4 >nul 2>&1 && set "LUA_BIN=lua5.4" && goto lua_ok
where lua5.3 >nul 2>&1 && set "LUA_BIN=lua5.3" && goto lua_ok
where lua    >nul 2>&1 && set "LUA_BIN=lua"     && goto lua_ok
echo [ERROR] 未找到 Lua 解释器 ^(尝试了 lua5.4, lua5.3, lua^)
exit /b 1
:lua_ok
echo [OK] 使用 Lua: %LUA_BIN%

rem 终止旧进程（按日志文件关联进程名）
echo.
echo ^>^>^> 清理旧进程...
tasklist /fi "imagename eq lua.exe" 2>nul | findstr /i "lua.exe" >nul
if %errorlevel% equ 0 (
    echo [WARN] 发现 lua.exe 进程，请手动确认是否需要终止
)

rem 准备目录
if not exist "%ARCHIVE_DIR%" mkdir "%ARCHIVE_DIR%"

rem 启动 DBProxy
echo.
echo ^>^>^> 启动 DBProxy ^(角色: %ROLE%^)...

echo ============================================
echo   ChaosEngine DBProxy
echo   角色:       %ROLE%
echo   同步端口:   9001
echo   数据库端口: 9003
echo   对端:       %PEER_HOST%:%PEER_PORT%
echo   MongoDB:    %MONGO_URI%/%MONGO_DB%
echo   日志:       %LOG_FILE%
echo ============================================
echo.

cd /d "%PROJECT_DIR%"

start /b "" %LUA_BIN% "%DBPROXY_DIR%\init.lua" ^
    --role "%ROLE%" ^
    --peer-host "%PEER_HOST%" ^
    --peer-port "%PEER_PORT%" ^
    --mongo-uri "%MONGO_URI%" ^
    --mongo-db "%MONGO_DB%" ^
    --archive-dir "%ARCHIVE_DIR%" >> "%LOG_FILE%" 2>&1

echo [OK] DBProxy 已启动
echo.
echo 停止 DBProxy:
echo   taskkill /f /im lua.exe
echo.
echo 查看日志:
echo   type "%LOG_FILE%"
echo.

exit /b 0
