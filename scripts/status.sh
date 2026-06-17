#!/bin/bash
# ChaosEngine 进程状态查询脚本
# 用法: ./scripts/status.sh [--json] [--watch]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PID_DIR="${PROJECT_DIR}/.pids"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'
BOLD='\033[1m'

JSON_MODE=false
WATCH_MODE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --json)  JSON_MODE=true; shift ;;
        --watch) WATCH_MODE=true; shift ;;
        *)       echo "用法: $0 [--json] [--watch]"; exit 1 ;;
    esac
done

# 服务定义: name port pid_file binary
SERVICES=(
    "game      7777  ${PID_DIR}/game.pid       chaos_server"
    "gateway   9000  ${PID_DIR}/gateway.pid    chaos_gateway"
    "dbproxy   9003  ${PID_DIR}/dbproxy.pid    chaos_dbproxy"
    "router    9100  ${PID_DIR}/router.pid     chaos_router"
    "admin     9090  ${PID_DIR}/admin.pid      lapis"
)

check_port() {
    local port=$1
    ss -tlnp 2>/dev/null | grep -q ":${port} " && echo "true" || echo "false"
}

check_pid() {
    local pid_file=$1
    if [ -f "$pid_file" ]; then
        local pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            echo "$pid"
            return 0
        fi
    fi
    echo ""
    return 1
}

get_process_info() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        local cpu=$(ps -p "$pid" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "?")
        local mem=$(ps -p "$pid" -o %mem --no-headers 2>/dev/null | tr -d ' ' || echo "?")
        local rss=$(ps -p "$pid" -o rss --no-headers 2>/dev/null | tr -d ' ' || echo "?")
        local uptime=$(ps -p "$pid" -o etime --no-headers 2>/dev/null | tr -d ' ' || echo "?")
        echo "$cpu|$mem|$rss|$uptime"
    else
        echo "?|?|?|?"
    fi
}

print_status() {
    if $JSON_MODE; then
        echo "{"
        echo '  "timestamp": "'$(date -Iseconds)'",'
        echo '  "services": ['
        local first=true
        for svc in "${SERVICES[@]}"; do
            IFS=' ' read -r name port pid_file binary <<< "$svc"
            local port_open=$(check_port "$port")
            local pid=$(check_pid "$pid_file")
            local info=$(get_process_info "$pid")
            IFS='|' read -r cpu mem rss uptime <<< "$info"

            local status="stopped"
            if [ -n "$pid" ] && [ "$port_open" = "true" ]; then
                status="running"
            elif [ -n "$pid" ]; then
                status="partial"
            elif [ "$port_open" = "true" ]; then
                status="orphan"
            fi

            $first || echo ","
            first=false
            echo -n "    {"
            echo -n "\"name\":\"$name\","
            echo -n "\"port\":$port,"
            echo -n "\"status\":\"$status\","
            echo -n "\"pid\":\"$pid\","
            echo -n "\"cpu\":\"$cpu\","
            echo -n "\"mem\":\"$mem\","
            echo -n "\"rss_kb\":\"$rss\","
            echo -n "\"uptime\":\"$uptime\""
            echo -n "}"
        done
        echo ""
        echo '  ]'
        echo '}'
    else
        # 彩色表格
        printf "${BOLD}%-10s %-8s %-10s %-8s %-6s %-6s %-10s %s${NC}\n" \
            "SERVICE" "PORT" "STATUS" "PID" "CPU%" "MEM%" "RSS(KB)" "UPTIME"
        printf "%-10s %-8s %-10s %-8s %-6s %-6s %-10s %s\n" \
            "----------" "--------" "----------" "--------" "------" "------" "----------" "----------"

        for svc in "${SERVICES[@]}"; do
            IFS=' ' read -r name port pid_file binary <<< "$svc"
            local port_open=$(check_port "$port")
            local pid=$(check_pid "$pid_file")
            local info=$(get_process_info "$pid")
            IFS='|' read -r cpu mem rss uptime <<< "$info"

            local status_str
            local color
            if [ -n "$pid" ] && [ "$port_open" = "true" ]; then
                status_str="● RUNNING"
                color="$GREEN"
            elif [ -n "$pid" ]; then
                status_str="◐ NO-PORT"
                color="$YELLOW"
            elif [ "$port_open" = "true" ]; then
                status_str="○ ORPHAN"
                color="$YELLOW"
            else
                status_str="✕ DOWN"
                color="$RED"
            fi

            printf "${color}%-10s %-8s %-10s %-8s %-6s %-6s %-10s %s${NC}\n" \
                "$name" "$port" "$status_str" "${pid:- -}" "$cpu" "$mem" "${rss:- -}" "$uptime"
        done
    fi
}

# 主逻辑
if $WATCH_MODE; then
    echo "按 Ctrl+C 退出监控..."
    while true; do
        clear 2>/dev/null || true
        echo "ChaosEngine 集群状态 — $(date '+%Y-%m-%d %H:%M:%S')"
        echo ""
        print_status
        sleep 2
    done
else
    print_status
fi
