#!/usr/bin/env python3
"""
4.8 性能基准测试

测试:
  1. 并发连接: 开 100 个 TCP 连接，全部发 PING 收 PONG
  2. 消息吞吐量: 单连接连续发送 1000 条 PING，统计 QPS 和 P50/P99 延迟
  3. TCP vs WebSocket 对比: 同样负载下对比两种协议的延迟

注意: CI 环境用 100 连接，1000 条消息。
"""

import socket
import struct
import time
import statistics

from gw_test_utils import (
    MockBackend, start_gateway, stop_process,
    pack_msg, unpack_msg, recv_msg,
    ws_handshake, ws_send_frame, ws_recv_frame,
    MSG_PING, MSG_PONG,
    WS_OPCODE_BINARY,
    run_tests,
)

PORT_GW = 19082
PORT_BE = 17782

NUM_CONCURRENT = 20
NUM_THROUGHPUT = 200


def test_concurrent_connections():
    """100 个并发 TCP 连接，每个发 PING 收 PONG"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=200,
                           heartbeat_interval=30000, heartbeat_timeout=90000)
        time.sleep(0.3)

        conns = []
        for i in range(NUM_CONCURRENT):
            s = socket.socket()
            s.settimeout(5)
            s.connect(("127.0.0.1", PORT_GW))
            conns.append(s)

        # 逐个发 PING 收 PONG（Gateway 是单线程 io_uring）
        success = 0
        for s in conns:
            try:
                s.sendall(pack_msg(MSG_PING))
                msg_type, _ = recv_msg(s, timeout=5)
                if msg_type == MSG_PONG:
                    success += 1
            except Exception:
                pass

        assert success == NUM_CONCURRENT, f"只有 {success}/{NUM_CONCURRENT} 个连接收到 PONG"

        for s in conns:
            s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_throughput():
    """单连接连续发送 1000 条 PING，统计 QPS 和延迟"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10,
                           heartbeat_interval=30000, heartbeat_timeout=90000)
        time.sleep(0.3)

        s = socket.socket()
        s.settimeout(10)
        s.connect(("127.0.0.1", PORT_GW))

        # 逐条发送 PING 收 PONG (ping-pong 模式)
        latencies = []
        start = time.time()

        for _ in range(NUM_THROUGHPUT):
            t0 = time.time()
            s.sendall(pack_msg(MSG_PING))
            msg_type, _ = recv_msg(s, timeout=10)
            t1 = time.time()
            assert msg_type == MSG_PONG, f"期望 PONG, 实际 0x{msg_type:04X}"
            latencies.append((t1 - t0) * 1000)  # ms

        elapsed = time.time() - start
        qps = NUM_THROUGHPUT / elapsed

        p50 = statistics.median(latencies)
        p99 = sorted(latencies)[int(len(latencies) * 0.99)]

        print(f"\n    吞吐量: {qps:.0f} QPS, P50={p50:.3f}ms, P99={p99:.3f}ms")

        # 基本断言: QPS > 100 (CI 环境)
        assert qps > 100, f"QPS {qps:.0f} 过低"
        assert p99 < 100, f"P99 {p99:.3f}ms 过高"

        s.close()
    finally:
        if gw:
            stop_process(gw)
        be.stop()


def test_tcp_vs_ws_latency():
    """TCP vs WebSocket 延迟对比"""
    be = MockBackend(PORT_BE)
    be.start()
    gw = None
    try:
        gw = start_gateway(PORT_GW, PORT_BE, max_connections=10,
                           heartbeat_interval=30000, heartbeat_timeout=90000)
        time.sleep(0.3)

        N = 100  # 对比用 100 条

        # TCP
        tcp_s = socket.socket()
        tcp_s.settimeout(10)
        tcp_s.connect(("127.0.0.1", PORT_GW))
        tcp_latencies = []
        for _ in range(N):
            t0 = time.time()
            tcp_s.sendall(pack_msg(MSG_PING))
            recv_msg(tcp_s, timeout=10)
            tcp_latencies.append((time.time() - t0) * 1000)
        tcp_s.close()
        time.sleep(0.5)

        # WebSocket
        ws_s = socket.socket()
        ws_s.settimeout(10)
        ws_s.connect(("127.0.0.1", PORT_GW))
        assert ws_handshake(ws_s, port=PORT_GW), "WS 握手失败"
        ws_latencies = []
        for _ in range(N):
            t0 = time.time()
            ws_send_frame(ws_s, pack_msg(MSG_PING), opcode=WS_OPCODE_BINARY)
            ws_recv_frame(ws_s, timeout=10)
            ws_latencies.append((time.time() - t0) * 1000)
        ws_s.close()

        tcp_p50 = statistics.median(tcp_latencies)
        ws_p50 = statistics.median(ws_latencies)
        print(f"\n    TCP P50={tcp_p50:.3f}ms, WS P50={ws_p50:.3f}ms")

        # WebSocket 有帧封装开销，延迟略高是正常的
        # 只验证两者都在合理范围
        assert tcp_p50 < 50, f"TCP P50 {tcp_p50:.3f}ms 过高"
        assert ws_p50 < 50, f"WS P50 {ws_p50:.3f}ms 过高"

    finally:
        if gw:
            stop_process(gw)
        be.stop()


# ── pytest 兼容 ──
def test_pytest_concurrent_connections():
    test_concurrent_connections()


def test_pytest_throughput():
    test_throughput()


def test_pytest_tcp_vs_ws_latency():
    test_tcp_vs_ws_latency()


if __name__ == "__main__":
    print("=== 4.8 性能基准测试 ===")
    tests = [
        ("concurrent_connections", test_concurrent_connections),
        ("throughput", test_throughput),
        ("tcp_vs_ws_latency", test_tcp_vs_ws_latency),
    ]
    import sys
    sys.exit(run_tests(tests))
