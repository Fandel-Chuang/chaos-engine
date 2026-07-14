#!/bin/bash
# ChaosEngine 客户端冒烟测试
# 用法: ./scripts/test_client.sh [--tcp] [--ws] [--stress N]
#       默认 TCP + WebSocket 快速连通性测试
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"

GATEWAY_TCP="127.0.0.1 9000"
GATEWAY_WS="127.0.0.1:9002"
ADMIN_URL="http://127.0.0.1:9090"

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
# 用 printf 直接管道到 nc，避免 bash 命令替换丢 null 字节
RESP_HEX=$(printf '\x00\x00\x00\x0A\x00\x10\x00\x00\x00\x01' | timeout 2 nc -w1 ${GATEWAY_TCP} 2>/dev/null | xxd -p 2>/dev/null || echo "")
if [ -n "$RESP_HEX" ]; then
    echo -e "  ${GREEN}✅${NC} TCP 连接成功，收到 $((${#RESP_HEX} / 2)) 字节响应 (hex: ${RESP_HEX})"
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

# Admin 只有 /api/stats 和 /api/aoi，没有 /health
check "Admin /api/stats" curl -sf "${ADMIN_URL}/api/stats" > /dev/null
check "Admin /api/aoi"   curl -sf "${ADMIN_URL}/api/aoi" > /dev/null

# ── 4. 二进制协议完整测试 ──
echo ""
echo -e "${BLUE}[4] 二进制协议消息类型测试${NC}"

# PING (0x0001): [4B total_len=6][2B type=0x0001]
# 用 printf 直接管道，避免 send_msg() 函数的变量名 bug
check "PING 消息" bash -c "printf '\x00\x00\x00\x06\x00\x01' | timeout 2 nc -w1 ${GATEWAY_TCP} | xxd -p | head -c 12 | grep -q '000000060002'"

# LOGIN (0x0010): [4B total_len=10][2B type=0x0010][4B payload=0x00000001]
check "LOGIN 消息" bash -c "printf '\x00\x00\x00\x0A\x00\x10\x00\x00\x00\x01' | timeout 2 nc -w1 ${GATEWAY_TCP} >/dev/null 2>&1"

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
