#!/bin/bash
# ChaosEngine 一键启动全集群
# 用法: ./scripts/start_cluster.sh [--gateway] [--router] [--game] [--dbproxy] [--admin] [--all]
#       默认启动全部服务
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
LOG_DIR="${PROJECT_DIR}/logs"
PID_DIR="${PROJECT_DIR}/.pids"

# 端口配置
GATEWAY_TCP_PORT=9000
GATEWAY_KCP_PORT=9001
GATEWAY_WS_PORT=9002
ROUTER_GAME_PORT=9100
ROUTER_CLUSTER_PORT=9101
GAME_PORT=7777
DBPROXY_PORT=9001
ADMIN_PORT=8080

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

mkdir -p "$LOG_DIR" "$PID_DIR"

# ── 解析参数 ──
START_GATEWAY=0
START_ROUTER=0
START_GAME=0
START_DBPROXY=0
START_ADMIN=0

if [ $# -eq 0 ]; then
    START_GATEWAY=1; START_ROUTER=1; START_GAME=1; START_DBPROXY=1; START_ADMIN=1
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --gateway) START_GATEWAY=1 ;;
        --router)  START_ROUTER=1 ;;
        --game)    START_GAME=1 ;;
        --dbproxy) START_DBPROXY=1 ;;
        --admin)   START_ADMIN=1 ;;
        --all)     START_GATEWAY=1; START_ROUTER=1; START_GAME=1; START_DBPROXY=1; START_ADMIN=1 ;;
        *)         echo "未知参数: $1"; exit 1 ;;
    esac
    shift
done

# ── 辅助函数 ──
start_service() {
    local name="$1"
    local bin="$2"
    local args="$3"
    local log="$LOG_DIR/${name}.log"
    local pidfile="$PID_DIR/${name}.pid"

    if [ ! -f "$bin" ]; then
        echo -e "${RED}❌ ${name}: 二进制不存在 ($bin)${NC}"
        return 1
    fi

    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo -e "${YELLOW}⚠️  ${name}: 已在运行 (PID $(cat "$pidfile"))${NC}"
        return 0
    fi

    echo -e "${BLUE}🚀 启动 ${name}...${NC}"
    nohup "$bin" $args > "$log" 2>&1 &
    local pid=$!
    echo "$pid" > "$pidfile"

    # 等待启动
    sleep 0.5
    if kill -0 "$pid" 2>/dev/null; then
        echo -e "${GREEN}✅ ${name}: 启动成功 (PID $pid)${NC}"
    else
        echo -e "${RED}❌ ${name}: 启动失败，查看日志: $log${NC}"
        tail -5 "$log"
        return 1
    fi
}

stop_service() {
    local name="$1"
    local pidfile="$PID_DIR/${name}.pid"

    if [ ! -f "$pidfile" ]; then
        return 0
    fi

    local pid=$(cat "$pidfile")
    if kill -0 "$pid" 2>/dev/null; then
        echo -e "${YELLOW}🛑 停止 ${name} (PID $pid)...${NC}"
        kill "$pid" 2>/dev/null || true
        sleep 0.3
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
}

# ── 检查编译 ──
if [ ! -f "${BIN_DIR}/chaos_server" ]; then
    echo -e "${RED}❌ 未编译！请先运行: ./scripts/build_and_test.sh${NC}"
    exit 1
fi

echo "============================================"
echo " ChaosEngine 集群启动"
echo "============================================"
echo ""

# ── 启动 DBProxy（先启动，Game 依赖它） ──
if [ $START_DBPROXY -eq 1 ]; then
    start_service "dbproxy" "${BIN_DIR}/chaos_server" \
        "--dbproxy --port ${DBPROXY_PORT}"
fi

# ── 启动 Router（Game 注册依赖它） ──
if [ $START_ROUTER -eq 1 ]; then
    start_service "router" "${BIN_DIR}/chaos_router" \
        "--port ${ROUTER_GAME_PORT} --cluster-port ${ROUTER_CLUSTER_PORT}"
fi

# ── 启动 Game Server ──
if [ $START_GAME -eq 1 ]; then
    start_service "game" "${BIN_DIR}/chaos_server" \
        "--port ${GAME_PORT} --router 127.0.0.1:${ROUTER_GAME_PORT} --dbproxy 127.0.0.1:${DBPROXY_PORT}"
fi

# ── 启动 Gateway（最后，客户端入口） ──
if [ $START_GATEWAY -eq 1 ]; then
    start_service "gateway" "${BIN_DIR}/chaos_server" \
        "--gateway --tcp-port ${GATEWAY_TCP_PORT} --kcp-port ${GATEWAY_KCP_PORT} --ws-port ${GATEWAY_WS_PORT} --backend 127.0.0.1:${GAME_PORT}"
fi

# ── 启动 Admin Web ──
if [ $START_ADMIN -eq 1 ]; then
    start_service "admin" "${BIN_DIR}/chaos_server" \
        "--admin --port ${ADMIN_PORT}"
fi

# ── 状态汇总 ──
echo ""
echo "============================================"
echo " 集群状态"
echo "============================================"

print_status() {
    local name="$1"
    local port="$2"
    local pidfile="$PID_DIR/${name}.pid"

    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        local pid=$(cat "$pidfile")
        printf "  ${GREEN}%-12s${NC} PID %-6s 端口 %-6s\n" "$name" "$pid" "$port"
    else
        printf "  ${RED}%-12s${NC} 未运行\n" "$name"
    fi
}

print_status "gateway"  "${GATEWAY_TCP_PORT}/${GATEWAY_KCP_PORT}/${GATEWAY_WS_PORT}"
print_status "router"   "${ROUTER_GAME_PORT}/${ROUTER_CLUSTER_PORT}"
print_status "game"     "${GAME_PORT}"
print_status "dbproxy"  "${DBPROXY_PORT}"
print_status "admin"    "${ADMIN_PORT}"

echo ""
echo "============================================"
echo " 连接方式"
echo "============================================"
echo "  TCP:       telnet 127.0.0.1 ${GATEWAY_TCP_PORT}"
echo "  WebSocket: ws://127.0.0.1:${GATEWAY_WS_PORT}"
echo "  Admin:     http://127.0.0.1:${ADMIN_PORT}"
echo ""
echo " 停止集群:   ./scripts/stop_cluster.sh"
echo " 查看日志:   tail -f logs/*.log"
echo "============================================"
