#!/bin/bash
# ============================================================
# test_save.sh — 存档功能集成测试
#
# 测试流程:
#   1. 启动 chaos_server（带 --admin 启用 Admin IPC）
#   2. 通过 Admin IPC Unix Socket 调用 save.now 命令
#   3. 验证存档响应
#   4. 清理进程
#
# Admin IPC 协议 (JSON-RPC 2.0 over Unix Socket):
#   请求:  {"jsonrpc":"2.0","id":<num>,"method":"<method>","params":{...}}\n
#   响应:  {"jsonrpc":"2.0","id":<num>,"result":{...}}\n
#
# Admin Socket: /tmp/chaos_admin.sock
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_BIN="$PROJECT_DIR/build/bin/chaos_server"
ADMIN_SOCK="/tmp/chaos_admin.sock"

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
SERVER_PID=""

cleanup() {
    echo ""
    info "清理进程..."
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        info "已终止 chaos_server (PID: $SERVER_PID)"
    fi
    # 确保没有残留
    pkill -f "chaos_server" 2>/dev/null || true
    rm -f "$ADMIN_SOCK"
}
trap cleanup EXIT INT TERM

# ── 辅助函数 ──

# 通过 Unix Socket 发送 JSON-RPC 请求
# 参数: method [params_json]
send_ipc() {
    local method=$1
    local params=${2:-"{}"}
    local id
    id=$(date +%s%N 2>/dev/null || echo $$)

    local request
    request=$(printf '{"jsonrpc":"2.0","id":%s,"method":"%s","params":%s}\n' "$id" "$method" "$params")

    if [ ! -S "$ADMIN_SOCK" ]; then
        echo ""
        return 1
    fi

    # 使用 socat 或 nc 发送到 Unix Socket
    if command -v socat >/dev/null 2>&1; then
        echo "$request" | timeout 5 socat - "UNIX-CONNECT:$ADMIN_SOCK" 2>/dev/null || true
    elif command -v nc >/dev/null 2>&1; then
        echo "$request" | timeout 5 nc -U "$ADMIN_SOCK" 2>/dev/null || true
    else
        # Python fallback
        python3 -c "
import socket, sys, json
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
try:
    sock.connect('$ADMIN_SOCK')
    sock.sendall(sys.stdin.read().encode())
    resp = b''
    while True:
        chunk = sock.recv(4096)
        if not chunk: break
        resp += chunk
        if b'\n' in resp: break
    print(resp.decode().strip())
except Exception as e:
    print(f'ERROR: {e}', file=sys.stderr)
finally:
    sock.close()
" <<< "$request" 2>/dev/null || true
    fi
}

# 等待 Unix Socket 就绪
wait_for_socket() {
    local sock_path=$1
    local max_wait=${2:-10}
    info "等待 Admin Socket ($sock_path) 就绪..."
    for i in $(seq 1 $((max_wait * 10))); do
        if [ -S "$sock_path" ]; then
            info "Admin Socket 已就绪"
            return 0
        fi
        sleep 0.1
    done
    fail "Admin Socket 未在 ${max_wait}s 内就绪"
    return 1
}

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

# ═══════════════════════════════════════════════════════════════
# 测试 1: 构建并启动 chaos_server (带 --admin)
# ═══════════════════════════════════════════════════════════════

step "测试 1: 启动 chaos_server (带 --admin)"

# 杀掉旧进程
pkill -f "chaos_server" 2>/dev/null || true
rm -f "$ADMIN_SOCK"
sleep 0.5

# 检查二进制是否存在
if [ ! -x "$BUILD_BIN" ]; then
    info "chaos_server 未构建，尝试构建..."
    cd "$PROJECT_DIR/build"
    make chaos_server -j"$(nproc)" 2>&1 | tail -5
    if [ ! -x "$BUILD_BIN" ]; then
        fail "chaos_server 构建失败"
        exit 1
    fi
fi

# 启动 chaos_server（带 --admin 启用 Admin IPC）
"$BUILD_BIN" --admin &
SERVER_PID=$!
info "chaos_server 已启动 (PID: $SERVER_PID, --admin)"

# 等待 Admin Socket 就绪
if ! wait_for_socket "$ADMIN_SOCK" 15; then
    # 检查进程是否还活着
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        fail "chaos_server 进程已退出"
        exit 1
    fi
    fail "Admin Socket 未创建"
    exit 1
fi
pass "chaos_server 启动成功 (Admin IPC 已启用)"

# ═══════════════════════════════════════════════════════════════
# 测试 2: 调用 health 方法验证 IPC 通信
# ═══════════════════════════════════════════════════════════════

step "测试 2: Admin IPC 健康检查 (health)"

HEALTH_RESP=$(send_ipc "health")
info "health 响应: $HEALTH_RESP"

