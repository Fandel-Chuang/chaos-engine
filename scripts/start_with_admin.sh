#!/bin/bash
# ============================================================
# ChaosEngine + Admin Dashboard 一键启动脚本
# 自动杀掉旧进程，构建，启动 server + admin
# ============================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${GREEN}[✓]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
step() { echo -e "\n${CYAN}>>> $1${NC}"; }

# ──────────────────────────────────────────────
# 0. 杀掉旧进程（温和方式）
# ──────────────────────────────────────────────
step "0/4 清理旧进程..."

killed=0

# 杀掉旧的 chaos_server（用 SIGTERM 而非 SIGKILL）
for pid in $(pgrep -f "chaos_server" 2>/dev/null || true); do
    warn "终止旧的 chaos_server (PID: $pid)..."
    kill "$pid" 2>/dev/null || true
    killed=1
done

# 杀掉旧的 lapis/lua 进程
for pid in $(pgrep -f "lapis server" 2>/dev/null || true); do
    warn "终止旧的 lapis server (PID: $pid)..."
    kill "$pid" 2>/dev/null || true
    killed=1
done

# 杀掉旧的 nginx/openresty（lapis production 模式遗留）
for pid in $(pgrep -f "nginx.*admin" 2>/dev/null || true); do
    warn "终止旧的 nginx (PID: $pid)..."
    kill "$pid" 2>/dev/null || true
    killed=1
done

# 杀掉占用 9090 端口的进程
for pid in $(fuser 9090/tcp 2>/dev/null || true); do
    warn "终止占用 9090 端口的进程 (PID: $pid)..."
    kill "$pid" 2>/dev/null || true
    killed=1
done

# 杀掉占用 7777 端口的进程
for pid in $(fuser 7777/tcp 2>/dev/null || true); do
    warn "终止占用 7777 端口的进程 (PID: $pid)..."
    kill "$pid" 2>/dev/null || true
    killed=1
done

if [ $killed -eq 1 ]; then
    sleep 2
    log "旧进程已清理"
else
    log "无旧进程需要清理"
fi

# 清理旧 socket 文件
rm -f /tmp/chaos_admin.sock

# ──────────────────────────────────────────────
# 1. 构建
# ──────────────────────────────────────────────
step "1/4 构建 chaos_server..."

cd "$PROJECT_DIR/build"
make chaos_server -j$(nproc) 2>&1 | tail -5
log "chaos_server 构建完成"

# ──────────────────────────────────────────────
# 2. 启动 chaos_server
# ──────────────────────────────────────────────
step "2/4 启动 chaos_server (带 admin IPC)..."

./build/bin/chaos_server --admin &
SERVER_PID=$!

# 等待 socket 就绪
for i in $(seq 1 50); do
    if [ -S /tmp/chaos_admin.sock ]; then
        log "Admin socket 就绪: /tmp/chaos_admin.sock"
        break
    fi
    sleep 0.1
done

if [ ! -S /tmp/chaos_admin.sock ]; then
    echo -e "${RED}[✗] Admin socket 未创建，请检查 chaos_server 是否正常启动${NC}"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi

# ──────────────────────────────────────────────
# 3. 启动 chaos_admin (Lapis cqueues 模式)
# ──────────────────────────────────────────────
step "3/4 启动 chaos_admin (Lapis Web 后台, cqueues 模式)..."

cd "$PROJECT_DIR/src_lua/admin"
# 使用 cqueues 模式（不需要 OpenResty/nginx）
lapis server &
ADMIN_PID=$!

# 等待 HTTP 服务就绪
for i in $(seq 1 30); do
    if curl -s http://localhost:9090/api/health > /dev/null 2>&1; then
        log "HTTP 服务就绪: http://localhost:9090"
        break
    fi
    sleep 0.2
done

# ──────────────────────────────────────────────
# 4. 打印信息
# ──────────────────────────────────────────────
step "4/4 启动完成"

echo ""
echo "============================================"
echo "  chaos_server PID:  $SERVER_PID"
echo "  chaos_admin PID:   $ADMIN_PID"
echo "  Dashboard:         http://localhost:9090"
echo "  API Health:        http://localhost:9090/api/health"
echo "  API Stats:         http://localhost:9090/api/stats"
echo "============================================"
echo ""
echo "按 Ctrl+C 停止所有服务"

# ──────────────────────────────────────────────
# 清理 trap
# ──────────────────────────────────────────────
cleanup() {
    echo ""
    warn "正在停止服务..."
    kill $SERVER_PID 2>/dev/null || true
    kill $ADMIN_PID 2>/dev/null || true
    # 确保子进程也被清理
    for pid in $(pgrep -f "nginx.*admin" 2>/dev/null || true); do
        kill "$pid" 2>/dev/null || true
    done
    rm -f /tmp/chaos_admin.sock
    log "所有服务已停止"
}
trap cleanup EXIT INT TERM

wait
