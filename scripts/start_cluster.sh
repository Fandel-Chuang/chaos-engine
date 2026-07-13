#!/bin/bash
# ChaosEngine 一键启动全集群
# 用法: ./scripts/start_cluster.sh [--game] [--gateway] [--router] [--dbproxy] [--admin] [--all]
#       默认启动全部服务
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
LOG_DIR="${PROJECT_DIR}/logs"
PID_DIR="${PROJECT_DIR}/.pids"

# 端口
GAME_PORT=7777
GATEWAY_TCP=9000
GATEWAY_KCP=9001
GATEWAY_WS=9002
ROUTER_GAME=9100
ROUTER_CLUSTER=9101
DBPROXY_SYNC=9001
DBPROXY_DB=9003
ADMIN_PORT=9090

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

mkdir -p "$LOG_DIR" "$PID_DIR"

# ── 解析参数 ──
START_GAME=0; START_GATEWAY=0; START_ROUTER=0; START_DBPROXY=0; START_ADMIN=0
if [ $# -eq 0 ]; then
    START_GAME=1; START_GATEWAY=1; START_ROUTER=1; START_DBPROXY=1; START_ADMIN=1
fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --game)    START_GAME=1 ;;
        --gateway) START_GATEWAY=1 ;;
        --router)  START_ROUTER=1 ;;
        --dbproxy) START_DBPROXY=1 ;;
        --admin)   START_ADMIN=1 ;;
        --all)     START_GAME=1; START_GATEWAY=1; START_ROUTER=1; START_DBPROXY=1; START_ADMIN=1 ;;
        *)         echo "未知参数: $1"; exit 1 ;;
    esac
    shift
done

# ── 辅助函数 ──
start_proc() {
    local name="$1"; local bin="$2"; local args="$3"
    local log="$LOG_DIR/${name}.log"; local pidfile="$PID_DIR/${name}.pid"

    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo -e "${YELLOW}⚠️  ${name}: 已在运行 (PID $(cat "$pidfile"))${NC}"
        return 0
    fi

    echo -e "${BLUE}🚀 启动 ${name}...${NC}"
    nohup $bin $args > "$log" 2>&1 &
    local pid=$!; echo "$pid" > "$pidfile"
    sleep 0.5
    if kill -0 "$pid" 2>/dev/null; then
        echo -e "${GREEN}✅ ${name}: PID $pid${NC}"
    else
        echo -e "${RED}❌ ${name}: 启动失败 → $log${NC}"
        tail -5 "$log"
        return 1
    fi
}

echo "============================================"
echo " ChaosEngine 集群启动"
echo "============================================"
echo ""

# ── 1. DBProxy (C 进程，加载 Lua 业务脚本) ──
if [ $START_DBPROXY -eq 1 ]; then
    start_proc "dbproxy" "${BIN_DIR}/chaos_dbproxy" \
        "--role primary --sync-port ${DBPROXY_SYNC} --db-port ${DBPROXY_DB}"
fi

# ── 2. Game Server (C 进程，带 admin socket) ──
if [ $START_GAME -eq 1 ]; then
    rm -f /tmp/chaos_admin.sock
    start_proc "game" "${BIN_DIR}/chaos_server" "--admin"
fi

# ── 3. Router (C 进程，加载 Lua 业务脚本) ──
if [ $START_ROUTER -eq 1 ]; then
    start_proc "router" "${BIN_DIR}/chaos_router" \
        "--game-port ${ROUTER_GAME} --cluster-port ${ROUTER_CLUSTER}"
fi

# ── 4. Gateway (C 进程，加载 Lua 业务脚本) ──
if [ $START_GATEWAY -eq 1 ]; then
    start_proc "gateway" "${BIN_DIR}/chaos_gateway" \
        "--port ${GATEWAY_TCP} --backend 127.0.0.1:${GAME_PORT}"
fi

# ── 5. Admin Web (Lapis) ──
if [ $START_ADMIN -eq 1 ]; then
    # 等待 admin socket 就绪
    echo -e "${BLUE}🚀 启动 Admin Web (Lapis)...${NC}"
    for i in $(seq 1 30); do
        if [ -S /tmp/chaos_admin.sock ]; then break; fi
        sleep 0.2
    done

    if [ ! -S /tmp/chaos_admin.sock ]; then
        echo -e "${RED}❌ Admin socket 未就绪 (game server 需带 --admin)${NC}"
    else
        echo -e "${GREEN}✅ Admin socket 就绪${NC}"
        cd "${PROJECT_DIR}/src_lua/admin"
        nohup lapis server > "${LOG_DIR}/admin.log" 2>&1 &
        apid=$!; echo "$apid" > "${PID_DIR}/admin.pid"
        cd "${PROJECT_DIR}"
        sleep 2
        if kill -0 "$apid" 2>/dev/null; then
            echo -e "${GREEN}✅ admin: PID $apid → http://localhost:${ADMIN_PORT}${NC}"
        else
            echo -e "${RED}❌ admin 启动失败 → ${LOG_DIR}/admin.log${NC}"
            tail -5 "${LOG_DIR}/admin.log"
        fi
    fi
fi

# ── 状态汇总 ──
echo ""
echo "============================================"
echo " 集群状态"
echo "============================================"

print_status() {
    local name="$1"; local desc="$2"; local pidfile="$PID_DIR/${name}.pid"
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        printf "  ${GREEN}%-10s${NC} PID %-6s  %s\n" "$name" "$(cat "$pidfile")" "$desc"
    else
        printf "  ${RED}%-10s${NC} 未运行\n" "$name"
    fi
}

print_status "dbproxy"  "sync :${DBPROXY_SYNC}  db :${DBPROXY_DB}"
print_status "game"     "port :${GAME_PORT}  admin /tmp/chaos_admin.sock"
print_status "router"   "game :${ROUTER_GAME}  cluster :${ROUTER_CLUSTER}"
print_status "gateway"  "tcp :${GATEWAY_TCP}  kcp :${GATEWAY_KCP}  ws :${GATEWAY_WS}"
print_status "admin"    "http://localhost:${ADMIN_PORT}"

echo ""
echo "============================================"
echo " 连接方式"
echo "============================================"
echo "  Game TCP:  telnet 127.0.0.1 ${GAME_PORT}"
echo "  Gateway:   telnet 127.0.0.1 ${GATEWAY_TCP}"
echo "  Admin:     http://localhost:${ADMIN_PORT}"
echo ""
echo " 停止:      ./scripts/stop_cluster_server.sh"
echo " 日志:      tail -f logs/*.log"
echo "============================================"
