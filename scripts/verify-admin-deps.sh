#!/bin/bash
# ============================================================
# ChaosEngine Web 管理后台 — 依赖验证脚本
# 用法:   bash scripts/verify-admin-deps.sh
# ============================================================
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${GREEN}[✓]${NC} $1"; }
err()  { echo -e "${RED}[✗]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }

PASS=0
FAIL=0
WARN=0

check_cmd() {
    if command -v "$1" &>/dev/null; then
        log "  $1"
        PASS=$((PASS + 1))
    else
        err "  $1 未找到"
        FAIL=$((FAIL + 1))
    fi
}

check_pkg() {
    if dpkg -l "$1" 2>/dev/null | grep -q '^ii'; then
        log "  $1"
        PASS=$((PASS + 1))
    else
        err "  $1 未安装"
        FAIL=$((FAIL + 1))
    fi
}

check_rock() {
    if luarocks list --porcelain 2>/dev/null | grep -q "^$1"; then
        log "  $1"
        PASS=$((PASS + 1))
    else
        err "  $1 未安装"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== ChaosEngine Admin 依赖验证 ==="
echo ""

# ── 命令行工具 ──
echo "--- 命令行工具 ---"
check_cmd luajit
check_cmd luarocks
check_cmd openresty

# ── 系统运行时库 ──
echo ""
echo "--- 系统运行时库 ---"
check_pkg libluajit-5.1-2
check_pkg libluajit-5.1-common
check_pkg libssl3t64
check_pkg libpcre2-8-0
check_pkg zlib1g

# ── 系统开发库 ──
echo ""
echo "--- 系统开发库 (编译时) ---"
check_pkg libluajit-5.1-dev
check_pkg libssl-dev
check_pkg libpcre2-dev
check_pkg zlib1g-dev
check_pkg build-essential

# ── LuaRocks 包 ──
echo ""
echo "--- LuaRocks 包 (共 10 个) ---"
check_rock lapis
check_rock lua-cjson
check_rock luaossl
check_rock luasocket
check_rock lpeg
check_rock etlua
check_rock date
check_rock loadkit
check_rock argparse
check_rock ansicolors

# ── 版本信息 ──
echo ""
echo "--- 版本信息 ---"
echo "  LuaJIT:    $(luajit -v 2>&1)"
echo "  LuaRocks:  $(luarocks --version 2>&1 | head -1)"
echo "  OpenResty: $(openresty -v 2>&1 || echo '未安装')"
echo "  Lapis:     $(luarocks show lapis 2>&1 | grep '^lapis ' | awk '{print $2}')"

# ── 结果 ──
TOTAL=$((PASS + FAIL))
echo ""
echo "============================================"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}  全部 $TOTAL 项通过 ✅${NC}"
else
    echo -e "${RED}  通过: $PASS / $TOTAL, 失败: $FAIL ❌${NC}"
    echo ""
    echo "修复: bash scripts/install-admin-deps.sh"
fi
echo "============================================"
