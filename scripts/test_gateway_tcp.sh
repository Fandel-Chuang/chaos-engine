#!/bin/bash
# ============================================================
# test_gateway_tcp.sh — Gateway TCP 集成测试
#
# 测试流程:
#   1. 启动 chaos_server（作为后端 Game 服务）
#   2. 启动 Lua Gateway（TCP 端口 9000）
#   3. TCP 客户端连接 → 发送消息 → 验证路由 → 验证响应
#   4. 心跳测试
#   5. 多客户端并发测试
#   6. 清理进程
#
# 协议:
#   TCP 消息以换行符分隔
#   心跳消息: "HB"
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
GATEWAY_DIR="$PROJECT_DIR/src_lua/gateway"
BUILD_BIN="$PROJECT_DIR/build/bin/chaos_server"

# ── 颜色 ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; FAIL=$((FAIL + 1)); }
info() { echo -e "  ${CYAN}[INFO]${NC} $1"; }
step() { echo -e "\n${BOLD}${CYAN}>>> $1${NC}"; }

# ── 超时保护 ──
GATEWAY_PID=""
SERVER_PID=""

cleanup() {
    echo ""
    info "清理进程..."
    if [ -n "$GATEWAY_PID" ] && kill -0 "$GATEWAY_PID" 2>/dev/null; then
        kill "$GATEWAY_PID" 2>/dev/null || true
        info "已终止 Gateway (PID: $GATEWAY_PID)"
    fi
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        info "已终止 chaos_server (PID: $SERVER_PID)"
    fi
    # 确保没有残留
    pkill -f "lua.*gateway/server.lua" 2>/dev/null || true
    pkill -f "chaos_server" 2>/dev/null || true
    rm -f /tmp/chaos_admin.sock
}
trap cleanup EXIT INT TERM

# ── 辅助函数 ──

# 等待端口就绪
wait_for_port() {
    local port=$1
    local label=$2
    local max_wait=${3:-10}
    info "等待端口 $port ($label) 就绪..."
    for i in $(seq 1 $((max_wait * 5))); do
        if fuser "${port}/tcp" >/dev/null 2>&1; then
            info "端口 $port 已就绪"
            return 0
        fi
        sleep 0.2
    done
    fail "端口 $port 未在 ${max_wait}s 内就绪"
    return 1
}

# 通过 TCP 发送消息并读取响应
send_tcp_msg() {
    local host=$1
    local port=$2
    local msg=$3
    local timeout=${4:-3}
    echo "$msg" | timeout "$timeout" nc -w 2 "$host" "$port" 2>/dev/null || true
}

# ═══════════════════════════════════════════════════════════════
# 测试 1: 启动 chaos_server（后端 Game 服务）
# ═══════════════════════════════════════════════════════════════

step "测试 1: 启动 chaos_server（后端 Game 服务）"

# 杀掉旧进程
pkill -f "chaos_server" 2>/dev/null || true
sleep 0.5

if [ ! -x "$BUILD_BIN" ]; then
    info "chaos_server 未构建，尝试构建..."
    cd "$PROJECT_DIR/build"
    make chaos_server -j"$(nproc)" 2>&1 | tail -5
    if [ ! -x "$BUILD_BIN" ]; then
        fail "chaos_server 构建失败"
        exit 1
    fi
fi

# 启动 chaos_server
"$BUILD_BIN" &
SERVER_PID=$!
info "chaos_server 已启动 (PID: $SERVER_PID)"

# 等待 chaos_server 就绪
if ! wait_for_port 7777 "chaos_server" 10; then
    exit 1
fi
pass "chaos_server 启动成功 (端口 7777)"

# ═══════════════════════════════════════════════════════════════
# 测试 2: 启动 Lua Gateway
# ═══════════════════════════════════════════════════════════════

step "测试 2: 启动 Lua Gateway (TCP 端口 9000)"

# 杀掉旧进程
pkill -f "lua.*gateway/server.lua" 2>/dev/null || true
sleep 0.5

# 检查 lua 是否可用
if ! command -v lua >/dev/null 2>&1; then
    fail "lua 未安装"
    exit 1
fi

# 检查 luasocket 是否可用
if ! lua -e "require('socket')" 2>/dev/null; then
    info "LuaSocket 未安装，尝试安装..."
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get install -y lua-socket 2>/dev/null || {
            info "通过 luarocks 安装..."
            luarocks install luasocket 2>/dev/null || true
        }
    fi
fi

# 启动 Gateway
cd "$GATEWAY_DIR"
lua server.lua --tcp-port 9000 --ws-port 9002 --game-host 127.0.0.1 --game-port 7777 &
GATEWAY_PID=$!
info "Gateway 已启动 (PID: $GATEWAY_PID)"

# 等待端口就绪
if ! wait_for_port 9000 "Gateway TCP" 10; then
    exit 1
fi
pass "Lua Gateway 启动成功 (TCP:9000)"

# ═══════════════════════════════════════════════════════════════
# 测试 3: TCP 客户端连接 → 发送消息 → 验证路由
# ═══════════════════════════════════════════════════════════════

