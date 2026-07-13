#!/usr/bin/env python3
"""
4.3 TCP 接入测试

测试流程:
  1. 启动 mock 后端 (Python TCP server)
  2. 启动 chaos_gateway --port 19030 --backend 127.0.0.1:17730
  3. TCP 客户端连接 gateway:19030
  4. 发送 PING 消息，验证收到 PONG
  5. 发送 GAME_DATA 消息，验证后端收到转发
  6. 关闭连接

可独立运行: python3 tests/gateway/test_tcp.py
也可 pytest 运行: pytest tests/gateway/test_tcp.py -v
"""

import socket
import time

from gw_test_utils import (
    MockBackend, start_gateway, stop_process,
    pack_msg, unpack_msg, recv_msg, recv_exact,
    MSG_PING, MSG_PONG, MSG_GAME_DATA, HEADER_SIZE,
    run_tests,
)

PORT_GW = 19030
PORT_BE = 17730


def test_tcp_ping_pong():
    """TCP 客户端发送 PING，验证收到 PONG"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10)
        time.sleep(0.3)

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        # 发送 PING
        s.sendall(pack_msg(MSG_PING))
        msg_type, payload = recv_msg(s, timeout=5)
        assert msg_type == MSG_PONG, f"期望 PONG(0x0002), 实际 0x{msg_type:04X}"
        assert len(payload) == 0, "PONG payload 应为空"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_tcp_game_data_forward():
    """TCP 客户端发送 GAME_DATA，验证后端收到转发"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10)
        time.sleep(0.5)  # 等待 gateway 连接后端

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        # 发送 GAME_DATA
        game_payload = b"HELLO_GAME"
        s.sendall(pack_msg(MSG_GAME_DATA, game_payload))

        # 等待后端收到数据
        deadline = time.time() + 3
        while time.time() < deadline:
            data = be.get_received()
            if len(data) >= HEADER_SIZE + len(game_payload):
                break
            time.sleep(0.1)
        else:
            assert False, "后端未收到数据"

        # 验证后端收到的数据
        data = be.get_received()
        msg_type, payload = unpack_msg(data[:HEADER_SIZE + len(game_payload)])
        assert msg_type == MSG_GAME_DATA, f"期望 GAME_DATA(0x0100), 实际 0x{msg_type:04X}"
        assert payload == game_payload, f"payload 不匹配: {payload} != {game_payload}"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_tcp_multiple_messages():
    """TCP 单连接连续发送多条消息 (PING + GAME_DATA)"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10)
        time.sleep(0.5)

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        # 连续发送 PING + PING
        s.sendall(pack_msg(MSG_PING) + pack_msg(MSG_PING))

        # 收到两个 PONG
        msg_type1, _ = recv_msg(s, timeout=5)
        assert msg_type1 == MSG_PONG, f"第一个应为 PONG, 实际 0x{msg_type1:04X}"
        msg_type2, _ = recv_msg(s, timeout=5)
        assert msg_type2 == MSG_PONG, f"第二个应为 PONG, 实际 0x{msg_type2:04X}"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


# ── pytest 兼容 ──
def setup_function(func):
    """每个测试前确保端口空闲 (pytest)"""
    pass


def teardown_function(func):
    """每个测试后清理 (pytest)"""
    pass


# ── pytest 测试函数 ──
def test_pytest_tcp_ping_pong():
    test_tcp_ping_pong()


def test_pytest_tcp_game_data_forward():
    test_tcp_game_data_forward()


def test_pytest_tcp_multiple_messages():
    test_tcp_multiple_messages()


if __name__ == "__main__":
    print("=== 4.3 TCP 接入测试 ===")
    tests = [
        ("tcp_ping_pong", test_tcp_ping_pong),
        ("tcp_game_data_forward", test_tcp_game_data_forward),
        ("tcp_multiple_messages", test_tcp_multiple_messages),
    ]
    import sys
    sys.exit(run_tests(tests))
