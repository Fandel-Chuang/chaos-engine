#!/bin/bash
# ============================================================
# ChaosEngine Gateway 启动脚本
#
# 启动 Lua Gateway 进程，提供 TCP 客户端接入。
#
# 用法:
#   ./start_gateway.sh                              # 默认配置
#   ./start_gateway.sh --port 9000                  # 指定端口
#   ./start_gateway.sh --backend 127.0.0.1:7777     # 指定后端
#   ./start_gateway.sh --log-level debug            # 调试日志
#
# 端口:
#   9000 — TCP 客户端接入
#   9001 — KCP 客户端接入 (预留)
#   9002 — WebSocket 客户端接入 (预留)
# ============================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
GATEWAY_DIR="$PROJECT_DIR/src_lua/gateway"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()    { echo -e "${GREEN}[✓]${NC} $1"; }
warn()   { echo -e "${YELLOW}[!]${NC} $1"; }
err()    { echo -e "${RED}[✗]${NC} $1"; }
step()   { echo -e "\n${CYAN}>>> $1${NC}"; }
banner() { echo -e "${CYAN}$1${NC}"; }

# ──────────────────────────────────────────────
# 默认配置
# ──────────────────────────────────────────────
GATEWAY_PORT="9000"
BACKEND="127.0.0.1:7777"
GAME_SERVER=""
MAX_CONNECTIONS="10000"
LOG_LEVEL="info"
LOG_DIR="$PROJECT_DIR/logs"
LOG_FILE="$LOG_DIR/gateway.log"
PID_FILE="$LOG_DIR/gateway.pid"

# ──────────────────────────────────────────────
# 解析命令行参数
# ──────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            GATEWAY_PORT="$2"
            shift 2
            ;;
        --backend)
            BACKEND="$2"
            shift 2
            ;;
        --game-server)
            GAME_SERVER="$2"
            shift 2
            ;; 
        --max-connections)
            MAX_CONNECTIONS="$2"
            shift 2
            ;;
        --log-level)
            LOG_LEVEL="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --port PORT             TCP listen port (default: 9000)"
            echo "  --backend HOST:PORT     Backend Game service (default: 127.0.0.1:7777)"
            echo "  --game-server HOST:PORT Game server connection (default: 127.0.0.1:9100)"
            echo "  --max-connections N     Max client connections (default: 10000)"
            echo "  --log-level LEVEL       Log level: trace, debug, info, warn, error (default: info)"
            echo "  --help, -h              Show this help"
            exit 0
            ;;
        *)
            warn "未知参数: $1"
            shift
            ;;
    esac
done

# ──────────────────────────────────────────────
# 环境检查
# ──────────────────────────────────────────────

step "检查环境..."

# 检查 Lua 5.4
LUA_BIN=""
if command -v lua5.4 &>/dev/null; then
    LUA_BIN="lua5.4"
elif command -v lua &>/dev/null; then
    LUA_VERSION=$(lua -v 2>&1 | grep -oP '\d+\.\d+' | head -1)
    if [[ "$LUA_VERSION" == "5.4" ]]; then
        LUA_BIN="lua"
    else
        err "需要 Lua 5.4，当前版本: $LUA_VERSION"
        err "请安装 Lua 5.4: sudo apt install lua5.4"
        exit 1
    fi
else
    err "未找到 Lua，请安装 Lua 5.4: sudo apt install lua5.4"
    exit 1
fi
log "Lua: $($LUA_BIN -v 2>&1)"

# 检查 LuaSocket
step "检查 LuaSocket..."

LUA_SOCKET_OK=0
if $LUA_BIN -e "require('socket')" &>/dev/null; then
    LUA_SOCKET_OK=1
    log "LuaSocket 已安装"
