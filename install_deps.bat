@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

echo ============================================================
echo  ChaosEngine Windows Dependency Installer
echo ============================================================
echo.

set "ROOT=%~dp0"
set "TP=%ROOT%third_party"
set "LUA_TP=%TP%\lua"
set "ERRORS=0"

rem ── 1. Check prerequisites ──
echo [1/5] Checking prerequisites...

cmake --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [WARN] CMake not found. Installing via winget...
    winget install Kitware.CMake --silent
    if !errorlevel! neq 0 ( echo [ERROR] CMake install failed & set "ERRORS=1" )
) else (
    for /f "tokens=3" %%v in ('cmake --version 2^>nul ^| findstr "version"') do echo [OK] CMake %%v
)

where python >nul 2>&1
if %errorlevel% neq 0 (
    echo [WARN] Python not found. Installing via winget...
    winget install Python.Python.3 --silent
) else (
    for /f "tokens=2" %%v in ('python --version 2^>nul') do echo [OK] Python %%v
)

echo.

rem ── 2. Lua 5.4 library for MSVC ──
echo [2/5] Setting up Lua 5.4 (MSVC static library)...

if exist "%LUA_TP%\lib\lua54.lib" (
    echo [OK] Lua 5.4 lib already present
    goto lua_done
)

if not exist "%LUA_TP%\lib" mkdir "%LUA_TP%\lib"

rem Try to find system Lua first
set "LUA_FOUND=0"
for %%p in (
    "C:\Lua\lib\lua54.lib"
    "C:\Program Files\Lua\lib\lua54.lib"
    "C:\tools\lua\lib\lua54.lib"
) do (
    if exist %%p (
        copy /y %%p "%LUA_TP%\lib\lua54.lib" >nul
        echo [OK] Copied Lua lib from %%p
        set "LUA_FOUND=1"
    )
)

if "%LUA_FOUND%"=="1" goto lua_done

rem Download Lua 5.4 binaries for Windows
echo [INFO] Downloading Lua 5.4 Windows binaries...
set "LUA_ZIP=%TEMP%\lua54_win.zip"
set "LUA_URL=https://sourceforge.net/projects/luabinaries/files/5.4.7/Windows%%20Libraries%%20Static/lua-5.4.7_Win64_vc17_lib.zip/download"

curl -L --retry 3 --retry-delay 2 -o "%LUA_ZIP%" "%LUA_URL%" 2>nul
if %errorlevel% neq 0 (
    echo [WARN] Download failed. Trying alternative...
    rem Alternative: build from source using mingw or use pre-built
    set "LUA_URL2=https://downloads.sourceforge.net/project/luabinaries/5.4.7/Windows%%20Libraries%%20Static/lua-5.4.7_Win64_vc17_lib.zip"
    curl -L --retry 3 -o "%LUA_ZIP%" "%LUA_URL2%" 2>nul
)

if exist "%LUA_ZIP%" (
    powershell -NoProfile -Command "Expand-Archive -Path '%LUA_ZIP%' -DestinationPath '%TEMP%\lua54_extracted' -Force"
    xcopy /y /s "%TEMP%\lua54_extracted\*.lib" "%LUA_TP%\lib\" >nul 2>&1
    xcopy /y /s "%TEMP%\lua54_extracted\*.h" "%LUA_TP%\include\" >nul 2>&1
    del /q "%LUA_ZIP%" >nul 2>&1
    rmdir /s /q "%TEMP%\lua54_extracted" >nul 2>&1
    if exist "%LUA_TP%\lib\lua54.lib" (
        echo [OK] Lua 5.4 library installed
    ) else (
        echo [WARN] lua54.lib not found in download. Trying manual build...
        goto lua_build
    )
) else (
    :lua_build
    echo [INFO] Building Lua from source...
    set "LUA_SRC=%TEMP%\lua-5.4.7"
    set "LUA_TAR=%TEMP%\lua-5.4.7.tar.gz"
    curl -L -o "%LUA_TAR%" "https://www.lua.org/ftp/lua-5.4.7.tar.gz" 2>nul
    if exist "%LUA_TAR%" (
        powershell -NoProfile -Command "cd $env:TEMP; tar -xzf lua-5.4.7.tar.gz"
        if exist "%LUA_SRC%\src" (
            pushd "%LUA_SRC%\src"
            cl /O2 /W3 /c /D_CRT_SECURE_NO_WARNINGS *.c /I. >nul 2>&1
            lib /OUT:lua54.lib *.obj >nul 2>&1
            if exist "lua54.lib" (
                copy /y lua54.lib "%LUA_TP%\lib\lua54.lib" >nul
                echo [OK] Lua 5.4 built from source
            ) else (
                echo [ERROR] Lua build failed
                set "ERRORS=1"
            )
            popd
        )
        del /q "%LUA_TAR%" >nul 2>&1
        rmdir /s /q "%LUA_SRC%" >nul 2>&1
    ) else (
        echo [ERROR] Cannot download Lua source. Check internet connection.
        set "ERRORS=1"
    )
)

:lua_done

rem ── 3. Vulkan SDK ──
echo.
echo [3/5] Checking Vulkan SDK...

if defined VULKAN_SDK (
    echo [OK] Vulkan SDK: %VULKAN_SDK%
) else (
    echo [WARN] Vulkan SDK not found (needed for chaos_client/chaos_editor)
    echo        Download from: https://vulkan.lunarg.com/sdk/home
    echo        chaos_headless/server/gateway/router/dbproxy do NOT need Vulkan
)

rem ── 4. Git (for CI) ──
echo.
echo [4/5] Checking Git...
git --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [WARN] Git not found. Install from https://git-scm.com
) else (
    for /f "tokens=3" %%v in ('git --version 2^>nul') do echo [OK] Git %%v
)

rem ── 5. Summary ──
echo.
echo [5/5] Verifying third_party layout...
if exist "%LUA_TP%\include\lua.h"   ( echo [OK] lua.h present  ) else ( echo [MISS] lua.h missing & set "ERRORS=1" )
if exist "%LUA_TP%\lib\lua54.lib"   ( echo [OK] lua54.lib present ) else if exist "%LUA_TP%\lib\lua5.4.lib" ( echo [OK] lua5.4.lib present ) else ( echo [MISS] lua54.lib missing & set "ERRORS=1" )

echo.
echo ============================================================
if "%ERRORS%"=="0" (
    echo  All dependencies installed successfully!
    echo  Run: run.bat --rebuild
) else (
    echo  Some dependencies missing. See warnings above.
    echo  Manual steps may be required.
)
echo ============================================================

endlocal
exit /b %ERRORS%
