#!/usr/bin/env bash
# ============================================================
# ChaosEngine io_uring vs POSIX 异步 I/O 性能对比基准测试
#
# 用法:
#   ./scripts/bench_async_io.sh              # 运行全部测试
#   ./scripts/bench_async_io.sh --quick       # 快速测试（减少迭代）
#   ./scripts/bench_async_io.sh --uring-only  # 仅测试 io_uring
#   ./scripts/bench_async_io.sh --posix-only  # 仅测试 POSIX
#
# 测试场景:
#   a. 单连接 echo 延迟（ping-pong 10000 次）
#   b. 100 并发连接 echo QPS
#   c. 1000 并发连接稳定性（运行 60s 无崩溃）
#   d. CPU 利用率对比（io_uring vs POSIX）
#
# 依赖:
#   - cmake, make, gcc
#   - liburing-dev (io_uring 后端)
#   - python3 (测试客户端)
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
BUILD_POSIX_DIR="$PROJECT_DIR/build_posix"
RESULTS_DIR="$PROJECT_DIR/bench_results"
BENCH_CLIENT="$SCRIPT_DIR/bench_async_io.py"
BENCH_PORT=7778

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# 测试参数（可通过命令行覆盖）
PINGS=10000
QPS_CONNS=100
QPS_DURATION=30
QPS_PAYLOAD=64
STABILITY_CONNS=1000
STABILITY_DURATION=60
STABILITY_PAYLOAD=64

# 模式选择
RUN_URING=true
RUN_POSIX=true
QUICK_MODE=false

# ============================================================
# 参数解析
# ============================================================

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            QUICK_MODE=true
            PINGS=2000
            QPS_CONNS=50
            QPS_DURATION=10
            STABILITY_CONNS=200
            STABILITY_DURATION=15
            ;;
        --uring-only)
            RUN_POSIX=false
            ;;
        --posix-only)
            RUN_URING=false
            ;;
        --port)
            BENCH_PORT="$2"
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --quick        Quick test mode (reduced iterations)"
            echo "  --uring-only   Only test io_uring backend"
            echo "  --posix-only   Only test POSIX backend"
            echo "  --port PORT    Server port (default: 7778)"
            echo "  --help         Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
    shift
done

# ============================================================
# 工具函数
# ============================================================

log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "\n${BOLD}${BLUE}=== $* ===${NC}"; }

check_deps() {
    local missing=()
    for cmd in cmake make gcc python3; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing dependencies: ${missing[*]}"
        exit 1
    fi
    log_ok "All build dependencies found"
}

kill_server() {
    local pid_file="$1"
    if [[ -f "$pid_file" ]]; then
        local pid
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            # 等待进程退出
            for i in {1..30}; do
                if ! kill -0 "$pid" 2>/dev/null; then
                    break
                fi
                sleep 0.1
            done
            # 强制 kill
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
        rm -f "$pid_file"
    fi
}

wait_for_server() {
    local port="$1"
    local timeout="${2:-10}"
    local elapsed=0
    while [[ $elapsed -lt $timeout ]]; do
        if python3 -c "
import socket
s = socket.socket()
s.settimeout(1)
try:
    s.connect(('127.0.0.1', $port))
    s.close()
    exit(0)
except:
    exit(1)
" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    return 1
}

start_server() {
    local binary="$1"
    local port="$2"
    local pid_file="$3"
    local label="$4"

    log_info "Starting $label server on port $port..."
    CHAOS_BENCH_PORT="$port" CHAOS_BENCH_QUIET=1 "$binary" &
    local pid=$!
    echo "$pid" > "$pid_file"

    if wait_for_server "$port" 15; then
        log_ok "$label server ready (pid=$pid)"
        return 0
    else
        log_error "$label server failed to start"
        kill_server "$pid_file"
        return 1
    fi
}

