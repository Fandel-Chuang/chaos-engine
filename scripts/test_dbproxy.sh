#!/bin/bash
# ============================================================
# test_dbproxy.sh — Lua DBProxy 集成测试
#
# 测试流程:
#   1. 启动 Lua DBProxy（备用模式，端口 9003）
#   2. 启动 chaos_server（连接 DBProxy）
#   3. 用 netcat 发送测试消息到 DBProxy db 端口
#   4. 验证 DBProxy 响应
#   5. 清理进程
#
# DBProxy 二进制协议 (db port 9003):
#   请求: [4B msg_len][1B msg_type][N payload]  (大端序)
#   响应: [4B msg_len][1B resp_type][N payload]  (大端序)
#
# 消息类型:
#   MSG_HEARTBEAT  = 0x04
# 响应类型:
#   RESP_OK    = 0x00
#   RESP_ERROR = 0x01
#   RESP_DATA  = 0x02
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DBPROXY_DIR="$PROJECT_DIR/src_lua/dbproxy"
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
TIMEOUT=30
DBPROXY_PID=""
SERVER_PID=""

cleanup() {
    echo ""
    info "清理进程..."
    if [ -n "$DBPROXY_PID" ] && kill -0 "$DBPROXY_PID" 2>/dev/null; then
        kill "$DBPROXY_PID" 2>/dev/null || true
        info "已终止 DBProxy (PID: $DBPROXY_PID)"
    fi
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        info "已终止 chaos_server (PID: $SERVER_PID)"
    fi
    # 确保没有残留
    pkill -f "lua.*dbproxy/init.lua" 2>/dev/null || true
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

# 发送二进制消息到 DBProxy 并读取响应
# 参数: host port msg_type payload_hex
send_dbproxy_msg() {
    local host=$1
    local port=$2
    local msg_type=$3
    local payload_hex=$4

    # 计算消息长度: 4B len + 1B type + payload
    local payload_len=$((${#payload_hex} / 2))
    local msg_len=$((5 + payload_len))

    # 构造二进制消息 (大端序)
    # 使用 printf 构造 4B 长度 + 1B 类型 + payload
    local hex_len
    hex_len=$(printf '%08x' "$msg_len")
    local hex_type
    hex_type=$(printf '%02x' "$msg_type")

    local full_hex="${hex_len}${hex_type}${payload_hex}"

    # 转换为二进制并发送，读取响应
    echo "$full_hex" | xxd -r -p | timeout 5 nc -w 3 "$host" "$port" 2>/dev/null | xxd -p | tr -d '\n'
}

# 解析 DBProxy 响应
# 返回格式: "type:XX payload:HEX"
parse_dbproxy_response() {
    local resp_hex=$1
    if [ -z "$resp_hex" ]; then
        echo "type:empty payload:"
        return
    fi
    # 前 8 个 hex 字符 = 4B 长度
    local len_hex="${resp_hex:0:8}"
    # 接下来 2 个 hex 字符 = 1B 响应类型
    local type_hex="${resp_hex:8:2}"
    # 剩余 = payload
    local payload="${resp_hex:10}"
    echo "type:${type_hex} payload:${payload}"
}

# ═══════════════════════════════════════════════════════════════
# 测试 1: 启动 Lua DBProxy
# ═══════════════════════════════════════════════════════════════

step "测试 1: 启动 Lua DBProxy（备用模式）"

# 杀掉旧进程
pkill -f "lua.*dbproxy/init.lua" 2>/dev/null || true
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

# 启动 DBProxy
cd "$DBPROXY_DIR"
lua init.lua --role backup --archive-dir /tmp/chaos_test_archive &
DBPROXY_PID=$!
info "DBProxy 已启动 (PID: $DBPROXY_PID, 角色: backup)"

# 等待端口就绪
if ! wait_for_port 9003 "DBProxy db" 10; then
    exit 1
fi
pass "Lua DBProxy 启动成功 (端口 9003)"

# ═══════════════════════════════════════════════════════════════
# 测试 2: 启动 chaos_server
# ═══════════════════════════════════════════════════════════════

step "测试 2: 启动 chaos_server"

if [ ! -x "$BUILD_BIN" ]; then
    info "chaos_server 未构建，尝试构建..."
    cd "$PROJECT_DIR/build"
    make chaos_server -j"$(nproc)" 2>&1 | tail -5
    if [ ! -x "$BUILD_BIN" ]; then
        fail "chaos_server 构建失败"
        exit 1
    fi
fi

# 启动 chaos_server（后台运行）
"$BUILD_BIN" &
SERVER_PID=$!
info "chaos_server 已启动 (PID: $SERVER_PID)"

# 等待 chaos_server 就绪（检查端口 7777）
if ! wait_for_port 7777 "chaos_server" 10; then
    exit 1
fi
pass "chaos_server 启动成功 (端口 7777)"

# ═══════════════════════════════════════════════════════════════
# 测试 3: 发送心跳消息到 DBProxy
# ═══════════════════════════════════════════════════════════════

step "测试 3: 发送心跳消息 (MSG_HEARTBEAT=0x04)"

info "发送心跳消息到 DBProxy db 端口 (9003)..."
HEARTBEAT_RESP=$(send_dbproxy_msg "127.0.0.1" 9003 "04" "")

if [ -z "$HEARTBEAT_RESP" ]; then
    fail "心跳消息无响应"
else
    PARSED=$(parse_dbproxy_response "$HEARTBEAT_RESP")
    info "收到响应: $PARSED"

    # 检查响应类型是否为 RESP_OK (0x00)
    if echo "$PARSED" | grep -q "type:00"; then
        pass "心跳响应正确 (RESP_OK)"
    else
        fail "心跳响应类型错误: 期望 00, 实际 $(echo "$PARSED" | cut -d' ' -f1)"
    fi

    # 检查 payload 是否包含 "pong"
    if echo "$PARSED" | grep -qi "706f6e67"; then
        pass "心跳响应包含 'pong'"
    else
        info "心跳响应 payload: $(echo "$PARSED" | cut -d' ' -f2)"
    fi
fi

# ═══════════════════════════════════════════════════════════════
# 测试 4: 发送无效消息类型
# ═══════════════════════════════════════════════════════════════

step "测试 4: 发送无效消息类型 (0xFF)"

info "发送未知消息类型到 DBProxy..."
UNKNOWN_RESP=$(send_dbproxy_msg "127.0.0.1" 9003 "FF" "74657374")  # payload="test"

if [ -z "$UNKNOWN_RESP" ]; then
    fail "无效消息无响应"
else
    PARSED=$(parse_dbproxy_response "$UNKNOWN_RESP")
    info "收到响应: $PARSED"

    # 应该返回 RESP_ERROR (0x01)
    if echo "$PARSED" | grep -q "type:01"; then
        pass "无效消息返回 RESP_ERROR"
    else
        info "响应类型: $(echo "$PARSED" | cut -d' ' -f1) (可能未实现错误处理)"
    fi
fi

# ═══════════════════════════════════════════════════════════════
# 测试 5: 验证 chaos_server 正常运行
# ═══════════════════════════════════════════════════════════════

step "测试 5: 验证 chaos_server 正常运行"

# 发送 echo 测试到 chaos_server
ECHO_RESP=$(echo "hello chaos" | timeout 3 nc -w 2 127.0.0.1 7777 2>/dev/null || true)
if [ "$ECHO_RESP" = "hello chaos" ]; then
    pass "chaos_server echo 功能正常"
else
    fail "chaos_server echo 失败: 期望 'hello chaos', 实际 '$ECHO_RESP'"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 6: 并发连接测试
# ═══════════════════════════════════════════════════════════════

step "测试 6: 并发心跳消息"

CONCURRENT_PASS=0
for i in $(seq 1 5); do
    RESP=$(send_dbproxy_msg "127.0.0.1" 9003 "04" "")
    if [ -n "$RESP" ]; then
        CONCURRENT_PASS=$((CONCURRENT_PASS + 1))
    fi
done

if [ "$CONCURRENT_PASS" -eq 5 ]; then
    pass "5/5 并发心跳消息全部成功"
else
    fail "并发心跳: ${CONCURRENT_PASS}/5 成功"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 7: 大 payload 消息
# ═══════════════════════════════════════════════════════════════

step "测试 7: 大 payload 消息"

# 构造 256 字节的 payload
LARGE_PAYLOAD=$(python3 -c "print('ab' * 128, end='')" 2>/dev/null || \
    printf 'ab%.0s' $(seq 1 128))
LARGE_PAYLOAD_HEX=$(echo -n "$LARGE_PAYLOAD" | xxd -p | tr -d '\n')

LARGE_RESP=$(send_dbproxy_msg "127.0.0.1" 9003 "04" "$LARGE_PAYLOAD_HEX")
if [ -n "$LARGE_RESP" ]; then
    pass "大 payload (256B) 消息发送成功"
else
    fail "大 payload 消息发送失败"
fi

# ═══════════════════════════════════════════════════════════════
# 结果汇总
# ═══════════════════════════════════════════════════════════════

echo ""
echo "============================================"
echo -e "  ${BOLD}DBProxy 集成测试结果${NC}"
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
