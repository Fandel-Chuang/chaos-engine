@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

echo ============================================================
echo  ChaosEngine Loop Build (build until 0 errors, 0 warnings)
echo ============================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "MAX_ITER=10"
set "ITER=0"
set "LOG=%ROOT%\build_loop.log"

if exist "%LOG%" del /q "%LOG%"

:loop
set /a ITER+=1
echo.
echo ─────────────────────────────────────────────────────────
echo  Iteration %ITER% / %MAX_ITER%
echo ─────────────────────────────────────────────────────────

rem Run build_and_test.bat and capture output
call "%ROOT%\build_and_test.bat" > "%TEMP%\build_out_%ITER%.txt" 2>&1
set "BUILD_EXIT=%errorlevel%"

type "%TEMP%\build_out_%ITER%.txt"
type "%TEMP%\build_out_%ITER%.txt" >> "%LOG%"

rem Count errors and warnings in output
set "ERR_COUNT=0"
set "WARN_COUNT=0"

for /f %%c in ('findstr /i /c:" error " /c:" error:" /c:"error C" /c:"error LNK" "%TEMP%\build_out_%ITER%.txt" 2^>nul ^| find /c /v ""') do set "ERR_COUNT=%%c"
for /f %%c in ('findstr /i /c:" warning " /c:" warning:" /c:"warning C" "%TEMP%\build_out_%ITER%.txt" 2^>nul ^| find /c /v ""') do set "WARN_COUNT=%%c"

echo.
echo  Build exit code : %BUILD_EXIT%
echo  Error lines     : %ERR_COUNT%
echo  Warning lines   : %WARN_COUNT%

if not "%BUILD_EXIT%"=="0" goto not_clean
if not "%ERR_COUNT%"=="0" goto not_clean
if not "%WARN_COUNT%"=="0" goto not_clean

echo.
echo ============================================================
echo  SUCCESS: Build clean with 0 errors and 0 warnings!
echo  Completed in %ITER% iteration(s^).
echo ============================================================
del /q "%TEMP%\build_out_%ITER%.txt" >nul 2>&1
endlocal
exit /b 0

:not_clean
if "%ERR_COUNT%"=="0" (
    echo  [FIX] Warnings or build failure remain. Running again...
) else (
    echo  [FIX] %ERR_COUNT% error line(s^) remain. Running again...
)

del /q "%TEMP%\build_out_%ITER%.txt" >nul 2>&1

if %ITER% geq %MAX_ITER% (
    echo.
    echo ============================================================
    echo  STOPPED: Reached max iterations (%MAX_ITER%).
    echo  Errors / warnings still present. Manual fix required.
    echo  Full log: %LOG%
    echo ============================================================
    endlocal
    exit /b 1
)

rem Brief pause so CMake cache refresh can happen
timeout /t 2 /nobreak > nul

goto loop