run_bench() {
    local mode="$1"
    local test_type="$2"
    local extra_args="$3"
    local output_file="$4"

    log_info "Running $test_type benchmark ($mode)..."
    python3 "$BENCH_CLIENT" \
        --mode "$mode" \
        --host 127.0.0.1 \
        --port "$BENCH_PORT" \
        $extra_args \
        --output "$output_file" \
        --format json \
        2>&1 | tail -20
}

measure_cpu() {
    local pid="$1"
    local duration="$2"
    local output_file="$3"

    log_info "Measuring CPU usage for $duration seconds..."

    # 使用 ps 采样 CPU 利用率
    local samples=()
    local interval=1
    local max_samples=$duration

    for ((i=0; i<max_samples; i++)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        local cpu
        cpu=$(ps -p "$pid" -o %cpu= 2>/dev/null | tr -d ' ' || echo "0")
        samples+=("$cpu")
        sleep "$interval"
    done

    # 计算平均值
    local sum=0
    local count=${#samples[@]}
    for v in "${samples[@]}"; do
        sum=$(echo "$sum + $v" | bc -l 2>/dev/null || echo "$sum")
    done
    local avg
    if [[ $count -gt 0 ]]; then
        avg=$(echo "scale=2; $sum / $count" | bc -l 2>/dev/null || echo "0")
    else
        avg="0"
    fi

    echo "{\"cpu_avg_percent\": $avg, \"samples\": $count}" > "$output_file"
    log_info "CPU avg: ${avg}% (${count} samples)"
}

# ============================================================
# 编译
# ============================================================

build_uring() {
    log_step "Building io_uring benchmark server"

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    cmake "$PROJECT_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCHAOS_BUILD_EDITOR=OFF \
        -DCHAOS_BUILD_TESTS=OFF \
        -DBUILD_POSIX_BENCH=OFF \
        > /dev/null 2>&1

    cmake --build . --target chaos_async_bench -j"$(nproc)" 2>&1 | tail -5

    if [[ -f "$BUILD_DIR/bin/chaos_async_bench" ]]; then
        log_ok "chaos_async_bench built successfully"
        echo "$BUILD_DIR/bin/chaos_async_bench"
    else
        log_error "Failed to build chaos_async_bench"
        return 1
    fi
}

build_posix() {
    log_step "Building POSIX benchmark server"

    mkdir -p "$BUILD_POSIX_DIR"
    cd "$BUILD_POSIX_DIR"

    cmake "$PROJECT_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCHAOS_BUILD_EDITOR=OFF \
        -DCHAOS_BUILD_TESTS=OFF \
        -DBUILD_POSIX_BENCH=ON \
        > /dev/null 2>&1

    cmake --build . --target chaos_async_bench_posix -j"$(nproc)" 2>&1 | tail -5

    if [[ -f "$BUILD_POSIX_DIR/bin/chaos_async_bench_posix" ]]; then
        log_ok "chaos_async_bench_posix built successfully"
        echo "$BUILD_POSIX_DIR/bin/chaos_async_bench_posix"
    else
        log_error "Failed to build chaos_async_bench_posix"
        return 1
    fi
}

# ============================================================
# 测试执行
# ============================================================

run_all_tests() {
    local mode="$1"
    local binary="$2"
    local pid_file="$RESULTS_DIR/server_${mode}.pid"

    log_step "Running $mode benchmark suite"

    # 启动服务器
    if ! start_server "$binary" "$BENCH_PORT" "$pid_file" "$mode"; then
        return 1
    fi

    local server_pid
    server_pid=$(cat "$pid_file")

    # --- 测试 A: 单连接延迟 ---
    run_bench "$mode" "latency" \
        "--latency --pings $PINGS --payload 64" \
        "$RESULTS_DIR/${mode}_latency.json"

    # --- 测试 B: 多连接 QPS ---
    run_bench "$mode" "QPS" \
        "--connections $QPS_CONNS --duration $QPS_DURATION --payload $QPS_PAYLOAD" \
        "$RESULTS_DIR/${mode}_qps.json"

    # --- 测试 C: 稳定性 ---
    run_bench "$mode" "stability" \
        "--stability --connections $STABILITY_CONNS --duration $STABILITY_DURATION --payload $STABILITY_PAYLOAD" \
        "$RESULTS_DIR/${mode}_stability.json"

    # --- 测试 D: CPU 利用率 ---
    # 启动一个中等负载（100 连接），同时测量 CPU
    log_info "Starting CPU measurement with 100 connections..."
    python3 "$BENCH_CLIENT" \
        --mode "$mode" \
        --host 127.0.0.1 \
        --port "$BENCH_PORT" \
        --connections 100 \
        --duration 15 \
        --payload 64 \
        --output "$RESULTS_DIR/${mode}_cpu_load.json" \
        --format json &
    local bench_pid=$!

    sleep 2  # 等待连接建立
    measure_cpu "$server_pid" 10 "$RESULTS_DIR/${mode}_cpu.json"
    wait "$bench_pid" 2>/dev/null || true

    # 停止服务器
    kill_server "$pid_file"
    log_ok "$mode benchmark suite complete"
}

# ============================================================
# 生成报告
# ============================================================

generate_report() {
    log_step "Generating comparison report"

    local report="$RESULTS_DIR/bench_report.md"

    # 报告头部
    {
        echo "# ChaosEngine io_uring vs POSIX 异步 I/O 性能对比报告"
        echo ""
        echo "**生成时间:** $(date)"
        echo ""
        echo "---"
        echo ""
        echo "## 测试环境"
        echo ""
        echo "- **Host:** $(hostname)"
        echo "- **Kernel:** $(uname -r)"
        echo "- **CPU:** $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo 'unknown')"
        echo "- **CPU Cores:** $(nproc)"
        echo "- **Memory:** $(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo 'unknown')"
        if pkg-config --exists liburing 2>/dev/null; then
            echo "- **liburing:** $(pkg-config --modversion liburing)"
        fi
        echo ""
        echo "## 测试参数"
        echo ""
        echo "| 参数 | 值 |"
        echo "|------|----|"
        echo "| 延迟测试 Ping 数 | $PINGS |"
        echo "| QPS 测试连接数 | $QPS_CONNS |"
        echo "| QPS 测试时长 | ${QPS_DURATION}s |"
        echo "| QPS 测试载荷 | ${QPS_PAYLOAD} bytes |"
        echo "| 稳定性测试连接数 | $STABILITY_CONNS |"
        echo "| 稳定性测试时长 | ${STABILITY_DURATION}s |"
        echo ""
    } > "$report"

    # --- 延迟对比 ---
    echo "## 1. 单连接延迟 (Ping-Pong)" >> "$report"
    echo "" >> "$report"
    echo "| 指标 | io_uring | POSIX | 差异 |" >> "$report"
    echo "|------|----------|-------|------|" >> "$report"

    for metric in p50 p90 p99 avg min max; do
        local uring_val posix_val diff
        uring_val=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/uring_latency.json') as f:
        d = json.load(f)
    print(d[0]['latency']['$metric'])
except: print('N/A')
" 2>/dev/null || echo "N/A")

        posix_val=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/posix_latency.json') as f:
        d = json.load(f)
    print(d[0]['latency']['$metric'])
except: print('N/A')
" 2>/dev/null || echo "N/A")

        if [[ "$uring_val" != "N/A" && "$posix_val" != "N/A" ]]; then
            diff=$(python3 -c "
u = float($uring_val)
p = float($posix_val)
if p > 0:
    d = (u - p) / p * 100
    print(f'{d:+.1f}%')
else:
    print('N/A')
" 2>/dev/null || echo "N/A")
        else
            diff="N/A"
        fi
        echo "| ${metric^^} | ${uring_val} µs | ${posix_val} µs | ${diff} |" >> "$report"
    done
    echo "" >> "$report"

    # --- QPS 对比 ---
    echo "## 2. 并发 QPS ($QPS_CONNS 连接)" >> "$report"
    echo "" >> "$report"
    echo "| 指标 | io_uring | POSIX | 差异 |" >> "$report"
    echo "|------|----------|-------|------|" >> "$report"

    for metric in qps throughput_mbps total_requests errors; do
        local uring_val posix_val diff
        uring_val=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/uring_qps.json') as f:
        d = json.load(f)
    print(d[0].get('$metric', 'N/A'))
except: print('N/A')
" 2>/dev/null || echo "N/A")

        posix_val=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/posix_qps.json') as f:
        d = json.load(f)
    print(d[0].get('$metric', 'N/A'))
except: print('N/A')
" 2>/dev/null || echo "N/A")

        if [[ "$uring_val" != "N/A" && "$posix_val" != "N/A" ]]; then
            diff=$(python3 -c "
u = float($uring_val)
p = float($posix_val)
if p > 0:
    d = (u - p) / p * 100
    print(f'{d:+.1f}%')
else:
    print('N/A')
" 2>/dev/null || echo "N/A")
        else
            diff="N/A"
        fi

        local label="$metric"
        case "$metric" in
            qps) label="QPS (req/s)" ;;
            throughput_mbps) label="吞吐量 (MB/s)" ;;
            total_requests) label="总请求数" ;;
            errors) label="错误数" ;;
        esac
        echo "| $label | $uring_val | $posix_val | $diff |" >> "$report"
    done
    echo "" >> "$report"

    # --- 稳定性对比 ---
    echo "## 3. 稳定性 ($STABILITY_CONNS 连接, ${STABILITY_DURATION}s)" >> "$report"
    echo "" >> "$report"
    echo "| 指标 | io_uring | POSIX |" >> "$report"
    echo "|------|----------|-------|" >> "$report"

    for metric in qps total_requests errors final_alive; do
        local uring_val posix_val
        uring_val=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/uring_stability.json') as f:
        d = json.load(f)
    print(d[0].get('$metric', 'N/A'))
except: print('N/A')
" 2>/dev/null || echo "N/A")

        posix_val=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/posix_stability.json') as f:
        d = json.load(f)
    print(d[0].get('$metric', 'N/A'))
except: print('N/A')
" 2>/dev/null || echo "N/A")

        local label="$metric"
        case "$metric" in
            qps) label="QPS (req/s)" ;;
            total_requests) label="总请求数" ;;
            errors) label="错误数" ;;
            final_alive) label="最终存活连接" ;;
        esac
        echo "| $label | $uring_val | $posix_val |" >> "$report"
    done
    echo "" >> "$report"

    # --- CPU 利用率对比 ---
    echo "## 4. CPU 利用率 (100 连接负载)" >> "$report"
    echo "" >> "$report"
    echo "| 指标 | io_uring | POSIX |" >> "$report"
    echo "|------|----------|-------|" >> "$report"

    local uring_cpu posix_cpu
    uring_cpu=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/uring_cpu.json') as f:
        d = json.load(f)
    print(d.get('cpu_avg_percent', 'N/A'))
