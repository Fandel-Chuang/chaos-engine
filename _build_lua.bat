@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set LUA_SRC=%TEMP%\lua-5.4.7\src
set LUA_OUT=d:\workspace\chaos-engine\third_party\lua

cd /d "%LUA_SRC%"
echo [1/3] Compiling Lua 5.4.7 sources...
cl /O2 /W0 /c /D_CRT_SECURE_NO_WARNINGS ^
  lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ^
  ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c ^
  lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c ^
  lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ^
  ltm.c lundump.c lutf8lib.c lvm.c lzio.c
if errorlevel 1 ( echo [ERROR] Compilation failed & exit /b 1 )

echo [2/3] Creating static library lua54.lib...
if not exist "%LUA_OUT%\lib" mkdir "%LUA_OUT%\lib"
lib /OUT:"%LUA_OUT%\lib\lua54.lib" *.obj
if errorlevel 1 ( echo [ERROR] lib failed & exit /b 1 )

echo [3/3] Copying headers...
if not exist "%LUA_OUT%\include" mkdir "%LUA_OUT%\include"
copy /y *.h "%LUA_OUT%\include\" >nul
echo [OK] Lua 5.4.7 installed to %LUA_OUT%
dir "%LUA_OUT%\lib"
exit /b 0
