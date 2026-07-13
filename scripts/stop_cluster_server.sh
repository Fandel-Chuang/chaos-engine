#!/bin/bash
# ChaosEngine 安全关闭服务
# 先 SIGTERM 优雅退出，超时才 SIGKILL
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PID_DIR="${PROJECT_DIR}/.pids"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

TIMEOUT=10  # 优雅退出等待秒数

echo "============================================"
echo " ChaosEngine 安全关闭服务"
echo "============================================"

STOPPED=0
FAILED=0

# 1. 通过 PID 文件关闭已注册的服务
for pidfile in "$PID_DIR"/*.pid; do
    [ -f "$pidfile" ] || continue
    name=$(basename "$pidfile" .pid)
    pid=$(cat "$pidfile")

    if kill -0 "$pid" 2>/dev/null; then
        echo -e "${YELLOW}🛑 发送 SIGTERM -> ${name} (PID $pid)...${NC}"
        kill -TERM "$pid" 2>/dev/null || true

        # 等待优雅退出
        for i in $(seq 1 $TIMEOUT); do
            if ! kill -0 "$pid" 2>/dev/null; then
                break
            fi
            sleep 1
        done

        # 还没退出，强制杀
        if kill -0 "$pid" 2>/dev/null; then
            echo -e "${RED}⚠ ${name} 未在 ${TIMEOUT}s 内退出，发送 SIGKILL${NC}"
            kill -9 "$pid" 2>/dev/null || true
        else
            echo -e "${GREEN}✅ ${name} 已安全退出${NC}"
        fi
        STOPPED=$((STOPPED + 1))
    fi
    rm -f "$pidfile"
done

# 2. 清理残留的 chaos 进程（不在 PID 文件中的）
ORPHANS=$(pgrep -f "chaos_(gateway|server|client)" 2>/dev/null || true)
if [ -n "$ORPHANS" ]; then
    echo -e "${YELLOW}清理残留 chaos 进程...${NC}"
    for p in $ORPHANS; do
        # 先 SIGTERM
        kill -TERM "$p" 2>/dev/null || true
        sleep 1
        # 还在就 SIGKILL
        if kill -0 "$p" 2>/dev/null; then
            kill -9 "$p" 2>/dev/null || true
        fi
    done
fi

# 3. Docker 容器（如果在 Docker 中运行）
if command -v docker &>/dev/null; then
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^chaos-server"; then
        echo -e "${YELLOW}🛑 停止 Docker 容器 chaos-server...${NC}"
        docker stop chaos-server 2>/dev/null || true
        echo -e "${GREEN}✅ chaos-server 容器已停止${NC}"
    fi
fi

# 4. 清理 admin socket
rm -f /tmp/chaos_admin.sock

echo ""
echo -e "${GREEN}✅ 已安全停止 ${STOPPED} 个服务${NC}"
echo "============================================"
