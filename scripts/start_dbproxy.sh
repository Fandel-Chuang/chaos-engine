#!/bin/bash
# ============================================================
# ChaosEngine DBProxy 启动脚本
#
# 启动 Lua DBProxy 进程，支持 primary/backup 角色。
#
# 用法:
#   ./start_dbproxy.sh                          # 默认 primary
#   ./start_dbproxy.sh --role backup            # 启动为 backup
#   ./start_dbproxy.sh --role primary --peer-host 192.168.1.10 --peer-port 9101
#
# 端口:
#   9001 — 状态同步 (Game → DBProxy 帧同步)
#   9003 — 数据库代理 (Game → DBProxy DB 操作)
# ============================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DBPROXY_DIR="$PROJECT_DIR/src_lua/dbproxy"

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
ROLE="primary"
PEER_HOST="127.0.0.1"
PEER_PORT="9101"
MONGO_URI="mongodb://127.0.0.1:27017"
MONGO_DB="chaos_engine"
ARCHIVE_DIR="/tmp/chaos_dbproxy/archive"
PID_FILE="/tmp/chaos_dbproxy.pid"
LOG_FILE="/tmp/chaos_dbproxy.log"

# ──────────────────────────────────────────────
# 参数解析
# ──────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --role)
            ROLE="$2"
            shift 2
            ;;
        --peer-host)
            PEER_HOST="$2"
            shift 2
            ;;
        --peer-port)
            PEER_PORT="$2"
            shift 2
            ;;
        --mongo-uri)
            MONGO_URI="$2"
            shift 2
            ;;
        --mongo-db)
            MONGO_DB="$2"
            shift 2
            ;;
        --archive-dir)
            ARCHIVE_DIR="$2"
            shift 2
            ;;
        --pid-file)
            PID_FILE="$2"
            shift 2
            ;;
        --log-file)
            LOG_FILE="$2"
            shift 2
            ;;
        --help|-h)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --role ROLE           角色: primary 或 backup (默认: primary)"
            echo "  --peer-host HOST      对端 DBProxy 地址 (默认: 127.0.0.1)"
            echo "  --peer-port PORT      对端 DBProxy 端口 (默认: 9101)"
            echo "  --mongo-uri URI       MongoDB 连接 URI"
            echo "  --mongo-db DB         MongoDB 数据库名 (默认: chaos_engine)"
            echo "  --archive-dir DIR     状态镜像存档目录 (默认: /tmp/chaos_dbproxy/archive)"
            echo "  --pid-file FILE       PID 文件路径"
            echo "  --log-file FILE       日志文件路径"
            echo "  --help, -h            显示此帮助"
            exit 0
            ;;
        *)
            err "未知参数: $1"
            echo "使用 --help 查看帮助"
            exit 1
            ;;
    esac
done

# ──────────────────────────────────────────────
# 验证
# ──────────────────────────────────────────────

if [[ "$ROLE" != "primary" && "$ROLE" != "backup" ]]; then
    err "无效的角色: $ROLE (必须是 primary 或 backup)"
    exit 1
fi

if [[ ! -d "$DBPROXY_DIR" ]]; then
    err "DBProxy 目录不存在: $DBPROXY_DIR"
    exit 1
fi

if [[ ! -f "$DBPROXY_DIR/init.lua" ]]; then
    err "找不到 init.lua: $DBPROXY_DIR/init.lua"
    exit 1
fi

# ──────────────────────────────────────────────
# 检查 Lua 和 LuaSocket
# ──────────────────────────────────────────────

LUA_BIN=""
for candidate in lua5.4 lua5.3 lua; do
    if command -v "$candidate" &>/dev/null; then
        LUA_BIN="$candidate"
        break
    fi
done

if [[ -z "$LUA_BIN" ]]; then
    err "未找到 Lua 解释器 (尝试了 lua5.4, lua5.3, lua)"
    exit 1
fi

log "使用 Lua: $LUA_BIN ($($LUA_BIN -v 2>&1))"

# 检查 luasocket
if ! $LUA_BIN -e "require('socket')" 2>/dev/null; then
    warn "LuaSocket 未安装，尝试安装..."
    if command -v luarocks &>/dev/null; then
        luarocks install luasocket 2>&1 | tail -3
    elif command -v apt-get &>/dev/null; then
        warn "请手动安装: sudo apt-get install lua-socket"
    else
        warn "请手动安装 LuaSocket"
    fi
fi

# ──────────────────────────────────────────────
# 清理旧进程
# ──────────────────────────────────────────────

step "清理旧进程..."

killed=0

# 通过 PID 文件清理
if [[ -f "$PID_FILE" ]]; then
    OLD_PID=$(cat "$PID_FILE" 2>/dev/null || true)
    if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
        warn "终止旧的 DBProxy (PID: $OLD_PID)..."
        kill "$OLD_PID" 2>/dev/null || true
        sleep 1
        # 如果还在运行，强制杀掉
        if kill -0 "$OLD_PID" 2>/dev/null; then
            warn "强制终止 (PID: $OLD_PID)..."
            kill -9 "$OLD_PID" 2>/dev/null || true
        fi
        killed=1
    fi
    rm -f "$PID_FILE"
