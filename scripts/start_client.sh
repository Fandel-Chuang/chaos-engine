#!/bin/bash
# ChaosEngine 一键启动客户端
# 用法: ./scripts/start_client.sh [--tcp] [--ws] [--headless] [--stress N]
#       默认启动 Vulkan 渲染客户端
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"

GATEWAY_HOST="${GATEWAY_HOST:-127.0.0.1}"
GATEWAY_TCP_PORT="${GATEWAY_TCP_PORT:-9000}"
GATEWAY_WS_PORT="${GATEWAY_WS_PORT:-9002}"
GAME_PORT="${GAME_PORT:-7777}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

MODE="vulkan"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tcp)      MODE="tcp"; shift ;;
        --ws)       MODE="ws"; shift ;;
        --headless) MODE="headless"; shift ;;
        --stress)   MODE="stress"; CONNS="${2:-100}"; shift 2 ;;
        --host)     GATEWAY_HOST="$2"; shift 2 ;;
        --port)     GAME_PORT="$2"; shift 2 ;;
        *)          echo "未知参数: $1"; exit 1 ;;
    esac
done

echo "============================================"
echo " ChaosEngine 客户端"
echo " 模式: ${MODE}"
echo "============================================"

# 检查编译
if [ ! -f "${BIN_DIR}/chaos_client" ]; then
    echo -e "${RED}❌ 未编译！请先运行: ./scripts/build_and_test.sh${NC}"
    exit 1
fi

case "$MODE" in
    vulkan)
        echo -e "${BLUE}🚀 启动 Vulkan 渲染客户端...${NC}"
        "${BIN_DIR}/chaos_client"
        ;;

    tcp)
        echo -e "${BLUE}🚀 启动 TCP 网络客户端 → ${GATEWAY_HOST}:${GATEWAY_TCP_PORT}${NC}"
        echo ""
        # 先检查 Gateway 是否在线
        if timeout 1 bash -c "echo -n '' | nc -w1 ${GATEWAY_HOST} ${GATEWAY_TCP_PORT}" 2>/dev/null; then
            echo -e "${GREEN}✅ Gateway 在线${NC}"
        else
            echo -e "${YELLOW}⚠️  Gateway 未响应，尝试直连 Game Server :${GAME_PORT}${NC}"
        fi
        echo ""
        "${BIN_DIR}/chaos_net_client"
        ;;

    ws)
        echo -e "${BLUE}🚀 启动 WebSocket 客户端 → ws://${GATEWAY_HOST}:${GATEWAY_WS_PORT}${NC}"
        echo ""
        # 使用 websocat 或 curl 测试
        if command -v websocat &>/dev/null; then
            echo "  使用 websocat 连接..."
            websocat "ws://${GATEWAY_HOST}:${GATEWAY_WS_PORT}"
        else
            echo -e "${YELLOW}  websocat 未安装，使用 curl 测试 WebSocket 握手...${NC}"
            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                -H "Upgrade: websocket" \
                -H "Connection: Upgrade" \
                -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
                -H "Sec-WebSocket-Version: 13" \
                "http://${GATEWAY_HOST}:${GATEWAY_WS_PORT}" 2>/dev/null || echo "000")
            if [ "$HTTP_CODE" = "101" ]; then
                echo -e "${GREEN}✅ WebSocket 握手成功 (HTTP 101)${NC}"
                echo "  安装 websocat 进行交互: sudo apt install websocat"
            else
                echo -e "${RED}❌ WebSocket 握手失败 (HTTP ${HTTP_CODE})${NC}"
            fi
        fi
        ;;

    headless)
        echo -e "${BLUE}🚀 启动 Headless 客户端 (无渲染)${NC}"
        "${BIN_DIR}/chaos_headless"
        ;;

    stress)
        echo -e "${BLUE}🚀 启动压力测试 (${CONNS} 并发连接)${NC}"
        if [ -f "${BIN_DIR}/bench_client" ]; then
            echo "  目标: ${GATEWAY_HOST}:${GATEWAY_TCP_PORT}"
            echo "  连接数: ${CONNS}"
            echo "  持续时间: 10s"
            echo ""
            "${BIN_DIR}/bench_client" -p "${GATEWAY_TCP_PORT}" -c "$CONNS" -d 10
        else
            echo -e "${RED}❌ bench_client 未编译${NC}"
            exit 1
        fi
        ;;
esac

echo ""
echo -e "${GREEN}✅ 客户端已退出${NC}"
