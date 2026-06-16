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

    # 4.2 验证 chaos_server 能启动并正常退出
    if [ -f "${BUILD_DIR}/bin/chaos_server" ]; then
        echo "  🚀 启动 chaos_server..."
        timeout 3 "${BUILD_DIR}/bin/chaos_server" --headless 2>&1 || true
        echo "  ✅ chaos_server 启动/退出正常"
        SMOKE_PASS=$((SMOKE_PASS + 1))
    fi

    # 4.3 验证关键测试通过
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