else
    warn "LuaSocket 未安装，尝试自动安装..."

    # 尝试通过 apt 安装
    if command -v apt-get &>/dev/null; then
        warn "通过 apt-get 安装 lua-socket..."
        if sudo apt-get install -y lua-socket lua5.4-socket 2>/dev/null; then
            log "LuaSocket 安装成功 (apt)"
            LUA_SOCKET_OK=1
        fi
    fi

    # 尝试通过 luarocks 安装
    if [[ $LUA_SOCKET_OK -eq 0 ]] && command -v luarocks &>/dev/null; then
        warn "通过 luarocks 安装 luasocket..."
        if luarocks install luasocket 2>/dev/null; then
            log "LuaSocket 安装成功 (luarocks)"
            LUA_SOCKET_OK=1
        fi
    fi

    if [[ $LUA_SOCKET_OK -eq 0 ]]; then
        err "无法自动安装 LuaSocket，请手动安装:"
        err "  sudo apt install lua-socket lua5.4-socket"
        err "  或: sudo luarocks install luasocket"
        exit 1
    fi
fi

# ──────────────────────────────────────────────
# 准备目录
# ──────────────────────────────────────────────

step "准备运行目录..."

mkdir -p "$LOG_DIR"

# 清理旧的停止标记
rm -f /tmp/chaos_gateway_stop

# 检查端口是否已被占用
if fuser "${GATEWAY_PORT}/tcp" >/dev/null 2>&1; then
    warn "端口 ${GATEWAY_PORT} 已被占用，尝试释放..."
    fuser -k "${GATEWAY_PORT}/tcp" 2>/dev/null || true
    sleep 1
fi

# ──────────────────────────────────────────────
# 启动 Gateway
# ──────────────────────────────────────────────

step "启动 Gateway..."

cd "$GATEWAY_DIR"

$LUA_BIN init.lua \
    --port "$GATEWAY_PORT" \
    --backend "$BACKEND" \
    --max-connections "$MAX_CONNECTIONS" \
    --log-level "$LOG_LEVEL" \
    >> "$LOG_FILE" 2>&1 &

GATEWAY_PID=$!

# 保存 PID
echo "$GATEWAY_PID" > "$PID_FILE"
log "Gateway 已启动 (PID: $GATEWAY_PID)"

# ──────────────────────────────────────────────
# 等待端口就绪
# ──────────────────────────────────────────────

step "等待端口就绪..."

PORT_READY=0
for i in $(seq 1 30); do
    # 检查进程是否还在运行
    if ! kill -0 "$GATEWAY_PID" 2>/dev/null; then
        err "Gateway 进程已退出，请检查日志: $LOG_FILE"
        tail -20 "$LOG_FILE"
        exit 1
    fi

    # 检查端口是否已监听
    if fuser "${GATEWAY_PORT}/tcp" >/dev/null 2>&1; then
        log "端口 ${GATEWAY_PORT} 已就绪"
        PORT_READY=1
        break
    fi

    sleep 0.2
done

if [[ $PORT_READY -eq 0 ]]; then
    err "端口 ${GATEWAY_PORT} 未就绪，请检查日志: $LOG_FILE"
    tail -20 "$LOG_FILE"
    exit 1
fi

# ──────────────────────────────────────────────
# 完成
# ──────────────────────────────────────────────

echo ""
echo "============================================"
echo "  Gateway PID:     $GATEWAY_PID"
echo "  PID 文件:        $PID_FILE"
echo "  日志文件:        $LOG_FILE"
echo "  TCP 端口:        $GATEWAY_PORT"
echo "  后端服务:        $BACKEND"
echo "  最大连接数:      $MAX_CONNECTIONS"
echo "============================================"
echo ""
echo "停止 Gateway:"
echo "  kill \$(cat $PID_FILE)"
echo "  或: touch /tmp/chaos_gateway_stop"
echo ""
echo "查看日志:"
echo "  tail -f $LOG_FILE"
echo ""

# ──────────────────────────────────────────────
# 可选：前台监控
# ──────────────────────────────────────────────

# 如果是在终端中运行（有 TTY），提供前台选项
if [[ -t 0 ]]; then
    echo "按 Ctrl+C 停止监控（Gateway 继续在后台运行）"
    echo "或按 Enter 进入前台监控模式..."
    read -t 3 -r || true

    # Trap Ctrl+C to just exit monitoring, not kill Gateway
    trap 'echo ""; log "监控已停止 (Gateway PID: $GATEWAY_PID 仍在运行)"; exit 0' INT

    log "进入监控模式 (Ctrl+C 退出监控)..."
    tail -f "$LOG_FILE"
fi
