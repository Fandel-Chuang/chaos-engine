@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "BUILD_DIR=%PROJECT_DIR%\build"
set "BIN_DBG=%BUILD_DIR%\bin\Debug"
set "BIN_REL=%BUILD_DIR%\bin\Release"
set "BIN=%BUILD_DIR%\bin"

set "GATEWAY_HOST=127.0.0.1"
set "GATEWAY_TCP_PORT=9000"
set "GATEWAY_WS_PORT=9002"
set "MODE=headless"
set "CONNS=100"

:parse_args
if "%~1"=="" goto done_args
if "%~1"=="--tcp"      ( set "MODE=tcp"      & shift & goto parse_args )
if "%~1"=="--ws"       ( set "MODE=ws"       & shift & goto parse_args )
if "%~1"=="--headless" ( set "MODE=headless" & shift & goto parse_args )
if "%~1"=="--vulkan"   ( set "MODE=vulkan"   & shift & goto parse_args )
if "%~1"=="--stress"   ( set "MODE=stress"   & set "CONNS=%~2" & shift & shift & goto parse_args )
if "%~1"=="--host"     ( set "GATEWAY_HOST=%~2" & shift & shift & goto parse_args )
if "%~1"=="--port"     ( set "GATEWAY_TCP_PORT=%~2" & shift & shift & goto parse_args )
echo [WARN] Unknown arg: %~1 & shift & goto parse_args

:done_args

echo ============================================
echo  ChaosEngine Client  [mode: %MODE%]
echo ============================================

rem Find exe helper �?checks Debug, Release, root bin in order
call :find_exe "%1" result
goto _find_done

:find_exe
rem Usage: call :find_exe <exename> <result_var>
rem Not using, inline below instead
exit /b 0

:_find_done

if "%MODE%"=="headless" (
    set "_EXE="
    for %%D in ("%BIN_DBG%" "%BIN_REL%" "%BIN%") do (
        if "!_EXE!"=="" if exist "%%~D\chaos_headless.exe" set "_EXE=%%~D\chaos_headless.exe"
    )
    if "!_EXE!"=="" (
        echo [ERROR] chaos_headless.exe not found. Run: build_and_test.bat
        exit /b 1
    )
    echo [INFO] Starting chaos_headless...
    "!_EXE!"
    goto end
)

if "%MODE%"=="vulkan" (
    set "_EXE="
    for %%D in ("%BIN_DBG%" "%BIN_REL%" "%BIN%") do (
        if "!_EXE!"=="" if exist "%%~D\chaos_client.exe" set "_EXE=%%~D\chaos_client.exe"
    )
    if "!_EXE!"=="" (
        echo [ERROR] chaos_client.exe not found. Install Vulkan SDK then run: build_and_test.bat
        exit /b 1
    )
    echo [INFO] Starting Vulkan client...
    "!_EXE!"
    goto end
)

if "%MODE%"=="tcp" (
    echo [INFO] TCP net_client is Linux-only. Use --headless on Windows.
    goto end
)

if "%MODE%"=="ws" (
    echo [INFO] WebSocket test -^> ws://%GATEWAY_HOST%:%GATEWAY_WS_PORT%
    curl -s -o nul -w "HTTP %%{http_code}" ^
         -H "Upgrade: websocket" -H "Connection: Upgrade" ^
         -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" ^
         -H "Sec-WebSocket-Version: 13" ^
         "http://%GATEWAY_HOST%:%GATEWAY_WS_PORT%" 2>nul
    echo.
    goto end
)

if "%MODE%"=="stress" (
    set "_EXE="
    for %%D in ("%BIN_DBG%" "%BIN_REL%" "%BIN%") do (
        if "!_EXE!"=="" if exist "%%~D\bench_client.exe" set "_EXE=%%~D\bench_client.exe"
    )
    if "!_EXE!"=="" ( echo [ERROR] bench_client.exe not found & exit /b 1 )
    echo [INFO] Stress: %CONNS% conns to %GATEWAY_HOST%:%GATEWAY_TCP_PORT%
    "!_EXE!" -p %GATEWAY_TCP_PORT% -c %CONNS% -d 10
    goto end
)

:end
echo.
echo [OK] Done.
endlocal
exit /b 0
