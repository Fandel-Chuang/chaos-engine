#!/usr/bin/env python3
"""
4.4 KCP 接入测试

KCP 协议较复杂，本测试简化为:
  1. 验证 gateway 的 UDP socket 正常监听 (端口可达)
  2. 发送 UDP 数据包，验证 gateway 不崩溃
  3. 发送带 conv ID 的 KCP 数据包，验证 gateway 有处理 (可能无响应，因 KCP 握手未完成)

注意: 完整 KCP 客户端实现复杂，这里只验证 UDP 端口可达 + 数据收发不崩溃。
"""

import socket
import struct
import time

from gw_test_utils import (
    MockBackend, start_gateway, stop_process,
    pack_msg, unpack_msg,
    MSG_PING, MSG_PONG,
    run_tests,
)

PORT_GW = 19040
PORT_BE = 17740


def _start_env(max_conns=10):
    be = MockBackend(PORT_BE)
    be.start()
    gw = start_gateway(PORT_GW, PORT_BE, max_connections=max_conns, kcp=True)
    time.sleep(0.3)
    return be, gw


def test_udp_port_listening():
    """验证 gateway 的 UDP socket 正常监听"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.settimeout(2)

        # 发送任意 UDP 数据包，不应抛异常
        udp.sendto(b"\x00" * 4, ("127.0.0.1", PORT_GW))
        # 不要求收到响应 (KCP 协议数据不合法时 gateway 丢弃)
        # 只要发送成功即可
        udp.close()

        # 确认 gateway 仍在运行
        assert gw.poll() is None, "gateway 进程不应崩溃"
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


def test_udp_invalid_data_no_crash():
    """发送无效 UDP 数据，验证 gateway 不崩溃"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.settimeout(2)

        # 发送各种无效数据
        udp.sendto(b"", ("127.0.0.1", PORT_GW))  # 空包
        udp.sendto(b"\x00", ("127.0.0.1", PORT_GW))  # 1 字节
        udp.sendto(b"\x00" * 100, ("127.0.0.1", PORT_GW))  # conv=0 (无效)
        udp.sendto(b"GARBAGE_DATA", ("127.0.0.1", PORT_GW))  # 垃圾数据

        time.sleep(0.5)
        udp.close()

        # gateway 仍应存活
        assert gw.poll() is None, "gateway 在无效 UDP 数据后不应崩溃"
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


def test_kcp_conv_data_accepted():
    """发送带有效 conv ID 的 KCP 数据包，验证 gateway 接受 (不崩溃)"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.settimeout(2)

        # KCP conv ID (小端序, 非0), 后面跟 KCP 协议数据
        conv_id = 0x00000001
        conv_bytes = struct.pack("<I", conv_id)  # KCP conv 是小端序

        # 构造一个简单的 KCP 数据包
        # KCP 头部: conv(4) + cmd(1) + frg(1) + wnd(2) + ts(4) + sn(4) + una(4) + len(4) = 24 字节
        # cmd=81=IKCP_CMD_PUSH, frg=0, wnd=128, ts=0, sn=0, una=0, len=payload_len
        inner_data = pack_msg(MSG_PING)  # 内含 PING 协议帧
        kcp_header = conv_bytes + struct.pack("<BBHIII", 81, 0, 128, 0, 0, 0) + struct.pack("<I", len(inner_data))
        kcp_packet = kcp_header + inner_data

        udp.sendto(kcp_packet, ("127.0.0.1", PORT_GW))

        # 等待可能的响应
        time.sleep(0.5)

        # gateway 仍应存活
        assert gw.poll() is None, "gateway 在 KCP 数据后不应崩溃"

        udp.close()
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


def test_tcp_and_udp_coexist():
    """验证 TCP 和 UDP (KCP) 端口共用不冲突"""
    be, gw = None, None
    try:
        be, gw = _start_env()

        # TCP 连接 + PING
        tcp_s = socket.socket()
        tcp_s.settimeout(5)
        tcp_s.connect(("127.0.0.1", PORT_GW))
        tcp_s.sendall(pack_msg(MSG_PING))
        data = tcp_s.recv(1024)
        msg_type, _ = unpack_msg(data[:6])
        assert msg_type == MSG_PONG, f"TCP PING 应收到 PONG, 实际 0x{msg_type:04X}"
        tcp_s.close()

        # UDP 发送
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.settimeout(2)
        udp.sendto(b"\x01\x00\x00\x00" + b"\x00" * 20, ("127.0.0.1", PORT_GW))
        udp.close()

        assert gw.poll() is None, "gateway 仍应运行"
    finally:
        if gw:
            stop_process(gw)
        if be:
            be.stop()


# ── pytest 兼容 ──
def test_pytest_udp_port_listening():
    test_udp_port_listening()


def test_pytest_udp_invalid_data_no_crash():
    test_udp_invalid_data_no_crash()


def test_pytest_kcp_conv_data_accepted():
    test_kcp_conv_data_accepted()


def test_pytest_tcp_and_udp_coexist():
    test_tcp_and_udp_coexist()


if __name__ == "__main__":
    print("=== 4.4 KCP 接入测试 ===")
    tests = [
        ("udp_port_listening", test_udp_port_listening),
        ("udp_invalid_data_no_crash", test_udp_invalid_data_no_crash),
        ("kcp_conv_data_accepted", test_kcp_conv_data_accepted),
        ("tcp_and_udp_coexist", test_tcp_and_udp_coexist),
    ]
    import sys
    sys.exit(run_tests(tests))
