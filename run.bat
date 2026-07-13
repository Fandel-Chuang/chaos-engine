@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
set "BIN=%BUILD%\bin"
set "TARGET=chaos_headless"
set "BUILD_TYPE=Debug"
set "REBUILD=0"
set "RUN_AFTER=1"
set "EXTRA_ARGS="

:parse
if "%~1"=="" goto done_parse
if /i "%~1"=="--release"  ( set "BUILD_TYPE=Release" & shift & goto parse )
if /i "%~1"=="--rebuild"  ( set "REBUILD=1" & shift & goto parse )
if /i "%~1"=="--no-run"   ( set "RUN_AFTER=0" & shift & goto parse )
if /i "%~1"=="--server"   ( set "TARGET=chaos_server" & shift & goto parse )
if /i "%~1"=="--editor"   ( set "TARGET=chaos_editor" & shift & goto parse )
if /i "%~1"=="--client"   ( set "TARGET=chaos_client" & shift & goto parse )
if /i "%~1"=="--headless" ( set "TARGET=chaos_headless" & shift & goto parse )
if /i "%~1"=="--gateway"  ( set "TARGET=chaos_gateway" & shift & goto parse )
if /i "%~1"=="--router"   ( set "TARGET=chaos_router" & shift & goto parse )
if /i "%~1"=="--dbproxy"  ( set "TARGET=chaos_dbproxy" & shift & goto parse )
if /i "%~1"=="--help"     goto usage
set "EXTRA_ARGS=%EXTRA_ARGS% %~1"
shift & goto parse

:usage
echo Usage: run.bat [OPTIONS]
echo.
echo Targets (default: --headless):
echo   --headless   chaos_headless
echo   --server     chaos_server
echo   --editor     chaos_editor
echo   --client     chaos_client
echo   --gateway    chaos_gateway
echo   --router     chaos_router
echo   --dbproxy    chaos_dbproxy
echo.
echo Build options:
echo   --release    Release build (default: Debug)
echo   --rebuild    Delete build dir and reconfigure
echo   --no-run     Build only, do not run
exit /b 0

:done_parse
echo ============================================================
echo  ChaosEngine  target: %TARGET%  ^| %BUILD_TYPE%
echo ============================================================

if "%REBUILD%"=="1" (
    echo [1/3] Clean rebuild...
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

if not exist "%BUILD%\CMakeCache.txt" (
    echo [1/3] CMake configure...
    if not exist "%BUILD%" mkdir "%BUILD%"
    cmake -S "%ROOT:~0,-1%" -B "%BUILD%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCHAOS_BUILD_EDITOR=ON
    if !errorlevel! neq 0 ( echo [ERROR] CMake configure failed & exit /b 1 )
) else (
    echo [1/3] CMake already configured
)

echo [2/3] Building %TARGET%...
cmake --build "%BUILD%" --target %TARGET% --config %BUILD_TYPE% -j %NUMBER_OF_PROCESSORS%
if %errorlevel% neq 0 ( echo [ERROR] Build failed & exit /b 1 )
echo [OK] Build succeeded

set "EXE=%BIN%\%BUILD_TYPE%\%TARGET%.exe"
if not exist "%EXE%" set "EXE=%BIN%\%TARGET%.exe"
if not exist "%EXE%" (
    echo [ERROR] Cannot find %TARGET%.exe under %BIN%
    exit /b 1
)

if "%RUN_AFTER%"=="0" (
    echo [INFO] --no-run: binary is %EXE%
    exit /b 0
)

echo [3/3] Running %TARGET%...
echo ============================================================
echo.
"%EXE%"%EXTRA_ARGS%
set RC=%errorlevel%
echo.
echo ============================================================
if %RC%==0 (
    echo [OK] %TARGET% exited normally
) else (
    echo [WARN] exited with code %RC%
)
exit /b %RC%
