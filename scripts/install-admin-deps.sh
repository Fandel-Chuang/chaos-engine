#!/bin/bash
# ============================================================
# ChaosEngine Web 管理后台 — 一键安装全部依赖
# 适用于: Ubuntu 24.04 (Noble Numbat) amd64
# 用法:   bash scripts/install-admin-deps.sh
# ============================================================
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

log()  { echo -e "${GREEN}[✓]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
err()  { echo -e "${RED}[✗]${NC} $1"; }
step() { echo -e "\n${CYAN}>>> $1${NC}"; }

# ──────────────────────────────────────────────
# 0. 检查权限
# ──────────────────────────────────────────────
if [ "$(id -u)" -eq 0 ]; then
    warn "检测到 root 用户，部分 luarocks 操作可能不需要 sudo"
fi

# ──────────────────────────────────────────────
# 1. 系统基础包
# ──────────────────────────────────────────────
step "1/6 安装系统基础包..."
sudo apt update -qq
sudo apt install -y \
    build-essential \
    curl \
    gnupg2 \
    ca-certificates \
    lsb-release
log "系统基础包完成"

# ──────────────────────────────────────────────
# 2. OpenResty 运行时（推荐生产方案）
# ──────────────────────────────────────────────
step "2/6 安装 OpenResty..."

# 检查是否已安装
if command -v openresty &>/dev/null; then
    log "OpenResty 已安装: $(openresty -v 2>&1)"
else
    # 导入 GPG 公钥
    curl -fsSL https://openresty.org/package/pubkey.gpg | \
        sudo gpg --dearmor -o /usr/share/keyrings/openresty.gpg 2>/dev/null

    # 添加 apt 源
    echo "deb [signed-by=/usr/share/keyrings/openresty.gpg] http://openresty.org/package/ubuntu $(lsb_release -sc) main" | \
        sudo tee /etc/apt/sources.list.d/openresty.list > /dev/null

    # 安装
    sudo apt update -qq
    sudo apt install -y openresty
    log "OpenResty 安装完成: $(openresty -v 2>&1)"
fi

# ──────────────────────────────────────────────
# 3. LuaJIT + 开发库
# ──────────────────────────────────────────────
step "3/6 安装 LuaJIT 及开发库..."

sudo apt install -y \
    luajit \
    libluajit-5.1-2 \
    libluajit-5.1-common \
    libluajit-5.1-dev \
    libssl-dev \
    libpcre2-dev \
    zlib1g-dev \
    libssl3t64 \
    libpcre2-8-0 \
    zlib1g

log "LuaJIT: $(luajit -v 2>&1)"

# ──────────────────────────────────────────────
# 4. LuaRocks
# ──────────────────────────────────────────────
step "4/6 安装 LuaRocks..."

if command -v luarocks &>/dev/null; then
    log "LuaRocks 已安装: $(luarocks --version 2>&1 | head -1)"
else
    sudo apt install -y luarocks
    log "LuaRocks 安装完成"
fi

# 配置 LuaRocks 使用 LuaJIT
sudo luarocks config lua_version 5.1
sudo luarocks config lua_interpreter luajit
log "LuaRocks 已配置为使用 LuaJIT"

# ──────────────────────────────────────────────
# 5. Lapis + 全部 Lua 依赖
# ──────────────────────────────────────────────
step "5/6 安装 Lapis 及 Lua 依赖包..."

ROCKS=(
    lapis
    lua-cjson
    luaossl
    luasocket
    lpeg
    etlua
    date
    loadkit
    argparse
    ansicolors
)

for rock in "${ROCKS[@]}"; do
    if luarocks list --porcelain 2>/dev/null | grep -q "^$rock"; then
        log "  $rock (已安装)"
    else
        echo "  安装 $rock..."
        sudo luarocks install "$rock" 2>&1 | tail -1
    fi
done

log "全部 LuaRocks 包就绪"

# ──────────────────────────────────────────────
# 6. 验证
# ──────────────────────────────────────────────
step "6/6 验证安装..."

PASS=0
FAIL=0

check_cmd() {
    if command -v "$1" &>/dev/null; then
        log "  $1"
        PASS=$((PASS + 1))
    else
        err "  $1 未找到"
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

echo ""
echo "--- 命令行工具 ---"
check_cmd luajit
check_cmd luarocks
check_cmd openresty

echo ""
echo "--- LuaRocks 包 ---"
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

echo ""
echo "--- 版本摘要 ---"
echo "  LuaJIT:    $(luajit -v 2>&1)"
echo "  LuaRocks:  $(luarocks --version 2>&1 | head -1)"
echo "  OpenResty: $(openresty -v 2>&1 || echo '未安装')"
echo "  Lapis:     $(luarocks show lapis 2>&1 | grep '^lapis ' | awk '{print $2}')"

echo ""
echo "============================================"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}  全部 $PASS 项依赖就绪 ✅${NC}"
else
    echo -e "${RED}  通过: $PASS, 失败: $FAIL ❌${NC}"
fi
echo "============================================"
echo ""
echo "启动管理后台:"
echo "  cd src_lua/admin"
echo "  lapis server production"
echo ""
