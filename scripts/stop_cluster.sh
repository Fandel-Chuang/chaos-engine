#!/bin/bash
# ChaosEngine 一键停止全集群
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PID_DIR="${PROJECT_DIR}/.pids"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "============================================"
echo " ChaosEngine 停止集群"
echo "============================================"

STOPPED=0
for pidfile in "$PID_DIR"/*.pid; do
    [ -f "$pidfile" ] || continue
    name=$(basename "$pidfile" .pid)
    pid=$(cat "$pidfile")

    if kill -0 "$pid" 2>/dev/null; then
        echo -e "${YELLOW}🛑 停止 ${name} (PID $pid)...${NC}"
        kill "$pid" 2>/dev/null || true
        sleep 0.5
        # 强制杀
        kill -9 "$pid" 2>/dev/null || true
        STOPPED=$((STOPPED + 1))
    fi
    rm -f "$pidfile"
done

# 清理残留的 chaos 进程
ORPHANS=$(pgrep -f "chaos_" 2>/dev/null || true)
if [ -n "$ORPHANS" ]; then
    echo -e "${YELLOW}清理残留 chaos 进程...${NC}"
    for p in $ORPHANS; do
        kill -9 "$p" 2>/dev/null || true
    done
fi

# 清理 lapis/nginx admin 进程
NGINX_PIDS=$(pgrep -f "nginx.*admin" 2>/dev/null || true)
if [ -n "$NGINX_PIDS" ]; then
    echo -e "${YELLOW}清理 nginx admin 进程...${NC}"
    for p in $NGINX_PIDS; do
        kill "$p" 2>/dev/null || true
    done
fi

# 清理 admin socket
rm -f /tmp/chaos_admin.sock

echo ""
echo -e "${GREEN}✅ 已停止 ${STOPPED} 个服务${NC}"
echo "============================================"
