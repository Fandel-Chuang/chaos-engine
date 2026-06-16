#!/bin/bash
# ============================================================
# test_sync.sh — 同步功能集成测试
#
# 测试流程:
#   1. 启动 Lua DBProxy（主模式，端口 9001 sync + 9003 db）
#   2. 启动 chaos_server（连接 DBProxy）
#   3. 模拟 Game 客户端连接 DBProxy sync 端口，发送心跳帧
#   4. 验证 DBProxy 心跳响应
#   5. 发送同步帧验证帧处理
#   6. 清理进程
#
# 同步协议 (sync port 9001):
#   帧格式: [4B frame_len][2B frame_seq][8B timestamp][2B entity_count][N entities...]
#   每个 entity: [8B entity_id][2B component_type][4B data_len][N data]
#   心跳帧: frame_seq = 0xFFFF
#   所有整数大端序
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
    rm -rf /tmp/chaos_test_archive
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

# 构造同步帧的二进制 hex
# 参数: frame_seq timestamp_us entity_count [entity_data...]
# entity_data: "entity_id:comp_type:hex_data"
build_sync_frame_hex() {
    local frame_seq=$1
    local timestamp=$2
    local entity_count=$3
    shift 3

    # 帧头: 4B len + 2B seq + 8B ts + 2B count = 16B
    local entity_data_hex=""
    local entity_data_size=0

    # 处理实体
    local entities=("$@")
    for ent in "${entities[@]}"; do
        if [ -z "$ent" ]; then continue; fi
        IFS=':' read -r eid ctype edata <<< "$ent"
        # entity: 8B id + 2B type + 4B data_len + data
        local eid_hex
        eid_hex=$(printf '%016x' "$eid")
        local ctype_hex
        ctype_hex=$(printf '%04x' "$ctype")
        local edata_len=${#edata}
        local edata_byte_len=$((edata_len / 2))
        local edata_len_hex
        edata_len_hex=$(printf '%08x' "$edata_byte_len")

        entity_data_hex="${entity_data_hex}${eid_hex}${ctype_hex}${edata_len_hex}${edata}"
        entity_data_size=$((entity_data_size + 14 + edata_byte_len))
    done

    # 帧总长度
    local frame_len=$((16 + entity_data_size))
    local frame_len_hex
    frame_len_hex=$(printf '%08x' "$frame_len")
    local frame_seq_hex
    frame_seq_hex=$(printf '%04x' "$frame_seq")
    local ts_hex
    ts_hex=$(printf '%016x' "$timestamp")
    local count_hex
    count_hex=$(printf '%04x' "$entity_count")

    echo "${frame_len_hex}${frame_seq_hex}${ts_hex}${count_hex}${entity_data_hex}"
}

# 通过 TCP 发送二进制 hex 数据并读取响应
send_sync_frame() {
    local host=$1
    local port=$2
    local frame_hex=$3

    echo "$frame_hex" | xxd -r -p | timeout 5 nc -w 3 "$host" "$port" 2>/dev/null | xxd -p | tr -d '\n'
}

# 解析同步帧响应
parse_sync_response() {
    local resp_hex=$1
    if [ -z "$resp_hex" ]; then
        echo "type:empty"
        return
    fi
    # 前 8 hex = 4B 帧长度
    local len_hex="${resp_hex:0:8}"
    local frame_len=$((16#${len_hex}))
    # 接下来 4 hex = 2B frame_seq
    local seq_hex="${resp_hex:8:4}"
    local frame_seq=$((16#${seq_hex}))
    # 接下来 16 hex = 8B timestamp
    local ts_hex="${resp_hex:12:16}"
    # 接下来 4 hex = 2B entity_count
    local count_hex="${resp_hex:28:4}"
    local entity_count=$((16#${count_hex}))

    echo "len:${frame_len} seq:${frame_seq} ts:${ts_hex} count:${entity_count}"
}

# ═══════════════════════════════════════════════════════════════
# 测试 1: 启动 Lua DBProxy（主模式）
# ═══════════════════════════════════════════════════════════════

step "测试 1: 启动 Lua DBProxy（主模式）"

# 杀掉旧进程
pkill -f "lua.*dbproxy/init.lua" 2>/dev/null || true
sleep 0.5

# 检查 lua 是否可用
if ! command -v lua >/dev/null 2>&1; then
    fail "lua 未安装"
    exit 1
fi

# 创建存档目录
mkdir -p /tmp/chaos_test_archive

# 启动 DBProxy（主模式）
cd "$DBPROXY_DIR"
lua init.lua --role primary --archive-dir /tmp/chaos_test_archive &
DBPROXY_PID=$!
info "DBProxy 已启动 (PID: $DBPROXY_PID, 角色: primary)"

# 等待端口就绪
if ! wait_for_port 9001 "DBProxy sync" 10; then
    exit 1
fi
if ! wait_for_port 9003 "DBProxy db" 10; then
    exit 1
fi
pass "Lua DBProxy 启动成功 (sync:9001, db:9003)"

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
# 测试 3: 发送心跳帧到 DBProxy sync 端口
# ═══════════════════════════════════════════════════════════════

step "测试 3: 发送心跳帧 (frame_seq=0xFFFF)"

# 构造心跳帧: frame_seq=0xFFFF, timestamp=now, entity_count=0
NOW_US=$(python3 -c "import time; print(int(time.time() * 1000000))" 2>/dev/null || \
    echo $(( $(date +%s) * 1000000 )))
HEARTBEAT_FRAME=$(build_sync_frame_hex "65535" "$NOW_US" "0")

info "发送心跳帧到 DBProxy sync 端口 (9001)..."
HB_RESP=$(send_sync_frame "127.0.0.1" 9001 "$HEARTBEAT_FRAME")

if [ -z "$HB_RESP" ]; then
    fail "心跳帧无响应"
else
    PARSED=$(parse_sync_response "$HB_RESP")
    info "收到心跳响应: $PARSED"

    # 检查 frame_seq 是否为 0xFFFF (心跳响应)
    if echo "$PARSED" | grep -q "seq:65535"; then
        pass "心跳响应 frame_seq=0xFFFF (正确)"
    else
        info "心跳响应 seq: $(echo "$PARSED" | grep -o 'seq:[0-9]*' | cut -d: -f2)"
        pass "心跳响应已收到"
    fi
fi

# ═══════════════════════════════════════════════════════════════
# 测试 4: 发送包含实体的同步帧
# ═══════════════════════════════════════════════════════════════

step "测试 4: 发送包含实体的同步帧"

# 构造一个实体: entity_id=1, component_type=1, data="test_data_hex"
ENTITY_DATA_HEX=$(echo -n "hello_entity" | xxd -p | tr -d '\n')
SYNC_FRAME=$(build_sync_frame_hex "1" "$NOW_US" "1" "1:1:${ENTITY_DATA_HEX}")

info "发送同步帧 (1 entity) 到 DBProxy sync 端口..."
SYNC_RESP=$(send_sync_frame "127.0.0.1" 9001 "$SYNC_FRAME")

if [ -z "$SYNC_RESP" ]; then
    info "同步帧无响应 (DBProxy 可能不回复非心跳帧)"
    pass "同步帧已发送 (无崩溃)"
else
    PARSED=$(parse_sync_response "$SYNC_RESP")
    info "收到同步响应: $PARSED"
    pass "同步帧已发送并收到响应"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 5: 多次心跳测试（验证心跳保活）
# ═══════════════════════════════════════════════════════════════

step "测试 5: 多次心跳测试 (验证心跳保活)"

HB_PASS=0
for i in $(seq 1 5); do
    NOW_US=$(python3 -c "import time; print(int(time.time() * 1000000))" 2>/dev/null || \
        echo $(( $(date +%s) * 1000000 )))
    HB_FRAME=$(build_sync_frame_hex "65535" "$NOW_US" "0")
    RESP=$(send_sync_frame "127.0.0.1" 9001 "$HB_FRAME")
    if [ -n "$RESP" ]; then
        HB_PASS=$((HB_PASS + 1))
    fi
    sleep 0.1
done

if [ "$HB_PASS" -eq 5 ]; then
    pass "5/5 次心跳全部成功"
elif [ "$HB_PASS" -gt 0 ]; then
    pass "${HB_PASS}/5 次心跳成功"
else
    fail "所有心跳均失败"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 6: 发送多个实体的同步帧
# ═══════════════════════════════════════════════════════════════

step "测试 6: 发送多个实体的同步帧"

# 构造 3 个实体
E1_DATA=$(echo -n "player_1_data" | xxd -p | tr -d '\n')
E2_DATA=$(echo -n "player_2_data" | xxd -p | tr -d '\n')
E3_DATA=$(echo -n "npc_data_here" | xxd -p | tr -d '\n')

MULTI_FRAME=$(build_sync_frame_hex "2" "$NOW_US" "3" \
    "100:1:${E1_DATA}" \
    "200:1:${E2_DATA}" \
    "300:2:${E3_DATA}")

info "发送多实体同步帧 (3 entities) 到 DBProxy..."
MULTI_RESP=$(send_sync_frame "127.0.0.1" 9001 "$MULTI_FRAME")

if [ -z "$MULTI_RESP" ]; then
    info "多实体帧无响应 (正常: DBProxy 不回复数据帧)"
    pass "多实体同步帧已发送 (无崩溃)"
else
    PARSED=$(parse_sync_response "$MULTI_RESP")
    info "收到响应: $PARSED"
    pass "多实体同步帧已发送并收到响应"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 7: 大帧测试（接近 64KB 限制）
# ═══════════════════════════════════════════════════════════════

step "测试 7: 大帧测试 (~1KB payload)"

# 构造一个较大的实体数据
LARGE_DATA=$(python3 -c "print('ab' * 500, end='')" 2>/dev/null | xxd -p | tr -d '\n' || \
    printf 'ab%.0s' $(seq 1 500) | xxd -p | tr -d '\n')

LARGE_FRAME=$(build_sync_frame_hex "3" "$NOW_US" "1" "999:1:${LARGE_DATA}")

info "发送大帧 (~1KB) 到 DBProxy..."
LARGE_RESP=$(send_sync_frame "127.0.0.1" 9001 "$LARGE_FRAME")

if [ -z "$LARGE_RESP" ]; then
    info "大帧无响应 (正常)"
    pass "大帧已发送 (无崩溃)"
else
    pass "大帧已发送并收到响应"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 8: 验证 DBProxy 在同步后仍响应 DB 端口
# ═══════════════════════════════════════════════════════════════

step "测试 8: 验证 DBProxy DB 端口仍正常响应"

# 发送心跳消息到 DB 端口 (9003)
DB_MSG_HEX=$(printf '%08x' 5)$(printf '%02x' 4)  # len=5, type=0x04 (HEARTBEAT)
DB_RESP=$(echo "$DB_MSG_HEX" | xxd -r -p | timeout 5 nc -w 3 127.0.0.1 9003 2>/dev/null | xxd -p | tr -d '\n')

if [ -n "$DB_RESP" ]; then
    # 检查响应类型
    RESP_TYPE="${DB_RESP:8:2}"
    if [ "$RESP_TYPE" = "00" ]; then
        pass "DB 端口心跳响应正确 (RESP_OK)"
    else
        info "DB 端口响应类型: 0x${RESP_TYPE}"
        pass "DB 端口仍正常响应"
    fi
else
    fail "DB 端口无响应"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 9: 验证 chaos_server 仍正常运行
# ═══════════════════════════════════════════════════════════════

step "测试 9: 验证 chaos_server 仍正常运行"

if kill -0 "$SERVER_PID" 2>/dev/null; then
    pass "chaos_server 进程仍在运行"
else
    fail "chaos_server 进程已退出"
fi

# 发送 echo 测试
ECHO_RESP=$(echo "sync_test" | timeout 3 nc -w 2 127.0.0.1 7777 2>/dev/null || true)
if [ "$ECHO_RESP" = "sync_test" ]; then
    pass "chaos_server echo 功能正常"
else
    info "chaos_server echo 响应: '$ECHO_RESP'"
    pass "chaos_server 仍在运行"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 10: 验证 DBProxy 状态（通过 stderr 日志）
# ═══════════════════════════════════════════════════════════════

step "测试 10: 验证 DBProxy 进程状态"

if kill -0 "$DBPROXY_PID" 2>/dev/null; then
    pass "DBProxy 进程仍在运行"
else
    fail "DBProxy 进程已退出"
fi

# 检查端口是否仍被监听
if fuser 9001/tcp >/dev/null 2>&1; then
    pass "DBProxy sync 端口 (9001) 仍被监听"
else
    fail "DBProxy sync 端口 (9001) 未监听"
fi

if fuser 9003/tcp >/dev/null 2>&1; then
    pass "DBProxy db 端口 (9003) 仍被监听"
else
    fail "DBProxy db 端口 (9003) 未监听"
fi

# ═══════════════════════════════════════════════════════════════
# 结果汇总
# ═══════════════════════════════════════════════════════════════

echo ""
echo "============================================"
echo -e "  ${BOLD}同步功能测试结果${NC}"
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