fi

# 清理占用端口的进程
for port in 9001 9003; do
    for pid in $(fuser ${port}/tcp 2>/dev/null || true); do
        warn "终止占用端口 $port 的进程 (PID: $pid)..."
        kill "$pid" 2>/dev/null || true
        killed=1
    done
done

if [[ $killed -eq 1 ]]; then
    sleep 1
    log "旧进程已清理"
else
    log "无旧进程需要清理"
fi

# ──────────────────────────────────────────────
# 准备运行环境
# ──────────────────────────────────────────────

step "准备运行环境..."

# 创建存档目录
mkdir -p "$ARCHIVE_DIR"
log "存档目录: $ARCHIVE_DIR"

# 创建日志目录
LOG_DIR="$(dirname "$LOG_FILE")"
mkdir -p "$LOG_DIR"

# ──────────────────────────────────────────────
# 启动 DBProxy
# ──────────────────────────────────────────────

step "启动 DBProxy (角色: $ROLE)..."

cd "$PROJECT_DIR"

# 构建 Lua 参数
LUA_ARGS=(
    "$DBPROXY_DIR/init.lua"
    "--role"       "$ROLE"
    "--peer-host"  "$PEER_HOST"
    "--peer-port"  "$PEER_PORT"
    "--mongo-uri"  "$MONGO_URI"
    "--mongo-db"   "$MONGO_DB"
    "--archive-dir" "$ARCHIVE_DIR"
)

banner "============================================"
banner "  ChaosEngine DBProxy"
banner "  角色:       $ROLE"
banner "  同步端口:   9001"
banner "  数据库端口: 9003"
banner "  对端:       $PEER_HOST:$PEER_PORT"
banner "  MongoDB:    $MONGO_URI/$MONGO_DB"
banner "  日志:       $LOG_FILE"
banner "============================================"
echo ""

# 启动进程（后台运行，输出到日志文件）
nohup "$LUA_BIN" "${LUA_ARGS[@]}" >> "$LOG_FILE" 2>&1 &
DBPROXY_PID=$!

# 保存 PID
echo "$DBPROXY_PID" > "$PID_FILE"
log "DBProxy 已启动 (PID: $DBPROXY_PID)"

# ──────────────────────────────────────────────
# 等待就绪
# ──────────────────────────────────────────────

step "等待端口就绪..."

for i in $(seq 1 30); do
    # 检查进程是否还在运行
    if ! kill -0 "$DBPROXY_PID" 2>/dev/null; then
        err "DBProxy 进程已退出，请检查日志: $LOG_FILE"
        tail -20 "$LOG_FILE"
        exit 1
    fi

    # 检查端口是否已监听
    if fuser 9001/tcp >/dev/null 2>&1 && fuser 9003/tcp >/dev/null 2>&1; then
        log "端口 9001 (sync) 和 9003 (db) 已就绪"
        break
    fi

    sleep 0.2
done

# 最终检查
if ! fuser 9001/tcp >/dev/null 2>&1; then
    err "端口 9001 未就绪，请检查日志: $LOG_FILE"
    tail -20 "$LOG_FILE"
    exit 1
fi

if ! fuser 9003/tcp >/dev/null 2>&1; then
    err "端口 9003 未就绪，请检查日志: $LOG_FILE"
    tail -20 "$LOG_FILE"
    exit 1
fi

# ──────────────────────────────────────────────
# 完成
# ──────────────────────────────────────────────

echo ""
echo "============================================"
echo "  DBProxy PID:     $DBPROXY_PID"
echo "  PID 文件:        $PID_FILE"
echo "  日志文件:        $LOG_FILE"
echo "  同步端口:        9001"
echo "  数据库端口:      9003"
echo "  存档目录:        $ARCHIVE_DIR"
echo "============================================"
echo ""
echo "停止 DBProxy:"
echo "  kill \$(cat $PID_FILE)"
echo "  或: touch /tmp/chaos_dbproxy_stop"
echo ""
echo "查看日志:"
echo "  tail -f $LOG_FILE"
echo ""

# ──────────────────────────────────────────────
# 可选：前台监控
# ──────────────────────────────────────────────

# 如果是在终端中运行（有 TTY），提供前台选项
if [[ -t 0 ]]; then
    echo "按 Ctrl+C 停止监控（DBProxy 继续在后台运行）"
    echo "或按 Enter 进入前台监控模式..."
    read -t 3 -r || true

    # Trap Ctrl+C to just exit monitoring, not kill DBProxy
    trap 'echo ""; log "监控已停止 (DBProxy PID: $DBPROXY_PID 仍在运行)"; exit 0' INT

    log "进入监控模式 (Ctrl+C 退出监控)..."
    tail -f "$LOG_FILE"
fi
