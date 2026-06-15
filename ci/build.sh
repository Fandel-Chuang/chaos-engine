#!/usr/bin/env bash
#
# ci/build.sh — ChaosEngine 本地 CI 构建脚本
#
# 一键构建流程：
#   1. 运行 CI 规则检查（跨目录引用、C++ 语法侵入）
#   2. CMake 配置
#   3. 编译
#   4. 运行测试（如果启用）
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo ""
    echo -e "${BLUE}============================================================${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}============================================================${NC}"
    echo ""
}

print_pass() {
    echo -e "  ${GREEN}[PASS]${NC} $1"
}

print_fail() {
    echo -e "  ${RED}[FAIL]${NC} $1"
}

print_info() {
    echo -e "  ${YELLOW}[INFO]${NC} $1"
}

# ============================================================
# Step 1: CI 规则检查
# ============================================================
print_header "Step 1: CI Rule Checks"

RULES_DIR="${PROJECT_ROOT}/ci/rules"
ALL_RULES_PASSED=true

# 1a. 跨目录引用检测
print_info "Running check_cross_ref.py..."
if python3 "${RULES_DIR}/check_cross_ref.py"; then
    print_pass "check_cross_ref.py"
else
    print_fail "check_cross_ref.py"
    ALL_RULES_PASSED=false
fi

# 1b. C++ 语法侵入检测
print_info "Running check_cpp_in_c.py..."
if python3 "${RULES_DIR}/check_cpp_in_c.py"; then
    print_pass "check_cpp_in_c.py"
else
    print_fail "check_cpp_in_c.py"
    ALL_RULES_PASSED=false
fi

if [ "$ALL_RULES_PASSED" = false ]; then
    echo ""
    print_fail "CI rules check failed. Aborting build."
    exit 1
fi

echo ""
print_pass "All CI rules passed!"

# ============================================================
# Step 2: CMake 配置
# ============================================================
print_header "Step 2: CMake Configuration"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

print_info "Running cmake .. -DCHAOS_BUILD_EDITOR=ON -DCHAOS_BUILD_TESTS=ON"
if cmake .. -DCHAOS_BUILD_EDITOR=ON -DCHAOS_BUILD_TESTS=ON; then
    print_pass "CMake configuration"
else
    print_fail "CMake configuration"
    exit 1
fi

# ============================================================
# Step 3: 编译
# ============================================================
print_header "Step 3: Build"

print_info "Running cmake --build ."
if cmake --build .; then
    print_pass "Build succeeded"
else
    print_fail "Build failed"
    exit 1
fi

# 列出构建产物
echo ""
print_info "Build artifacts:"
ls -la "${BUILD_DIR}/bin/" 2>/dev/null || print_info "No binaries in bin/"
ls -la "${BUILD_DIR}/lib/" 2>/dev/null || print_info "No libraries in lib/"

# ============================================================
# Step 4: 运行测试
# ============================================================
print_header "Step 4: Run Tests"

if [ -f "${BUILD_DIR}/tests/CTestTestfile.cmake" ]; then
    print_info "Running ctest..."
    cd "${BUILD_DIR}"
    if ctest --output-on-failure; then
        print_pass "All tests passed"
    else
        print_fail "Some tests failed"
        exit 1
    fi
else
    print_info "No tests configured (CHAOS_BUILD_TESTS may be OFF)"
fi

# ============================================================
# 完成
# ============================================================
print_header "CI Build Complete"
echo -e "  ${GREEN}All steps completed successfully!${NC}"
echo ""

exit 0
