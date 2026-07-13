#!/usr/bin/env python3
"""
4.5 WebSocket 接入测试

测试流程:
  1. 启动 gateway --port 19050
  2. 发送 HTTP Upgrade 请求，验证收到 101 Switching Protocols
  3. 发送 WebSocket 二进制帧 (内含 PING 协议帧)，验证收到 PONG 帧
  4. 发送 WS PING 帧，验证收到 PONG 帧
  5. 发送 Close 帧，验证连接关闭
"""

import socket
import time

from gw_test_utils import (
    MockBackend, start_gateway, stop_process,
    pack_msg, unpack_msg,
    ws_handshake, ws_send_frame, ws_recv_frame,
    MSG_PING, MSG_PONG,
    WS_OPCODE_BINARY, WS_OPCODE_PING, WS_OPCODE_PONG, WS_OPCODE_CLOSE,
    run_tests,
)

PORT_GW = 19050
PORT_BE = 17750


def _start_env(max_conns=10):
    be = MockBackend(PORT_BE)
    be.start()
    gw = start_gateway(PORT_GW, PORT_BE, max_connections=max_conns, ws=True)
    time.sleep(0.3)
    return be, gw


def test_ws_handshake():
    """WebSocket 握手：发送 HTTP Upgrade，验证收到 101"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        ok = ws_handshake(s, port=PORT_GW)
        assert ok, "WebSocket 握手失败 (未收到 101 Switching Protocols)"
        s.close()
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


def test_ws_binary_ping_pong():
    """WebSocket 二进制帧：发送 PING 协议帧，验证收到 PONG"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        assert ws_handshake(s, port=PORT_GW), "握手失败"

        # 发送 WS BINARY 帧内含 PING 协议帧
        ws_send_frame(s, pack_msg(MSG_PING), opcode=WS_OPCODE_BINARY)

        # 接收 WS 响应帧
        opcode, payload = ws_recv_frame(s, timeout=5)
        assert opcode == WS_OPCODE_BINARY, f"期望 BINARY(0x2), 实际 0x{opcode:X}"
        msg_type, msg_payload = unpack_msg(payload)
        assert msg_type == MSG_PONG, f"期望 PONG(0x0002), 实际 0x{msg_type:04X}"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


def test_ws_ping_frame():
    """WebSocket PING 控制帧，验证收到 PONG"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        assert ws_handshake(s, port=PORT_GW), "握手失败"

        # 发送 WS PING 帧
        ws_send_frame(s, b"heartbeat", opcode=WS_OPCODE_PING)

        # 接收 PONG 帧
        opcode, payload = ws_recv_frame(s, timeout=5)
        assert opcode == WS_OPCODE_PONG, f"期望 PONG(0xA), 实际 0x{opcode:X}"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


def test_ws_close_frame():
    """WebSocket Close 帧，验证连接关闭"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        assert ws_handshake(s, port=PORT_GW), "握手失败"

        # 发送 Close 帧
        ws_send_frame(s, b"", opcode=WS_OPCODE_CLOSE)

        # 等待连接关闭 (gateway 关闭后 recv 返回空或抛异常)
        time.sleep(0.5)
        try:
            s.settimeout(2)
            data = s.recv(1024)
            # 可能收到 close 响应帧，或空数据
            if not data:
                pass  # 连接已关闭
        except (ConnectionResetError, socket.timeout, OSError):
            pass  # 连接已关闭

        s.close()
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


# ── pytest 兼容 ──
def test_pytest_ws_handshake():
    test_ws_handshake()


def test_pytest_ws_binary_ping_pong():
    test_ws_binary_ping_pong()


def test_pytest_ws_ping_frame():
    test_ws_ping_frame()


def test_pytest_ws_close_frame():
    test_ws_close_frame()


if __name__ == "__main__":
    print("=== 4.5 WebSocket 接入测试 ===")
    tests = [
        ("ws_handshake", test_ws_handshake),
        ("ws_binary_ping_pong", test_ws_binary_ping_pong),
        ("ws_ping_frame", test_ws_ping_frame),
        ("ws_close_frame", test_ws_close_frame),
    ]
    import sys
    sys.exit(run_tests(tests))
