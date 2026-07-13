"""
Gateway 集成测试公共工具模块

提供 Gateway 进程启停、mock 后端、协议帧编解码、WebSocket 帧操作等工具。
仅依赖 Python 标准库 (socket, struct, subprocess, threading, hashlib, base64, os)。
"""

import os
import socket
import struct
import subprocess
import threading
import time
import signal
import hashlib
import base64
import sys

# ── 常量 ──────────────────────────────────────────────

GATEWAY_BIN = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "build", "bin", "chaos_gateway",
)

# 协议帧头：4B total_len + 2B msg_type = 6 字节
HEADER_SIZE = 6

# 消息类型
MSG_PING = 0x0001
MSG_PONG = 0x0002
MSG_LOGIN = 0x0010
MSG_LOGIN_RESP = 0x0011
MSG_GAME_DATA = 0x0100
MSG_DISCONNECT = 0xFFFF

# WebSocket opcodes (RFC 6455)
WS_OPCODE_CONTINUATION = 0x0
WS_OPCODE_TEXT = 0x1
WS_OPCODE_BINARY = 0x2
WS_OPCODE_CLOSE = 0x8
WS_OPCODE_PING = 0x9
WS_OPCODE_PONG = 0xA

WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# 端口基数，各测试可用不同偏移避免冲突
PORT_BASE = 19000


# ── 协议帧编解码 ──────────────────────────────────────

def pack_msg(msg_type, payload=b""):
    """打包协议帧 [4B total_len][2B msg_type][N payload] (大端序)"""
    total_len = HEADER_SIZE + len(payload)
    return struct.pack(">IH", total_len, msg_type) + payload


def unpack_msg(data):
    """解包协议帧，返回 (msg_type, payload)"""
    if len(data) < HEADER_SIZE:
        raise ValueError(f"data too short: {len(data)} < {HEADER_SIZE}")
    total_len, msg_type = struct.unpack(">IH", data[:HEADER_SIZE])
    payload = data[HEADER_SIZE:total_len]
    return msg_type, payload


def recv_exact(sock, n, timeout=5):
    """从 socket 精确接收 n 字节，超时抛 socket.timeout"""
    sock.settimeout(timeout)
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed while reading")
        buf += chunk
    return buf


def recv_msg(sock, timeout=5):
    """接收一个完整协议帧，返回 (msg_type, payload)"""
    header = recv_exact(sock, HEADER_SIZE, timeout)
    total_len, msg_type = struct.unpack(">IH", header)
    if total_len < HEADER_SIZE:
        raise ValueError(f"invalid total_len: {total_len}")
    payload = b""
    if total_len > HEADER_SIZE:
        payload = recv_exact(sock, total_len - HEADER_SIZE, timeout)
    return msg_type, payload


# ── WebSocket 帧编解码 ────────────────────────────────

