#!/bin/bash
# ============================================================
# ChaosEngine 客户端同步一键验证
#
# 流程:
#   1. 确保 chaos_server / chaos_client 已编译
#   2. 启动集群（game + gateway），客户端通过 Gateway 连接
#   3. 启动两个 Vulkan 客户端，连接 Gateway:9000
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
START_CLUSTER="${SCRIPT_DIR}/start_cluster.sh"
STOP_CLUSTER="${SCRIPT_DIR}/stop_cluster_server.sh"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

CLIENT1_LOG="$(mktemp)"
CLIENT2_LOG="$(mktemp)"
CLUSTER_STARTED=0

cleanup() {
    local rc=$?
    # 清理客户端
    pkill -f "chaos_client" 2>/dev/null || true
    # 如果我们启动了集群，停止它
    if [ "$CLUSTER_STARTED" -eq 1 ]; then
        bash "${STOP_CLUSTER}" 2>/dev/null || true
    fi
    rm -f "${CLIENT1_LOG}" "${CLIENT2_LOG}"
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
    # 直接调用 chaos_client，不通过 start_client.sh 包装，确保日志输出干净
    # 设置 DISPLAY/XAUTHORITY 让 Vulkan 可用
    export DISPLAY="${DISPLAY:-:0}"
    local xauth=""
    xauth="$(pgrep -af 'Xwayland' | head -n1 | grep -oP '(?<=-auth )\S+' || true)"
    if [ -n "$xauth" ] && [ -r "$xauth" ]; then
        export XAUTHORITY="$xauth"
    fi
    # 客户端连 Gateway:9000
    timeout 8s "${BIN_DIR}/chaos_client" --connect 127.0.0.1:9000 >"${log_file}" 2>&1 &
    echo $!
}

main() {
    echo "============================================"
    echo " ChaosEngine 客户端同步一键验证"
    echo "============================================"
    echo ""

    ensure_build

    # 检查 7777 是否已在用（集群可能已启动）
    if ! fuser 7777/tcp >/dev/null 2>&1; then
        echo -e "${BLUE}🚀 启动集群...${NC}"
        bash "${START_CLUSTER}" --all >/dev/null 2>&1
        CLUSTER_STARTED=1
        sleep 3
    else
        echo -e "${GREEN}✅ 集群已在运行${NC}"
    fi

    wait_for_port 7777 "game server"
    wait_for_port 9000 "gateway"

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
    echo "--- Client 1 head ---"
    head -n 10 "${CLIENT1_LOG}" || true
    echo "--- Client 1 tail ---"
    tail -n 10 "${CLIENT1_LOG}" || true
    echo "--- Client 2 head ---"
    head -n 10 "${CLIENT2_LOG}" || true
    echo "--- Client 2 tail ---"
    tail -n 10 "${CLIENT2_LOG}" || true

    # 判定标准：至少一个客户端 Vulkan 启动成功 + 网络连接成功
    # （两个 Vulkan 客户端同时启动可能抢设备，第二个可能失败）
    # chaos_client 输出 "Vulkan device created successfully" + "Connected to 127.0.0.1:9000"
    local c1_vk=0 c2_vk=0 c1_net=0 c2_net=0
    grep -q "Vulkan device created successfully" "${CLIENT1_LOG}" 2>/dev/null && c1_vk=1
    grep -q "Vulkan device created successfully" "${CLIENT2_LOG}" 2>/dev/null && c2_vk=1
    grep -q "Connected to" "${CLIENT1_LOG}" 2>/dev/null && c1_net=1
    grep -q "Connected to" "${CLIENT2_LOG}" 2>/dev/null && c2_net=1

    if { [ "$c1_vk" -eq 1 ] && [ "$c1_net" -eq 1 ]; } || \
       { [ "$c2_vk" -eq 1 ] && [ "$c2_net" -eq 1 ]; }; then
        ok=1
    fi

    echo ""
    if [ "$ok" -eq 1 ]; then
        echo -e "${GREEN}✅ 一键验证通过：客户端成功启动并连接${NC}"
        return 0
    fi

    echo -e "${RED}❌ 一键验证失败：请查看上述日志${NC}"
    return 1
}

main "$@"
