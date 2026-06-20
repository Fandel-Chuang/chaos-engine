#!/bin/bash
# ============================================================
# ChaosEngine 客户端联调脚本
#
# 启动顺序:
#   1. chaos_server
#   2. 两个 Vulkan 客户端（默认直连 Game Server:7777）
#
# 备注:
#   - 依赖当前桌面会话的 DISPLAY/XAUTHORITY
#   - 仅做联调，不做长期守护
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
SERVER_BIN="${BIN_DIR}/chaos_server"
CLIENT_SCRIPT="${SCRIPT_DIR}/start_client.sh"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SERVER_LOG="${PROJECT_DIR}/logs/client_joint_server.log"
CLIENT1_LOG="${PROJECT_DIR}/logs/client_joint_1.log"
CLIENT2_LOG="${PROJECT_DIR}/logs/client_joint_2.log"
SERVER_PID_FILE="${PROJECT_DIR}/.pids/client_joint_server.pid"
CLIENT1_PID_FILE="${PROJECT_DIR}/.pids/client_joint_1.pid"
CLIENT2_PID_FILE="${PROJECT_DIR}/.pids/client_joint_2.pid"
mkdir -p "${PROJECT_DIR}/logs" "${PROJECT_DIR}/.pids"

cleanup() {
    if [ -f "${CLIENT1_PID_FILE}" ]; then
        kill "$(cat "${CLIENT1_PID_FILE}")" 2>/dev/null || true
        rm -f "${CLIENT1_PID_FILE}"
    fi
    if [ -f "${CLIENT2_PID_FILE}" ]; then
        kill "$(cat "${CLIENT2_PID_FILE}")" 2>/dev/null || true
        rm -f "${CLIENT2_PID_FILE}"
    fi
    if [ -f "${SERVER_PID_FILE}" ]; then
        kill "$(cat "${SERVER_PID_FILE}")" 2>/dev/null || true
        rm -f "${SERVER_PID_FILE}"
    fi
}
trap cleanup EXIT INT TERM

if [ ! -x "${SERVER_BIN}" ] || [ ! -x "${BIN_DIR}/chaos_client" ]; then
    echo -e "${RED}❌ 先编译：./scripts/build_and_test.sh${NC}"
    exit 1
fi

if [ -z "${DISPLAY:-}" ] || [ -z "${XAUTHORITY:-}" ] || [ ! -r "${XAUTHORITY}" ]; then
    echo -e "${RED}❌ 需要有效的 DISPLAY/XAUTHORITY 才能启动 Vulkan 客户端${NC}"
    exit 1
fi

start_server() {
    echo -e "${BLUE}🚀 启动 chaos_server...${NC}"
    "${SERVER_BIN}" --admin >"${SERVER_LOG}" 2>&1 &
    echo $! > "${SERVER_PID_FILE}"
}

start_client() {
    local log_file="$1"
    local pid_file="$2"
    echo -e "${BLUE}🚀 启动 Vulkan 客户端 -> ${log_file##*/}${NC}"
    timeout 12s "${CLIENT_SCRIPT}" --vulkan --connect 127.0.0.1:7777 >"${log_file}" 2>&1 &
    echo $! > "${pid_file}"
}

wait_for_port() {
    local port="$1"
    for _ in $(seq 1 50); do
        if fuser "${port}/tcp" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "============================================"
echo " ChaosEngine 客户端联调"
echo "============================================"

start_server
if ! wait_for_port 7777; then
    echo -e "${RED}❌ chaos_server 7777 未就绪${NC}"
    tail -n 20 "${SERVER_LOG}" || true
    exit 1
fi

echo -e "${GREEN}✅ chaos_server 就绪${NC}"
start_client "${CLIENT1_LOG}" "${CLIENT1_PID_FILE}"
sleep 1
start_client "${CLIENT2_LOG}" "${CLIENT2_PID_FILE}"

wait "$(cat "${CLIENT1_PID_FILE}")" 2>/dev/null || true
wait "$(cat "${CLIENT2_PID_FILE}")" 2>/dev/null || true

echo ""
echo "--- client 1 tail ---"
tail -n 20 "${CLIENT1_LOG}" || true
echo "--- client 2 tail ---"
tail -n 20 "${CLIENT2_LOG}" || true

echo ""
if grep -q "Received ENTITY_UPDATE" "${CLIENT1_LOG}" || grep -q "Received ENTITY_UPDATE" "${CLIENT2_LOG}"; then
    echo -e "${GREEN}✅ 联调完成：客户端收到实体同步${NC}"
else
    echo -e "${YELLOW}⚠️  联调完成，但未观察到实体同步日志${NC}"
fi
