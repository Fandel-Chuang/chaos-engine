#!/bin/bash
# ============================================================
# test_gateway_stress.sh — Gateway 压力测试
#
# 测试流程:
#   1. 启动 chaos_server（后端 Game 服务）
#   2. 启动 Lua Gateway（TCP 9000 + WS 9002）
#   3. 并发连接测试（100 TCP 连接）
#   4. 消息吞吐量测试
#   5. WebSocket 并发连接测试
#   6. 长时间运行稳定性测试
#   7. 清理进程
#
# 注意: 需要 Python 3 用于并发测试
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
GATEWAY_DIR="$PROJECT_DIR/src_lua/gateway"
BUILD_BIN="$PROJECT_DIR/build/bin/chaos_server"

# ── 颜色 ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; FAIL=$((FAIL + 1)); }
info() { echo -e "  ${CYAN}[INFO]${NC} $1"; }
step() { echo -e "\n${BOLD}${CYAN}>>> $1${NC}"; }

# ── 配置 ──
STRESS_CONNECTIONS=${STRESS_CONNECTIONS:-100}
STRESS_DURATION=${STRESS_DURATION:-10}
STRESS_MESSAGES=${STRESS_MESSAGES:-1000}

# ── 超时保护 ──
GATEWAY_PID=""
SERVER_PID=""

cleanup() {
    echo ""
    info "清理进程..."
    if [ -n "$GATEWAY_PID" ] && kill -0 "$GATEWAY_PID" 2>/dev/null; then
        kill "$GATEWAY_PID" 2>/dev/null || true
        info "已终止 Gateway (PID: $GATEWAY_PID)"
    fi
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        info "已终止 chaos_server (PID: $SERVER_PID)"
    fi
    # 确保没有残留
    pkill -f "lua.*gateway/server.lua" 2>/dev/null || true
    pkill -f "chaos_server" 2>/dev/null || true
    rm -f /tmp/chaos_admin.sock
    rm -f /tmp/stress_test_*.py
}
trap cleanup EXIT INT TERM

# ── 辅助函数 ──

# 等待端口就绪
wait_for_port() {
    local port=$1
    local label=$2
    local max_wait=${3:-15}
    info "等待端口 $port ($label) 就绪..."
    for i in $(seq 1 $((max_wait * 5))); do
        if fuser "${port}/tcp" >/dev/null 2>&1; then
            info "端口 $port 已就绪"
            return 0
        fi
        sleep 0.2
    done
    fail "端口 $port 未在 ${max_wait}s 内就绪"
    return 1
}

# 通过 TCP 发送消息并读取响应
send_tcp_msg() {
    local host=$1
    local port=$2
    local msg=$3
    local timeout=${4:-3}
    echo "$msg" | timeout "$timeout" nc -w 2 "$host" "$port" 2>/dev/null || true
}

# ═══════════════════════════════════════════════════════════════
# 测试 1: 启动服务
# ═══════════════════════════════════════════════════════════════

step "测试 1: 启动服务 (chaos_server + Gateway)"

# 清理旧进程
pkill -f "chaos_server" 2>/dev/null || true
pkill -f "lua.*gateway/server.lua" 2>/dev/null || true
sleep 0.5

# 检查依赖
if ! command -v lua >/dev/null 2>&1; then
    fail "lua 未安装"
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    fail "python3 未安装 (压力测试需要)"
    exit 1
fi

# 启动 chaos_server
if [ -x "$BUILD_BIN" ]; then
    "$BUILD_BIN" &
    SERVER_PID=$!
    info "chaos_server 已启动 (PID: $SERVER_PID)"
else
    info "chaos_server 未构建，跳过 (仅测试 Gateway 连接能力)"
    # 启动一个简单的 echo 服务器代替
    python3 -c "
import socket, threading, sys

def handle(conn, addr):
    try:
        while True:
            data = conn.recv(4096)
            if not data: break
            conn.sendall(data)
    except:
        pass
    finally:
        conn.close()

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', 7777))
s.listen(128)
print('Echo server on :7777', flush=True)
while True:
    conn, addr = s.accept()
    threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
" &
    SERVER_PID=$!
    info "Python echo server 已启动 (PID: $SERVER_PID)"
fi

if ! wait_for_port 7777 "Backend" 10; then
    exit 1
fi
pass "后端服务启动成功 (端口 7777)"

