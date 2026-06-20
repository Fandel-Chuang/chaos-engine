#!/bin/bash
# ============================================================
# ChaosEngine 客户端同步一键验证
#
# 流程:
#   1. 确保 chaos_server / chaos_client 已编译
#   2. 启动 chaos_server
#   3. 启动两个 Vulkan 客户端，直接连接 Game Server:7777
#   4. 检查客户端日志是否收到 ENTITY_UPDATE
#   5. 清理进程并输出结果
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
SERVER_BIN="${BIN_DIR}/chaos_server"
CLIENT_START="${SCRIPT_DIR}/start_client.sh"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SERVER_LOG="$(mktemp)"
CLIENT1_LOG="$(mktemp)"
CLIENT2_LOG="$(mktemp)"
SERVER_PID=""

cleanup() {
    local rc=$?
    if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -f "${SERVER_LOG}" "${CLIENT1_LOG}" "${CLIENT2_LOG}"
    exit "$rc"
}
trap cleanup EXIT INT TERM

wait_for_port() {
    local port="$1"
    local name="$2"
    for _ in $(seq 1 50); do
        if fuser "${port}/tcp" >/dev/null 2>&1; then
            echo -e "${GREEN}✅ ${name}(${port}) ready${NC}"
            return 0
        fi
        sleep 0.2
    done
    echo -e "${RED}❌ ${name}(${port}) not ready${NC}"
    return 1
}

ensure_build() {
    if [ ! -x "${SERVER_BIN}" ] || [ ! -x "${BIN_DIR}/chaos_client" ]; then
        echo -e "${BLUE}🔧 Building targets...${NC}"
        cmake --build "${BUILD_DIR}" --target chaos_server chaos_client -j2
    fi
}

start_client() {
    local log_file="$1"
    timeout 8s "${CLIENT_START}" --vulkan --connect 127.0.0.1:7777 >"${log_file}" 2>&1 &
    echo $!
}

main() {
    echo "============================================"
    echo " ChaosEngine 客户端同步一键验证"
    echo "============================================"
    echo ""

    ensure_build

    echo -e "${BLUE}🚀 启动 chaos_server...${NC}"
    "${SERVER_BIN}" --admin >"${SERVER_LOG}" 2>&1 &
    SERVER_PID=$!

    wait_for_port 7777 "chaos_server"

    echo -e "${BLUE}🚀 启动客户端 1...${NC}"
    local c1_pid
    c1_pid="$(start_client "${CLIENT1_LOG}")"
    sleep 1

    echo -e "${BLUE}🚀 启动客户端 2...${NC}"
    local c2_pid
    c2_pid="$(start_client "${CLIENT2_LOG}")"

    wait "${c1_pid}" 2>/dev/null || true
    wait "${c2_pid}" 2>/dev/null || true

    local ok=0
    echo ""
    echo "--- Client 1 tail ---"
    tail -n 20 "${CLIENT1_LOG}" || true
    echo "--- Client 2 tail ---"
    tail -n 20 "${CLIENT2_LOG}" || true

    if grep -q "Vulkan device created successfully" "${CLIENT1_LOG}" && \
       grep -q "Vulkan device created successfully" "${CLIENT2_LOG}" && \
       { grep -q "Received ENTITY_UPDATE" "${CLIENT1_LOG}" || grep -q "Received ENTITY_UPDATE" "${CLIENT2_LOG}"; }; then
        ok=1
    fi

    echo ""
    if [ "$ok" -eq 1 ]; then
        echo -e "${GREEN}✅ 一键验证通过：客户端成功启动并收到实体同步${NC}"
        return 0
    fi

    echo -e "${RED}❌ 一键验证失败：请查看上述日志${NC}"
    echo "--- Server tail ---"
    tail -n 40 "${SERVER_LOG}" || true
    return 1
}

main "$@"