except: print('N/A')
" 2>/dev/null || echo "N/A")

    posix_cpu=$(python3 -c "
import json
try:
    with open('$RESULTS_DIR/posix_cpu.json') as f:
        d = json.load(f)
    print(d.get('cpu_avg_percent', 'N/A'))
except: print('N/A')
" 2>/dev/null || echo "N/A")

    echo "| CPU 平均利用率 | ${uring_cpu}% | ${posix_cpu}% |" >> "$report"
    echo "" >> "$report"

    # --- 总结 ---
    echo "## 5. 总结" >> "$report"
    echo "" >> "$report"

    python3 -c "
import json, os

def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except: return None

uring_lat = load_json('$RESULTS_DIR/uring_latency.json')
posix_lat = load_json('$RESULTS_DIR/posix_latency.json')
uring_qps = load_json('$RESULTS_DIR/uring_qps.json')
posix_qps = load_json('$RESULTS_DIR/posix_qps.json')
uring_cpu = load_json('$RESULTS_DIR/uring_cpu.json')
posix_cpu = load_json('$RESULTS_DIR/posix_cpu.json')

lines = []

if uring_lat and posix_lat:
    u_p50 = uring_lat[0]['latency']['p50']
    p_p50 = posix_lat[0]['latency']['p50']
    if p_p50 > 0:
        delta = (u_p50 - p_p50) / p_p50 * 100
        lines.append(f'- **延迟 P50:** io_uring 比 POSIX {delta:+.1f}%')

if uring_qps and posix_qps:
    u_qps = uring_qps[0].get('qps', 0)
    p_qps = posix_qps[0].get('qps', 0)
    if p_qps > 0:
        delta = (u_qps - p_qps) / p_qps * 100
        lines.append(f'- **QPS:** io_uring 比 POSIX {delta:+.1f}%')

if uring_cpu and posix_cpu:
    u_cpu = uring_cpu.get('cpu_avg_percent', 0)
    p_cpu = posix_cpu.get('cpu_avg_percent', 0)
    if p_cpu > 0:
        delta = (u_cpu - p_cpu) / p_cpu * 100
        lines.append(f'- **CPU 利用率:** io_uring 比 POSIX {delta:+.1f}%')

if lines:
    for l in lines:
        print(l)
else:
    print('_数据不足，无法生成总结_')
" >> "$report"

    echo "" >> "$report"
    echo "---" >> "$report"
    echo "*报告由 scripts/bench_async_io.sh 自动生成*" >> "$report"

    log_ok "Report saved to $report"
    echo ""
    cat "$report"
}

