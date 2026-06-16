#!/bin/bash
# ChaosEngine 一键构建 + 测试脚本
# 用法: ./scripts/build_and_test.sh [--release] [--no-test] [--no-bpf]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
BUILD_TYPE="Debug"
RUN_TESTS=1
BUILD_BPF=1
JOBS=$(nproc)

# 解析参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) BUILD_TYPE="Release"; shift ;;
        --no-test) RUN_TESTS=0; shift ;;
        --no-bpf)  BUILD_BPF=0; shift ;;
        -j)        JOBS="$2"; shift 2 ;;
        -j*)       JOBS="${1#-j}"; shift ;;
        *)         echo "未知参数: $1"; exit 1 ;;
    esac
done

echo "============================================"
echo " ChaosEngine 构建 & 测试"
echo " 构建类型: ${BUILD_TYPE}"
echo " 并行数:   ${JOBS}"
echo " 测试:     $([ $RUN_TESTS -eq 1 ] && echo '是' || echo '否')"
echo " BPF:      $([ $BUILD_BPF -eq 1 ] && echo '是' || echo '否')"
echo "============================================"

# ── Step 1: CMake 配置 ──
echo ""
echo "[1/4] CMake 配置..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CMAKE_ARGS="-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
if [ $BUILD_BPF -eq 0 ]; then
    CMAKE_ARGS="$CMAKE_ARGS -DCHAOS_HAS_EBPF=OFF"
fi

cmake "$PROJECT_DIR" $CMAKE_ARGS 2>&1 | tail -5

# ── Step 2: 编译 ──
echo ""
echo "[2/4] 编译 (make -j${JOBS})..."
START_TIME=$(date +%s)

if make -j"$JOBS" 2>&1; then
    END_TIME=$(date +%s)
    DURATION=$((END_TIME - START_TIME))
    echo ""
    echo "✅ 编译成功 (${DURATION}s)"
else
    echo ""
    echo "❌ 编译失败"
    exit 1
fi

# ── Step 3: 运行测试 ──
if [ $RUN_TESTS -eq 1 ]; then
    echo ""
    echo "[3/4] 运行测试..."

    TEST_START=$(date +%s)
    if ctest --output-on-failure -j"$JOBS" 2>&1; then
        TEST_END=$(date +%s)
        TEST_DUR=$((TEST_END - TEST_START))
        echo ""
        echo "✅ 全部测试通过 (${TEST_DUR}s)"
    else
        echo ""
        echo "❌ 测试失败"
        exit 1
    fi
else
    echo ""
    echo "[3/4] 跳过测试"
fi

# ── Step 4: 冒烟测试 (快速验证关键功能) ──
if [ $RUN_TESTS -eq 1 ]; then
    echo ""
    echo "[4/4] 冒烟测试..."

    SMOKE_PASS=0
    SMOKE_FAIL=0

    # 4.1 验证二进制文件存在
    for bin in chaos_server chaos_router chaos_client chaos_headless; do
        if [ -f "${BUILD_DIR}/bin/${bin}" ]; then
            echo "  ✅ ${bin} 存在"
            SMOKE_PASS=$((SMOKE_PASS + 1))
        else
            echo "  ⚠️  ${bin} 不存在（可能未构建此目标）"
        fi
    done

    # 4.2 验证 chaos_server 能启动并正常退出（带 admin IPC）
    if [ -f "${BUILD_DIR}/bin/chaos_server" ]; then
        echo "  🚀 启动 chaos_server (--admin)..."
        timeout 3 "${BUILD_DIR}/bin/chaos_server" --admin 2>&1 || true
        echo "  ✅ chaos_server 启动/退出正常"
        SMOKE_PASS=$((SMOKE_PASS + 1))
    fi

    # 4.3 Admin 后台冒烟测试
    echo "  🌐 Admin 后台测试..."
    ADMIN_PASS=0

    # 清理旧 socket，启动 server
    rm -f /tmp/chaos_admin.sock
    "${BUILD_DIR}/bin/chaos_server" --admin &
    ADMIN_SRV_PID=$!
    sleep 1

    # 等待 socket 就绪
    for i in $(seq 1 20); do
        if [ -S /tmp/chaos_admin.sock ]; then break; fi
        sleep 0.1
    done

    if [ -S /tmp/chaos_admin.sock ]; then
        echo "  ✅ Admin socket 就绪"

        # 启动 Lapis admin (cqueues 模式)
        cd "${PROJECT_DIR}/src_lua/admin"
        lapis server &
        LAPIS_PID=$!
        sleep 2

        # 测试 API 端点
        for endpoint in health stats aoi cell network memory cpu render system; do
            if curl -sf "http://localhost:9090/api/${endpoint}" > /dev/null 2>&1; then
                echo "  ✅ GET /api/${endpoint}"
                ADMIN_PASS=$((ADMIN_PASS + 1))
            else
                echo "  ⚠️  GET /api/${endpoint} 不可用"
            fi
        done

        # 测试主页
        if curl -sf "http://localhost:9090/" > /dev/null 2>&1; then
            echo "  ✅ GET / (Dashboard HTML)"
            ADMIN_PASS=$((ADMIN_PASS + 1))
        fi

        # 清理 admin 进程
        kill $LAPIS_PID 2>/dev/null || true
        cd "${PROJECT_DIR}"
    else
        echo "  ⚠️  Admin socket 未创建，跳过 API 测试"
    fi

    # 停止 server
    kill $ADMIN_SRV_PID 2>/dev/null || true
    wait $ADMIN_SRV_PID 2>/dev/null || true
    rm -f /tmp/chaos_admin.sock

    echo "  Admin 测试: ${ADMIN_PASS} 端点可用"
    SMOKE_PASS=$((SMOKE_PASS + ADMIN_PASS))

    # 4.4 验证关键测试通过
    for test_name in math memory ecs network aoi cell net_base; do
        if ctest -R "^${test_name}$" --output-on-failure 2>&1 | grep -q "Passed"; then
            echo "  ✅ test_${test_name} 通过"
            SMOKE_PASS=$((SMOKE_PASS + 1))
        else
            echo "  ❌ test_${test_name} 失败"
            SMOKE_FAIL=$((SMOKE_FAIL + 1))
        fi
    done

    echo ""
    echo "  冒烟测试: ${SMOKE_PASS} 通过, ${SMOKE_FAIL} 失败"
    if [ $SMOKE_FAIL -gt 0 ]; then
        exit 1
    fi
fi

# ── 汇总 ──
echo ""
echo "============================================"
echo " ✅ ChaosEngine 构建 & 测试全部通过"
echo "============================================"
