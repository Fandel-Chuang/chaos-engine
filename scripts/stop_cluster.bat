@echo off
chcp 65001 > nul
rem scripts\stop_cluster.bat — ChaosEngine 一键停止全集群 (Windows)

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."

echo ============================================
echo  ChaosEngine 停止集群
echo ============================================

set "STOPPED=0"

rem 按名称终止各服务进程
for %%p in (chaos_server chaos_router chaos_gateway chaos_dbproxy chaos_headless) do (
    tasklist /fi "imagename eq %%p.exe" 2>nul | findstr /i "%%p.exe" >nul
    if !errorlevel! equ 0 (
        echo [STOP] 终止 %%p...
        taskkill /f /im %%p.exe >nul 2>&1
        set /a STOPPED+=1
    )
)

rem 终止 lapis/nginx admin 进程
tasklist /fi "imagename eq nginx.exe" 2>nul | findstr /i "nginx.exe" >nul
if %errorlevel% equ 0 (
    echo [STOP] 终止 nginx...
    taskkill /f /im nginx.exe >nul 2>&1
    set /a STOPPED+=1
)

rem 清理 PID 文件
set "PID_DIR=%PROJECT_DIR%\.pids"
if exist "%PID_DIR%\" (
    del /q "%PID_DIR%\*.pid" >nul 2>&1
)

echo.
echo [OK] 已停止 %STOPPED% 个服务
echo ============================================

exit /b 0
