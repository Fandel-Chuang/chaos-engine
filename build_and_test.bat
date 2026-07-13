@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

rem ============================================================
rem  build_and_test.bat �?Build ALL Windows targets
rem  Exits 0 if all targets build with 0 errors.
rem ============================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "BUILD_TYPE=Debug"
set "REBUILD=0"

:parse
if "%~1"=="" goto done_parse
if /i "%~1"=="--release" ( set "BUILD_TYPE=Release" & shift & goto parse )
if /i "%~1"=="--rebuild" ( set "REBUILD=1" & shift & goto parse )
shift & goto parse
:done_parse

echo ============================================================
echo  Build ALL targets ^| %BUILD_TYPE%
echo ============================================================

if "%REBUILD%"=="1" (
    echo [CLEAN] Removing build dir...
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

if not exist "%BUILD%\CMakeCache.txt" (
    echo [CONFIG] Running CMake configure...
    if not exist "%BUILD%" mkdir "%BUILD%"
    cmake -S "%ROOT%" -B "%BUILD%" ^
        -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
        -DCHAOS_BUILD_EDITOR=ON
    if !errorlevel! neq 0 (
        echo [ERROR] CMake configure failed
        exit /b 1
    )
) else (
    echo [CONFIG] Already configured, reconfiguring...
    cmake -S "%ROOT%" -B "%BUILD%" ^
        -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
    if !errorlevel! neq 0 (
        echo [ERROR] CMake reconfigure failed
        exit /b 1
    )
)

echo.
echo [BUILD] Building all targets...
echo ────────────────────────────────────────────────────────────
cmake --build "%BUILD%" --config %BUILD_TYPE% -j %NUMBER_OF_PROCESSORS%
set "BUILD_EXIT=!errorlevel!"
echo ────────────────────────────────────────────────────────────

if "!BUILD_EXIT!"=="0" (
    echo [OK] All targets built successfully
) else (
    echo [FAIL] Build errors detected (exit code !BUILD_EXIT!)
)

exit /b !BUILD_EXIT!