def ws_handshake(sock, host="127.0.0.1", port=None, timeout=5):
    """
    执行 WebSocket 握手。
    sock: 已连接的 TCP socket
    返回 True 表示握手成功 (收到 101 Switching Protocols)
    """
    sock.settimeout(timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    if port is None:
        port = sock.getpeername()[1]
    request = (
        "GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request.encode())

    # 读取 HTTP 响应头
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(4096)
        if not chunk:
            return False
        resp += chunk
        if len(resp) > 8192:
            return False

    return b"101" in resp.split(b"\r\n")[0]


def ws_send_frame(sock, payload, opcode=WS_OPCODE_BINARY, mask=True):
    """
    发送 WebSocket 帧 (客户端默认 mask)。
    payload: 帧数据 (对于 BINARY 帧，内含协议帧)
    """
    sock.settimeout(5)
    frame = bytearray()
    # FIN=1, opcode
    frame.append(0x80 | opcode)

    plen = len(payload)
    mask_bit = 0x80 if mask else 0x00

    if plen < 126:
        frame.append(mask_bit | plen)
    elif plen <= 65535:
        frame.append(mask_bit | 126)
        frame += struct.pack(">H", plen)
    else:
        frame.append(mask_bit | 127)
        frame += struct.pack(">Q", plen)

    if mask:
        mask_key = os.urandom(4)
        frame += mask_key
        masked = bytearray(len(payload))
        for i in range(len(payload)):
            masked[i] = payload[i] ^ mask_key[i % 4]
        frame += masked
    else:
        frame += payload

    sock.sendall(bytes(frame))


def ws_recv_frame(sock, timeout=5):
    """
    接收一个 WebSocket 帧 (服务端发送，不 mask)。
    返回 (opcode, payload)
    """
    sock.settimeout(timeout)
    header = recv_exact(sock, 2, timeout)
    fin_opcode = header[0]
    opcode = fin_opcode & 0x0F
    plen = header[1] & 0x7F

    if plen == 126:
        ext = recv_exact(sock, 2, timeout)
        plen = struct.unpack(">H", ext)[0]
    elif plen == 127:
        ext = recv_exact(sock, 8, timeout)
        plen = struct.unpack(">Q", ext)[0]

    # 服务端发送的帧不 mask
    payload = b""
    if plen > 0:
        payload = recv_exact(sock, plen, timeout)

    return opcode, payload


# ── 进程管理 ──────────────────────────────────────────

def wait_port_ready(port, host="127.0.0.1", timeout=5):
    """等待 TCP 端口可连接，返回 True/False"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket()
            s.settimeout(0.5)
            s.connect((host, port))
            s.close()
            return True
        except (ConnectionRefusedError, socket.timeout, OSError):
            time.sleep(0.1)
    return False


def start_gateway(port, backend_port, max_connections=100,
                  heartbeat_interval=30000, heartbeat_timeout=90000,
                  kcp=True, ws=True, extra_args=None):
    """
    启动 chaos_gateway 进程，返回 subprocess.Popen。
    需调用方确保 backend_port 已有 mock 后端在监听 (否则 gateway 启动时连接失败)。
    """
    args = [
        GATEWAY_BIN,
        "--port", str(port),
        "--backend", f"127.0.0.1:{backend_port}",
        "--max-connections", str(max_connections),
        "--heartbeat-interval", str(heartbeat_interval),
        "--heartbeat-timeout", str(heartbeat_timeout),
    ]
    if not kcp:
        args.append("--no-kcp")
    if not ws:
        args.append("--no-ws")
    if extra_args:
        args.extend(extra_args)

    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    log_dir = os.path.join(repo_root, "logs")
    os.makedirs(log_dir, exist_ok=True)

    proc = subprocess.Popen(
        args,
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    if not wait_port_ready(port, timeout=5):
        # 读取输出帮助调试
        try:
            out = proc.stdout.read(4096).decode(errors="replace") if proc.stdout else ""
        except Exception:
            out = ""
        stop_process(proc)
        raise RuntimeError(f"Gateway failed to start on port {port}. Output: {out}")

    return proc


def stop_process(proc, timeout=3):
    """优雅停止进程 (SIGTERM)，超时则 SIGKILL"""
    if proc.poll() is not None:
        return
    try:
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=1)


# ── Mock 后端 ─────────────────────────────────────────

class MockBackend:
    """
    Python TCP server 模拟 chaos_server 后端。
    用法:
        be = MockBackend(port)
        be.start()
        ... 发送 GAME_DATA 后 be.received 会有数据 ...
        be.stop()
    """

    def __init__(self, port=0):
        self.port = port
        self._sock = None
        self._thread = None
        self._running = False
        self._client_sock = None
        self._client_lock = threading.Lock()
        self.received = []  # 收到的所有数据块
        self.received_lock = threading.Lock()

    def start(self):
        self._sock = socket.socket()
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", self.port))
        self._sock.listen(5)
        self._sock.settimeout(0.5)
        self.port = self._sock.getsockname()[1]  # 获取实际端口 (port=0 时)
        self._running = True
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()
        # 等待监听就绪
        wait_port_ready(self.port, timeout=2)

    def _accept_loop(self):
        while self._running:
            try:
                conn, addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self._client_lock:
                if self._client_sock:
                    self._client_sock.close()
                self._client_sock = conn
            t = threading.Thread(target=self._recv_loop, args=(conn,), daemon=True)
            t.start()

    def _recv_loop(self, conn):
        conn.settimeout(0.5)
        while self._running:
            try:
                data = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            with self.received_lock:
                self.received.append(data)

    def get_received(self):
        """返回收到的所有数据 (合并)"""
        with self.received_lock:
            return b"".join(self.received)

    def clear_received(self):
        with self.received_lock:
            self.received.clear()

    def send_to_client(self, data):
        """向当前连接的 gateway 后端连接发送数据"""
        with self._client_lock:
            if self._client_sock:
                try:
                    self._client_sock.sendall(data)
                    return True
                except OSError:
                    return False
        return False

    def stop(self):
        self._running = False
        with self._client_lock:
            if self._client_sock:
                try:
                    self._client_sock.close()
                except OSError:
                    pass
                self._client_sock = None
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
        if self._thread:
            self._thread.join(timeout=2)


# ── 测试框架兼容 ──────────────────────────────────────

def run_tests(test_functions):
    """
    简易测试运行器，支持直接 python3 test_xxx.py 运行。
    test_functions: list of (name, callable)
    返回 0 (全通过) 或 1 (有失败)
    """
    passed = 0
    failed = 0
    for name, fn in test_functions:
        print(f"  [{name}] ... ", end="", flush=True)
        try:
            fn()
            print("PASS")
            passed += 1
        except Exception as e:
            print(f"FAIL: {e}")
            import traceback
            traceback.print_exc()
            failed += 1
    print(f"\n  结果: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1
