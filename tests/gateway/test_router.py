#!/usr/bin/env python3
"""
4.7 消息路由测试

测试:
  1. 正确路由: 发 PING (系统消息, gateway 自己处理回 PONG)
  2. 正确转发: 发 GAME_DATA, 验证后端收到
  3. 未知 msg_type: 发 0x9999, 验证 gateway 不崩溃 (丢弃即可)
  4. 后端不可用: 不启动后端, 发 GAME_DATA, gateway 不应崩溃
"""

import socket
import struct
import time

from gw_test_utils import (
    MockBackend, start_gateway, stop_process,
    pack_msg, unpack_msg, recv_msg,
    MSG_PING, MSG_PONG, MSG_GAME_DATA, HEADER_SIZE,
    run_tests,
)

PORT_GW = 19070
PORT_BE = 17770

UNKNOWN_MSG_TYPE = 0x9999


def test_ping_routed_locally():
    """PING 是系统消息，gateway 自己处理回 PONG (不到后端)"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10)
        time.sleep(0.3)

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        s.sendall(pack_msg(MSG_PING))
        msg_type, _ = recv_msg(s, timeout=5)
        assert msg_type == MSG_PONG, f"期望 PONG, 实际 0x{msg_type:04X}"

        # 确认后端没收到 PING 数据
        time.sleep(0.3)
        assert len(be.get_received()) == 0, "PING 不应转发到后端"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_game_data_forwarded():
    """GAME_DATA 应转发到后端"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10)
        time.sleep(0.5)

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        payload = b"ROUTER_TEST_DATA"
        s.sendall(pack_msg(MSG_GAME_DATA, payload))

        # 等待后端收到
        deadline = time.time() + 3
        while time.time() < deadline:
            if len(be.get_received()) >= HEADER_SIZE + len(payload):
                break
            time.sleep(0.1)
        else:
            assert False, "后端未收到 GAME_DATA"

        data = be.get_received()
        msg_type, recv_payload = unpack_msg(data[:HEADER_SIZE + len(payload)])
        assert msg_type == MSG_GAME_DATA
        assert recv_payload == payload

        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_unknown_msg_type_no_crash():
    """未知 msg_type (0x9999) 应被丢弃，gateway 不崩溃"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10)
        time.sleep(0.3)

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        # 发送未知消息类型
        s.sendall(pack_msg(UNKNOWN_MSG_TYPE, b"unknown_data"))

        time.sleep(0.5)

        # gateway 应仍存活
        assert gw.poll() is None, "gateway 在未知 msg_type 后不应崩溃"

        # 连接应仍可用 (发 PING 收 PONG)
        s.sendall(pack_msg(MSG_PING))
        msg_type, _ = recv_msg(s, timeout=5)
        assert msg_type == MSG_PONG, "未知消息后连接应仍可用"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_backend_unavailable_no_crash():
    """后端不可用时发 GAME_DATA，gateway 不崩溃
    注意：当前 Gateway 后端重连是阻塞调用（W5），会短暂卡住事件循环。
    本测试只验证 gateway 不崩溃，PING/PONG 可选验证。"""
    gw_port = 19075
    be_port = 17775
    gw = None
    try:
        gw = start_gateway(gw_port, be_port, max_connections=10)
        time.sleep(0.5)

        # 后端不可用（不启动 MockBackend）
        s = socket.socket()
        s.settimeout(10)
        s.connect(("127.0.0.1", gw_port))
        s.sendall(pack_msg(MSG_GAME_DATA, b"NO_BACKEND"))

        time.sleep(2.0)

        # gateway 应仍存活（核心断言）
        assert gw.poll() is None, "gateway 在后端不可用时不应崩溃"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        # be 已在测试中 stop


# ── pytest 兼容 ──
def test_pytest_ping_routed_locally():
    test_ping_routed_locally()


def test_pytest_game_data_forwarded():
    test_game_data_forwarded()


def test_pytest_unknown_msg_type_no_crash():
    test_unknown_msg_type_no_crash()


def test_pytest_backend_unavailable_no_crash():
    test_backend_unavailable_no_crash()


if __name__ == "__main__":
    print("=== 4.7 消息路由测试 ===")
    tests = [
        ("ping_routed_locally", test_ping_routed_locally),
        ("game_data_forwarded", test_game_data_forwarded),
        ("unknown_msg_type_no_crash", test_unknown_msg_type_no_crash),
        ("backend_unavailable_no_crash", test_backend_unavailable_no_crash),
    ]
    import sys
    sys.exit(run_tests(tests))
