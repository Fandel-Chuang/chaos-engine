@echo off
chcp 65001 > nul
rem scripts\verify-admin-deps.bat — Web 管理后台依赖验证 (Windows)

setlocal enabledelayedexpansion

set "PASS=0"
set "FAIL=0"

echo === ChaosEngine Admin 依赖验证 ^(Windows^) ===
echo.

rem ── 命令行工具 ──
echo --- 命令行工具 ---

where luajit >nul 2>&1
if %errorlevel% equ 0 (
    for /f "delims=" %%v in ('luajit -v 2^>^&1') do set "LJ_VER=%%v"
    echo   [OK] luajit - !LJ_VER!
    set /a PASS+=1
) else (
    echo   [FAIL] luajit 未找到
    set /a FAIL+=1
)

where luarocks >nul 2>&1
if %errorlevel% equ 0 (
    for /f "delims=" %%v in ('luarocks --version 2^>^&1') do ( set "LR_VER=%%v" & goto lr_done )
    :lr_done
    echo   [OK] luarocks - !LR_VER!
    set /a PASS+=1
) else (
    echo   [FAIL] luarocks 未找到
    set /a FAIL+=1
)

where openresty >nul 2>&1
if %errorlevel% equ 0 (
    echo   [OK] openresty
    set /a PASS+=1
) else (
    rem 检查常见安装路径
    if exist "C:\openresty\nginx.exe" (
        echo   [OK] openresty ^(C:\openresty\nginx.exe^)
        set /a PASS+=1
    ) else (
        echo   [FAIL] openresty 未找到
        set /a FAIL+=1
    )
)

rem ── LuaRocks 包 ──
echo.
echo --- LuaRocks 包 ---

where luarocks >nul 2>&1
if %errorlevel% neq 0 (
    echo   [SKIP] luarocks 未安装，跳过包检查
    goto versions
)

for %%r in (lapis lua-cjson luaossl luasocket lpeg etlua date loadkit argparse ansicolors) do (
    luarocks list --porcelain 2>nul | findstr /i "^%%r" >nul 2>&1
    if !errorlevel! equ 0 (
        echo   [OK] %%r
        set /a PASS+=1
    ) else (
        echo   [FAIL] %%r 未安装
        set /a FAIL+=1
    )
)

:versions
rem ── 版本信息 ──
echo.
echo --- 版本信息 ---
where luajit >nul 2>&1 && luajit -v 2>&1 | findstr /v "^$"
where luarocks >nul 2>&1 && luarocks --version 2>&1 | findstr /v "^$"

rem ── 结果 ──
set /a TOTAL=PASS+FAIL
echo.
echo ============================================
if %FAIL% equ 0 (
    echo   全部 %TOTAL% 项通过
) else (
    echo   通过: %PASS% / %TOTAL%, 失败: %FAIL%
    echo.
    echo   修复: scripts\install-admin-deps.bat
)
echo ============================================

if %FAIL% gtr 0 exit /b 1
exit /b 0