# ============================================================
# 主流程
# ============================================================

main() {
    echo -e "${BOLD}${BLUE}"
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║  ChaosEngine io_uring vs POSIX Benchmark Suite      ║"
    echo "╚══════════════════════════════════════════════════════╝"
    echo -e "${NC}"

    if $QUICK_MODE; then
        log_warn "Quick mode: reduced test parameters"
    fi

    # 检查依赖
    check_deps

    # 创建结果目录
    mkdir -p "$RESULTS_DIR"

    # 编译
    local uring_bin=""
    local posix_bin=""

    if $RUN_URING; then
        uring_bin=$(build_uring) || {
            log_error "io_uring build failed, skipping io_uring tests"
            RUN_URING=false
        }
    fi

    if $RUN_POSIX; then
        posix_bin=$(build_posix) || {
            log_error "POSIX build failed, skipping POSIX tests"
            RUN_POSIX=false
        }
    fi

    # 运行测试
    if $RUN_URING && [[ -n "$uring_bin" ]]; then
        run_all_tests "uring" "$uring_bin" || log_warn "Some io_uring tests may have failed"
    fi

    if $RUN_POSIX && [[ -n "$posix_bin" ]]; then
        run_all_tests "posix" "$posix_bin" || log_warn "Some POSIX tests may have failed"
    fi

    # 生成报告
    if $RUN_URING && $RUN_POSIX; then
        generate_report
    elif $RUN_URING; then
        log_info "Only io_uring tested — skipping comparison report"
        log_info "Results in: $RESULTS_DIR/"
        ls -la "$RESULTS_DIR/"uring_*.json 2>/dev/null || true
    elif $RUN_POSIX; then
        log_info "Only POSIX tested — skipping comparison report"
        log_info "Results in: $RESULTS_DIR/"
        ls -la "$RESULTS_DIR/"posix_*.json 2>/dev/null || true
    fi

    echo ""
    log_ok "Benchmark suite complete!"
    echo "  Results directory: $RESULTS_DIR"
    if [[ -f "$RESULTS_DIR/bench_report.md" ]]; then
        echo "  Report: $RESULTS_DIR/bench_report.md"
    fi
}

main "$@"
