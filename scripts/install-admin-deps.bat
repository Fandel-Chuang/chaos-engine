@echo off
chcp 65001 > nul
rem scripts\install-admin-deps.bat — Web 管理后台依赖安装 (Windows 说明)
rem
rem 原始脚本使用 apt/dpkg/luarocks，适用于 Ubuntu 24.04。
rem Windows 需手动安装以下依赖。

echo ============================================================
echo   ChaosEngine Admin 依赖安装 (Windows)
echo ============================================================
echo.
echo   请手动安装以下组件:
echo.
echo   1. OpenResty (Nginx + LuaJIT)
echo      下载: https://openresty.org/en/download.html
echo      选择 Windows 版本并安装。
echo.
echo   2. LuaRocks
echo      下载: https://luarocks.org/
echo      配置为使用 OpenResty 的 LuaJIT。
echo.
echo   3. Lapis 及依赖包 (安装 LuaRocks 后执行):
echo      luarocks install lapis
echo      luarocks install lua-cjson
echo      luarocks install luaossl
echo      luarocks install luasocket
echo      luarocks install lpeg
echo      luarocks install etlua
echo      luarocks install date
echo      luarocks install loadkit
echo      luarocks install argparse
echo      luarocks install ansicolors
echo.
echo   4. 验证安装:
echo      scripts\verify-admin-deps.bat
echo.
echo   推荐使用 WSL2 (Ubuntu) 运行原始安装脚本:
echo      wsl bash scripts/install-admin-deps.sh
echo.

exit /b 0