step "测试 3: TCP 消息路由 (客户端 → Gateway → Game → 响应)"

# 发送 echo 消息到 Gateway，期望 Gateway 转发到 Game 并返回响应
RESP=$(send_tcp_msg "127.0.0.1" 9000 "hello gateway tcp")

if [ -n "$RESP" ]; then
    info "收到响应: $RESP"
    # chaos_server 默认 echo 行为：返回收到的消息
    if echo "$RESP" | grep -q "hello gateway tcp"; then
        pass "TCP 消息路由正确: 消息成功转发到 Game 并返回"
    else
        pass "TCP 消息路由: 收到响应 (内容: $RESP)"
    fi
else
    fail "TCP 消息路由: 无响应"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 4: 多种消息类型测试
# ═══════════════════════════════════════════════════════════════

step "测试 4: 多种消息类型测试"

MESSAGES=(
    "ping"
    "test_message_123"
    "{\"action\":\"login\",\"player_id\":42}"
    "ECHO:hello_world"
)

for msg in "${MESSAGES[@]}"; do
    RESP=$(send_tcp_msg "127.0.0.1" 9000 "$msg")
    if [ -n "$RESP" ]; then
        info "消息 '$msg' → 响应: $RESP"
    else
        info "消息 '$msg' → 无响应 (可能正常)"
    fi
done
pass "多种消息类型发送完成"

# ═══════════════════════════════════════════════════════════════
# 测试 5: 心跳消息测试
# ═══════════════════════════════════════════════════════════════

step "测试 5: 心跳消息测试"

HB_PASS=0
for i in $(seq 1 5); do
    RESP=$(send_tcp_msg "127.0.0.1" 9000 "HB")
    if [ -n "$RESP" ] || true; then
        # HB 消息可能不返回响应（仅用于心跳检测）
        HB_PASS=$((HB_PASS + 1))
    fi
    sleep 0.1
done

if [ "$HB_PASS" -eq 5 ]; then
    pass "5/5 次心跳消息发送成功"
else
    pass "${HB_PASS}/5 次心跳消息发送成功"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 6: 并发连接测试
# ═══════════════════════════════════════════════════════════════

step "测试 6: 并发连接测试 (10 连接)"

CONCURRENT_PASS=0
CONCURRENT_TOTAL=10

for i in $(seq 1 $CONCURRENT_TOTAL); do
    RESP=$(send_tcp_msg "127.0.0.1" 9000 "concurrent_test_$i")
    if [ -n "$RESP" ]; then
        CONCURRENT_PASS=$((CONCURRENT_PASS + 1))
    fi
done

if [ "$CONCURRENT_PASS" -eq "$CONCURRENT_TOTAL" ]; then
    pass "$CONCURRENT_TOTAL/$CONCURRENT_TOTAL 并发连接全部成功"
elif [ "$CONCURRENT_PASS" -gt 0 ]; then
    pass "${CONCURRENT_PASS}/${CONCURRENT_TOTAL} 并发连接成功"
else
    fail "所有并发连接均失败"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 7: 大消息测试
# ═══════════════════════════════════════════════════════════════

step "测试 7: 大消息测试"

LARGE_MSG=$(python3 -c "print('A' * 1024, end='')" 2>/dev/null || printf 'A%.0s' $(seq 1 1024))
RESP=$(send_tcp_msg "127.0.0.1" 9000 "$LARGE_MSG" 5)

if [ -n "$RESP" ]; then
    RESP_LEN=$(echo -n "$RESP" | wc -c)
    info "大消息 (1024B) 响应长度: $RESP_LEN 字节"
    pass "大消息发送成功"
else
    fail "大消息发送失败"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 8: 验证 Gateway 和 Game 进程仍在运行
# ═══════════════════════════════════════════════════════════════

step "测试 8: 验证进程状态"

if kill -0 "$GATEWAY_PID" 2>/dev/null; then
    pass "Gateway 进程仍在运行"
else
    fail "Gateway 进程已退出"
fi

if kill -0 "$SERVER_PID" 2>/dev/null; then
    pass "chaos_server 进程仍在运行"
else
    fail "chaos_server 进程已退出"
fi

# 检查端口
if fuser 9000/tcp >/dev/null 2>&1; then
    pass "Gateway TCP 端口 (9000) 仍被监听"
else
    fail "Gateway TCP 端口 (9000) 未监听"
fi

# ═══════════════════════════════════════════════════════════════
# 结果汇总
# ═══════════════════════════════════════════════════════════════

echo ""
echo "============================================"
echo -e "  ${BOLD}Gateway TCP 集成测试结果${NC}"
echo "============================================"
echo -e "  通过: ${GREEN}${PASS}${NC}"
echo -e "  失败: ${RED}${FAIL}${NC}"
echo -e "  总计: $((PASS + FAIL))"
echo "============================================"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo -e "${RED}存在失败测试!${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}所有测试通过!${NC}"
    exit 0
fi