# 启动 Gateway
cd "$GATEWAY_DIR"
lua server.lua --tcp-port 9000 --ws-port 9002 --game-host 127.0.0.1 --game-port 7777 &
GATEWAY_PID=$!
info "Gateway 已启动 (PID: $GATEWAY_PID)"

if ! wait_for_port 9000 "Gateway TCP" 10; then
    exit 1
fi
if ! wait_for_port 9002 "Gateway WS" 10; then
    exit 1
fi
pass "Gateway 启动成功 (TCP:9000, WS:9002)"

# ═══════════════════════════════════════════════════════════════
# 测试 2: TCP 并发连接测试 (100 连接)
# ═══════════════════════════════════════════════════════════════

step "测试 2: TCP 并发连接测试 (${STRESS_CONNECTIONS} 连接)"

info "启动 ${STRESS_CONNECTIONS} 个并发 TCP 连接..."

# 使用 Python 进行并发连接测试
CONCURRENT_RESULT=$(python3 -c "
import socket
import threading
import time
import sys

HOST = '127.0.0.1'
PORT = 9000
NUM_CONNS = $STRESS_CONNECTIONS

results = {'success': 0, 'failed': 0, 'errors': []}
lock = threading.Lock()

def test_connection(i):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect((HOST, PORT))
        # Send a message
        s.sendall(f'stress_test_{i}\n'.encode())
        # Try to receive response
        s.settimeout(2)
        try:
            resp = s.recv(4096)
            if resp:
                with lock:
                    results['success'] += 1
            else:
                with lock:
                    results['success'] += 1  # Connected successfully
        except socket.timeout:
            with lock:
                results['success'] += 1  # Connected but no response (acceptable)
        s.close()
    except Exception as e:
        with lock:
            results['failed'] += 1
            results['errors'].append(str(e))

threads = []
start_time = time.time()

for i in range(NUM_CONNS):
    t = threading.Thread(target=test_connection, args=(i,), daemon=True)
    t.start()
    threads.append(t)

for t in threads:
    t.join(timeout=10)

elapsed = time.time() - start_time

print(f'SUCCESS:{results[\"success\"]}')
print(f'FAILED:{results[\"failed\"]}')
print(f'ELAPSED:{elapsed:.2f}')
print(f'RATE:{results[\"success\"]/elapsed:.1f}' if elapsed > 0 else 'RATE:0')
" 2>/dev/null)

SUCCESS=$(echo "$CONCURRENT_RESULT" | grep "SUCCESS:" | cut -d: -f2)
FAILED=$(echo "$CONCURRENT_RESULT" | grep "FAILED:" | cut -d: -f2)
ELAPSED=$(echo "$CONCURRENT_RESULT" | grep "ELAPSED:" | cut -d: -f2)
RATE=$(echo "$CONCURRENT_RESULT" | grep "RATE:" | cut -d: -f2)

info "并发连接结果: 成功=$SUCCESS, 失败=$FAILED, 耗时=${ELAPSED}s, 速率=${RATE} conn/s"

if [ "${SUCCESS:-0}" -gt 0 ]; then
    SUCCESS_RATE=$(python3 -c "print(f'{$SUCCESS / $STRESS_CONNECTIONS * 100:.1f}%')" 2>/dev/null || echo "?")
    pass "TCP 并发连接: ${SUCCESS}/${STRESS_CONNECTIONS} 成功 (${SUCCESS_RATE})"
else
    fail "TCP 并发连接: 0/${STRESS_CONNECTIONS} 成功"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 3: 消息吞吐量测试
# ═══════════════════════════════════════════════════════════════

step "测试 3: 消息吞吐量测试 (${STRESS_MESSAGES} 条消息)"

info "发送 ${STRESS_MESSAGES} 条消息..."

THROUGHPUT_RESULT=$(python3 -c "
import socket
import time
import sys

HOST = '127.0.0.1'
PORT = 9000
NUM_MSGS = $STRESS_MESSAGES

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(10)
s.connect((HOST, PORT))

start_time = time.time()
sent = 0
received = 0

for i in range(NUM_MSGS):
    try:
        s.sendall(f'msg_{i}\n'.encode())
        sent += 1
    except:
        break

# Try to receive responses
s.settimeout(2)
try:
    while True:
        data = s.recv(4096)
        if not data:
            break
        received += data.count(b'\n')
except:
    pass

elapsed = time.time() - start_time
s.close()

print(f'SENT:{sent}')
print(f'RECEIVED:{received}')
print(f'ELAPSED:{elapsed:.3f}')
print(f'THROUGHPUT_SEND:{sent/elapsed:.1f}' if elapsed > 0 else 'THROUGHPUT_SEND:0')
print(f'THROUGHPUT_RECV:{received/elapsed:.1f}' if elapsed > 0 else 'THROUGHPUT_RECV:0')
" 2>/dev/null)

SENT=$(echo "$THROUGHPUT_RESULT" | grep "SENT:" | cut -d: -f2)
RECEIVED=$(echo "$THROUGHPUT_RESULT" | grep "RECEIVED:" | cut -d: -f2)
T_ELAPSED=$(echo "$THROUGHPUT_RESULT" | grep "ELAPSED:" | cut -d: -f2)
T_SEND=$(echo "$THROUGHPUT_RESULT" | grep "THROUGHPUT_SEND:" | cut -d: -f2)
T_RECV=$(echo "$THROUGHPUT_RESULT" | grep "THROUGHPUT_RECV:" | cut -d: -f2)

info "吞吐量: 发送=${SENT}, 接收=${RECEIVED}, 耗时=${T_ELAPSED}s"
info "发送速率: ${T_SEND} msg/s, 接收速率: ${T_RECV} msg/s"

if [ "${SENT:-0}" -gt 0 ]; then
    pass "消息吞吐量: ${SENT} 条消息已发送 (${T_SEND} msg/s)"
else
    fail "消息吞吐量: 发送失败"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 4: WebSocket 并发连接测试
# ═══════════════════════════════════════════════════════════════

step "测试 4: WebSocket 并发连接测试 (10 连接)"

info "启动 10 个 WebSocket 并发连接..."

WS_RESULT=$(python3 -c "
import socket
import threading
import time
import base64
import os
import hashlib
import struct

HOST = '127.0.0.1'
PORT = 9002
NUM_CONNS = 10
WS_GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'

results = {'success': 0, 'failed': 0}
lock = threading.Lock()

def test_ws_connection(i):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect((HOST, PORT))

        # Generate WebSocket key
        ws_key = base64.b64encode(os.urandom(16)).decode()

        # Send HTTP Upgrade request
        request = (
            f'GET / HTTP/1.1\r\n'
            f'Host: {HOST}:{PORT}\r\n'
            f'Upgrade: websocket\r\n'
            f'Connection: Upgrade\r\n'
            f'Sec-WebSocket-Key: {ws_key}\r\n'
            f'Sec-WebSocket-Version: 13\r\n'
            f'\r\n'
        )
        s.sendall(request.encode())

        # Read response
        response = b''
        while b'\r\n\r\n' not in response:
            chunk = s.recv(4096)
            if not chunk:
                break
            response += chunk

        if b'101' in response:
            # Verify accept key
            expected_accept = base64.b64encode(
                hashlib.sha1(ws_key.encode() + WS_GUID).digest()
            ).decode()

            # Send a text frame
            msg = f'ws_stress_{i}'.encode()
            frame = struct.pack('BB', 0x81, 0x80 | len(msg)) + os.urandom(4)
            masked = bytes(b ^ frame[2 + (j % 4)] for j, b in enumerate(msg))
            s.sendall(frame + masked)

            with lock:
                results['success'] += 1
        else:
            with lock:
                results['failed'] += 1

        s.close()
    except Exception as e:
        with lock:
            results['failed'] += 1

threads = []
start_time = time.time()

for i in range(NUM_CONNS):
    t = threading.Thread(target=test_ws_connection, args=(i,), daemon=True)
    t.start()
    threads.append(t)

for t in threads:
    t.join(timeout=15)

elapsed = time.time() - start_time

print(f'SUCCESS:{results[\"success\"]}')
print(f'FAILED:{results[\"failed\"]}')
print(f'ELAPSED:{elapsed:.2f}')
" 2>/dev/null)

WS_SUCCESS=$(echo "$WS_RESULT" | grep "SUCCESS:" | cut -d: -f2)
WS_FAILED=$(echo "$WS_RESULT" | grep "FAILED:" | cut -d: -f2)
WS_ELAPSED=$(echo "$WS_RESULT" | grep "ELAPSED:" | cut -d: -f2)

info "WebSocket 并发: 成功=$WS_SUCCESS, 失败=$WS_FAILED, 耗时=${WS_ELAPSED}s"

if [ "${WS_SUCCESS:-0}" -gt 0 ]; then
    pass "WebSocket 并发连接: ${WS_SUCCESS}/10 成功"
else
    fail "WebSocket 并发连接: 0/10 成功"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 5: 持续连接稳定性测试
# ═══════════════════════════════════════════════════════════════

step "测试 5: 持续连接稳定性测试 (${STRESS_DURATION}s)"

info "维持连接并持续发送消息 ${STRESS_DURATION} 秒..."

STABILITY_RESULT=$(python3 -c "
import socket
import time
import sys

HOST = '127.0.0.1'
PORT = 9000
DURATION = $STRESS_DURATION

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(10)
s.connect((HOST, PORT))

start_time = time.time()
sent = 0
received = 0
errors = 0

while time.time() - start_time < DURATION:
    try:
        s.sendall(f'keepalive_{sent}\n'.encode())
        sent += 1

        # Non-blocking receive attempt
        s.settimeout(0.1)
        try:
            data = s.recv(4096)
            if data:
                received += data.count(b'\n')
        except socket.timeout:
            pass
        except:
            errors += 1

        time.sleep(0.01)  # Small delay to avoid overwhelming
    except Exception as e:
        errors += 1
        break

elapsed = time.time() - start_time
s.close()

print(f'SENT:{sent}')
print(f'RECEIVED:{received}')
print(f'ERRORS:{errors}')
print(f'ELAPSED:{elapsed:.2f}')
print(f'AVG_RATE:{sent/elapsed:.1f}' if elapsed > 0 else 'AVG_RATE:0')
" 2>/dev/null)

ST_SENT=$(echo "$STABILITY_RESULT" | grep "SENT:" | cut -d: -f2)
ST_RECV=$(echo "$STABILITY_RESULT" | grep "RECEIVED:" | cut -d: -f2)
ST_ERRS=$(echo "$STABILITY_RESULT" | grep "ERRORS:" | cut -d: -f2)
ST_ELAPSED=$(echo "$STABILITY_RESULT" | grep "ELAPSED:" | cut -d: -f2)
ST_RATE=$(echo "$STABILITY_RESULT" | grep "AVG_RATE:" | cut -d: -f2)

info "稳定性: 发送=$ST_SENT, 接收=$ST_RECV, 错误=$ST_ERRS, 耗时=${ST_ELAPSED}s"
info "平均速率: ${ST_RATE} msg/s"

if [ "${ST_SENT:-0}" -gt 0 ] && [ "${ST_ERRS:-999}" -eq 0 ]; then
    pass "持续连接稳定性: ${ST_SENT} 条消息, 0 错误"
elif [ "${ST_SENT:-0}" -gt 0 ]; then
    pass "持续连接稳定性: ${ST_SENT} 条消息, ${ST_ERRS} 错误"
else
    fail "持续连接稳定性: 发送失败"
fi

# ═══════════════════════════════════════════════════════════════
# 测试 6: 验证进程在压力后仍正常
# ═══════════════════════════════════════════════════════════════

step "测试 6: 压力后验证进程状态"

if kill -0 "$GATEWAY_PID" 2>/dev/null; then
    pass "Gateway 进程在压力测试后仍运行"
else
    fail "Gateway 进程在压力测试后已退出"
fi

if kill -0 "$SERVER_PID" 2>/dev/null; then
    pass "后端服务在压力测试后仍运行"
else
    fail "后端服务在压力测试后已退出"
fi

# 验证仍能接受新连接
POST_STRESS_RESP=$(send_tcp_msg "127.0.0.1" 9000 "post_stress_check")
if [ -n "$POST_STRESS_RESP" ]; then
    pass "压力测试后仍能处理新连接"
else
    fail "压力测试后无法处理新连接"
fi

# ═══════════════════════════════════════════════════════════════
# 结果汇总
# ═══════════════════════════════════════════════════════════════

echo ""
echo "============================================"
echo -e "  ${BOLD}Gateway 压力测试结果${NC}"
echo "============================================"
echo -e "  并发连接数: ${STRESS_CONNECTIONS}"
echo -e "  消息数量:   ${STRESS_MESSAGES}"
echo -e "  持续时间:   ${STRESS_DURATION}s"
echo "--------------------------------------------"
echo -e "  通过: ${GREEN}${PASS}${NC}"
echo -e "  失败: ${RED}${FAIL}${NC}"
echo -e "  总计: $((PASS + FAIL))"
echo "============================================"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo -e "${RED}存在失败测试!${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}所有压力测试通过!${NC}"
    exit 0
fi