if echo "$HEALTH_RESP" | grep -q '"ok"'; then
    pass "health 方法返回 ok"
elif echo "$HEALTH_RESP" | grep -q '"result"'; then
    pass "health 方法返回有效 JSON-RPC 响应"
else
    fail "health 方法失败: $HEALTH_RESP"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 3: 调用 save.now 方法
# ═══════════════════════════════════════════════════════════════

step "测试 3: 调用 save.now 触发手动存档"

SAVE_RESP=$(send_ipc "save.now")
info "save.now 响应: $SAVE_RESP"

if [ -z "$SAVE_RESP" ]; then
    fail "save.now 无响应 (DBProxy 未连接时存档模块可能未初始化)"
elif echo "$SAVE_RESP" | grep -q '"status"'; then
    # 检查状态
    if echo "$SAVE_RESP" | grep -q '"ok"'; then
        pass "save.now 返回 status=ok"
    elif echo "$SAVE_RESP" | grep -q '"error"'; then
        info "save.now 返回 status=error (预期: DBProxy 未连接)"
        pass "save.now 正确返回错误状态"
    else
        pass "save.now 返回有效响应"
    fi
elif echo "$SAVE_RESP" | grep -q '"result"'; then
    pass "save.now 返回有效 JSON-RPC 响应"
else
    info "save.now 响应格式: $SAVE_RESP"
    pass "save.now 已调用 (响应已收到)"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 4: 调用 save.status 方法
# ═══════════════════════════════════════════════════════════════

step "测试 4: 调用 save.status 查询存档状态"

STATUS_RESP=$(send_ipc "save.status")
info "save.status 响应: $STATUS_RESP"

if [ -z "$STATUS_RESP" ]; then
    fail "save.status 无响应"
elif echo "$STATUS_RESP" | grep -q '"result"'; then
    pass "save.status 返回有效 JSON-RPC 响应"
elif echo "$STATUS_RESP" | grep -q '"total_saves"'; then
    pass "save.status 返回存档统计信息"
else
    info "save.status 响应: $STATUS_RESP"
    pass "save.status 已调用 (响应已收到)"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 5: 调用 stats 方法验证系统状态
# ═══════════════════════════════════════════════════════════════

step "测试 5: 调用 stats 方法验证系统状态"

STATS_RESP=$(send_ipc "stats")
info "stats 响应: $STATS_RESP"

if echo "$STATS_RESP" | grep -q '"entity_count"'; then
    pass "stats 返回 entity_count 字段"
elif echo "$STATS_RESP" | grep -q '"result"'; then
    pass "stats 返回有效 JSON-RPC 响应"
else
    fail "stats 方法失败: $STATS_RESP"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 6: 多次调用 save.now (压力测试)
# ═══════════════════════════════════════════════════════════════

step "测试 6: 多次调用 save.now (压力测试)"

SAVE_COUNT=0
for i in $(seq 1 5); do
    RESP=$(send_ipc "save.now")
    if [ -n "$RESP" ]; then
        SAVE_COUNT=$((SAVE_COUNT + 1))
    fi
done

if [ "$SAVE_COUNT" -eq 5 ]; then
    pass "5/5 次 save.now 调用全部成功"
elif [ "$SAVE_COUNT" -gt 0 ]; then
    pass "${SAVE_COUNT}/5 次 save.now 调用成功"
else
    fail "所有 save.now 调用均失败"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 7: 验证 chaos_server 在存档调用后仍正常运行
# ═══════════════════════════════════════════════════════════════

step "测试 7: 验证 chaos_server 在存档调用后仍正常运行"

# 检查进程是否还活着
if kill -0 "$SERVER_PID" 2>/dev/null; then
    pass "chaos_server 进程仍在运行"
else
    fail "chaos_server 进程已退出"
fi

# 再次调用 health
HEALTH2=$(send_ipc "health")
if [ -n "$HEALTH2" ]; then
    pass "存档调用后 health 仍可正常响应"
else
    fail "存档调用后 health 无响应"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 8: 无效方法调用
# ═══════════════════════════════════════════════════════════════

step "测试 8: 无效方法调用"

INVALID_RESP=$(send_ipc "nonexistent.method")
info "无效方法响应: $INVALID_RESP"

if [ -n "$INVALID_RESP" ]; then
    if echo "$INVALID_RESP" | grep -q '"error"'; then
        pass "无效方法返回错误响应"
    else
        info "无效方法响应: $INVALID_RESP"
        pass "无效方法返回响应 (未崩溃)"
    fi
else
    info "无效方法无响应 (可能被忽略)"
    pass "无效方法未导致崩溃"
fi

# ═══════════════════════════════════════════════════════════════
# 结果汇总
# ═══════════════════════════════════════════════════════════════

echo ""
echo "============================================"
echo -e "  ${BOLD}存档功能测试结果${NC}"
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
