#!/usr/bin/env python3
"""
4.6 连接管理测试

测试:
  1. 心跳超时断开: 连接后不发送消息，等待超时
  2. 连接数上限拒绝: --max-connections 3，开 4 个连接，第 4 个应被拒绝
  3. 客户端主动断开: 连接后主动关闭，验证 gateway 不崩溃
  4. 优雅关闭: 发送 SIGTERM，验证 gateway 正常退出
"""

import socket
import struct
import time
import signal

from gw_test_utils import (
    MockBackend, start_gateway, stop_process,
    pack_msg, unpack_msg, recv_msg,
    MSG_PING, MSG_PONG,
    run_tests,
)

PORT_GW = 19061
PORT_BE = 17761


def test_heartbeat_timeout():
    """连接后不发送消息，验证心跳超时后连接被断开"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10,
                           heartbeat_interval=1000, heartbeat_timeout=2000)
        time.sleep(0.3)

        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))

        # 不发送任何消息，等待心跳超时 (2秒)
        time.sleep(4.0)

        # 连接应已被 gateway 关闭
        # recv 返回空 或 send 抛异常
        closed = False
        try:
            s.sendall(pack_msg(MSG_PING))
            data = s.recv(1024)
            if not data:
                closed = True
        except (ConnectionResetError, BrokenPipeError, OSError):
            closed = True

        assert closed, "连接应已被心跳超时关闭"
        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_max_connections_reject():
    """连接数上限：max-connections=3，第 4 个连接应被拒绝"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=3,
                           heartbeat_interval=30000, heartbeat_timeout=90000)
        time.sleep(0.5)

        conns = []
        # 开 3 个连接 (应全部成功)，逐个 ping-pong
        for i in range(3):
            s = socket.socket()
            s.settimeout(5)
            s.connect(("127.0.0.1", PORT_GW))
            conns.append(s)
            time.sleep(0.1)
            s.sendall(pack_msg(MSG_PING))
            msg_type, _ = recv_msg(s, timeout=5)
            assert msg_type == MSG_PONG, f"连接 {i+1} 应能收到 PONG"

        # 第 4 个连接：TCP 握手可能成功，但 Gateway 会立即关闭
        s4 = socket.socket()
        s4.settimeout(3)
        s4.connect(("127.0.0.1", PORT_GW))
        time.sleep(0.5)

        # 第 4 个连接应被关闭 -- send 或 recv 会失败
        rejected = False
        try:
            s4.sendall(pack_msg(MSG_PING))
            data = s4.recv(1024)
            if not data:
                rejected = True
        except (ConnectionResetError, BrokenPipeError, OSError, socket.timeout):
            rejected = True

        assert rejected, "第 4 个连接应被拒绝（连接被关闭或超时）"

        s4.close()
        for s in conns:
            s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_client_disconnect():
    """客户端主动断开，验证 gateway 不崩溃"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10,
                           heartbeat_interval=30000, heartbeat_timeout=90000)
        time.sleep(0.5)

        # 建立连接后立即关闭
        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))
        s.sendall(pack_msg(MSG_PING))
        msg_type, _ = recv_msg(s, timeout=5)
        assert msg_type == MSG_PONG
        s.close()

        # 等待 gateway 处理断开
        time.sleep(1.5)

        # 再建一个连接，确认 gateway 仍正常工作
        s2 = socket.socket()
        s2.settimeout(5)
        s2.connect(("127.0.0.1", PORT_GW))
        time.sleep(0.1)
        s2.sendall(pack_msg(MSG_PING))
        msg_type, _ = recv_msg(s2, timeout=5)
        assert msg_type == MSG_PONG, "gateway 在客户端断开后应仍正常工作"
        s2.close()

        # gateway 仍应存活
        assert gw.poll() is None, "gateway 不应崩溃"
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_graceful_shutdown():
    """SIGTERM 优雅关闭，验证 gateway 正常退出"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10,
                           heartbeat_interval=30000, heartbeat_timeout=90000)
        time.sleep(0.3)

        # 建立一个活跃连接
        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT_GW))
        s.sendall(pack_msg(MSG_PING))
        msg_type, _ = recv_msg(s, timeout=5)
        assert msg_type == MSG_PONG

        # 发送 SIGTERM
        gw.send_signal(signal.SIGTERM)

        # 等待进程退出
        ret = gw.wait(timeout=5)
        assert ret == 0, f"gateway 应以 exit code 0 退出, 实际 {ret}"

        # 连接应被关闭
        try:
            data = s.recv(1024)
            assert not data, "连接应已关闭"
        except (ConnectionResetError, BrokenPipeError, OSError):
            pass  # 符合预期

        s.close()
        gw = None  # 防止 finally 中重复 stop
    finally:
        if gw:
            stop_process(gw)
        be.stop()


if __name__ == "__main__":
    run_tests([
        ("heartbeat_timeout", test_heartbeat_timeout),
        ("max_connections_reject", test_max_connections_reject),
        ("client_disconnect", test_client_disconnect),
        ("graceful_shutdown", test_graceful_shutdown),
    ])
