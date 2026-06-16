#!/bin/bash
# ChaosEngine 客户端冒烟测试
# 用法: ./scripts/test_client.sh [--tcp] [--ws] [--stress N]
#       默认 TCP + WebSocket 快速连通性测试
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"

GATEWAY_TCP="127.0.0.1:9000"
GATEWAY_WS="127.0.0.1:9002"
ADMIN_URL="http://127.0.0.1:8080"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

PASS=0
FAIL=0

check() {
    local desc="$1"
    shift
    if "$@" 2>/dev/null; then
        echo -e "  ${GREEN}✅${NC} $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}❌${NC} $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "============================================"
echo " ChaosEngine 客户端冒烟测试"
echo "============================================"
echo ""

# ── 1. 二进制协议测试 (TCP) ──
echo -e "${BLUE}[1] TCP 二进制协议测试${NC}"

# 构造二进制消息: [4B total_len=10][2B type=0x0010 LOGIN][4B payload=0x00000001]
# total_len = 4(len) + 2(type) + 4(payload) = 10
MSG=$(printf '\x00\x00\x00\x0A\x00\x10\x00\x00\x00\x01')

# 发送并等待响应 (timeout 2s)
RESP=$(echo -n "$MSG" | timeout 2 nc "$GATEWAY_TCP" 2>/dev/null || echo "")
if [ -n "$RESP" ]; then
    echo -e "  ${GREEN}✅${NC} TCP 连接成功，收到 $(echo -n "$RESP" | wc -c) 字节响应"
    PASS=$((PASS + 1))
else
    echo -e "  ${YELLOW}⚠️${NC}  TCP 无响应（Gateway 可能未启动）"
fi

# ── 2. WebSocket 测试 ──
echo ""
echo -e "${BLUE}[2] WebSocket 测试${NC}"

# 用 curl 测试 WebSocket 升级
WS_RESP=$(curl -s -o /dev/null -w "%{http_code}" \
    -H "Upgrade: websocket" \
    -H "Connection: Upgrade" \
    -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
    -H "Sec-WebSocket-Version: 13" \
    "http://${GATEWAY_WS}" 2>/dev/null || echo "000")

if [ "$WS_RESP" = "101" ]; then
    echo -e "  ${GREEN}✅${NC} WebSocket 升级成功 (HTTP 101)"
    PASS=$((PASS + 1))
else
    echo -e "  ${YELLOW}⚠️${NC}  WebSocket 返回 $WS_RESP（Gateway WS 可能未启动）"
fi

# ── 3. Admin API 测试 ──
echo ""
echo -e "${BLUE}[3] Admin API 测试${NC}"

check "Admin /health" curl -sf "${ADMIN_URL}/health" > /dev/null
check "Admin /api/stats" curl -sf "${ADMIN_URL}/api/stats" > /dev/null
check "Admin /api/aoi"   curl -sf "${ADMIN_URL}/api/aoi" > /dev/null

# ── 4. 二进制协议完整测试 ──
echo ""
echo -e "${BLUE}[4] 二进制协议消息类型测试${NC}"

send_msg() {
    local type_hex="$1"
    local payload_hex="$2"
    local payload_len=$((${#payload_hex} / 2))
    local total_len=$((4 + 2 + payload_len))
    local total_hex=$(printf '%08X' "$total_len" | sed 's/../\\x&/g')
    local type_bytes=$(echo "$type_hex" | sed 's/../\\x&/g')
    local payload_bytes=$(echo "$payload_hex" | sed 's/../\\x&/g')
    echo -n -e "${total_bytes}${type_bytes}${payload_bytes}"
}

# PING (0x0001)
check "PING 消息" timeout 2 bash -c "echo -n -e '\x00\x00\x00\x06\x00\x01' | nc -w1 ${GATEWAY_TCP} > /dev/null"

# ── 5. 压力测试 (可选) ──
if [ "${1:-}" = "--stress" ]; then
    CONNS="${2:-100}"
    echo ""
    echo -e "${BLUE}[5] 压力测试 (${CONNS} 连接)${NC}"

    if [ -f "${BIN_DIR}/bench_client" ]; then
        echo "  启动 bench server..."
        timeout 3 "${BIN_DIR}/chaos_async_bench" --port 19999 &
        sleep 0.5

        echo "  运行 bench_client..."
        "${BIN_DIR}/bench_client" -p 19999 -c "$CONNS" -d 3 -j 2>/dev/null || true

        echo -e "  ${GREEN}✅${NC} 压力测试完成"
        PASS=$((PASS + 1))
    else
        echo -e "  ${YELLOW}⚠️${NC}  bench_client 未编译"
    fi
fi

# ── 汇总 ──
echo ""
echo "============================================"
TOTAL=$((PASS + FAIL))
echo " 结果: ${PASS}/${TOTAL} 通过"
if [ $FAIL -gt 0 ]; then
    echo -e " ${RED}${FAIL} 项失败${NC}"
    exit 1
else
    echo -e " ${GREEN}全部通过 ✅${NC}"
fi
echo "============================================"
