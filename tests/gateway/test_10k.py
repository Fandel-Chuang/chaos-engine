#!/usr/bin/env python3
"""
1 万并发连接压力测试 v2

策略：
  1. 分批建立 10000 个连接
  2. 分批发 PING（不等 PONG）
  3. 等待 Gateway 处理
  4. 分批收 PONG
  5. 统计结果
"""

import socket
import struct
import time
import sys
import os
import resource
import subprocess

resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))

HEADER_SIZE = 6
MSG_PING = 0x0001
MSG_PONG = 0x0002
TARGET_CONNS = 10000
GW_PORT = 19000
BE_PORT = 17700
BATCH = 200

def pack_msg(msg_type, payload=b""):
    total_len = HEADER_SIZE + len(payload)
    return struct.pack(">IH", total_len, msg_type) + payload

def try_recv_msg(sock, timeout=2):
    """尝试收一个消息，超时返回 None"""
    sock.settimeout(timeout)
    try:
        header = b""
        while len(header) < HEADER_SIZE:
            chunk = sock.recv(HEADER_SIZE - len(header))
            if not chunk:
                return None
            header += chunk
        total_len, msg_type = struct.unpack(">IH", header)
        payload = b""
        if total_len > HEADER_SIZE:
            while len(payload) < total_len - HEADER_SIZE:
                chunk = sock.recv(total_len - HEADER_SIZE - len(payload))
                if not chunk:
                    return None
                payload += chunk
        return msg_type
    except (socket.timeout, OSError):
        return None

def main():
    print(f"=== 1 万并发连接压力测试 v2 ===")
    print(f"目标: {TARGET_CONNS} 个 TCP 连接")
    print(f"fd 上限: {resource.getrlimit(resource.RLIMIT_NOFILE)[0]}")
    print()

    gw_bin = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                          "build", "bin", "chaos_gateway")
    gw = subprocess.Popen(
        [gw_bin, "--port", str(GW_PORT), "--backend", f"127.0.0.1:{BE_PORT}",
         "--max-connections", "12000", "--heartbeat-interval", "600000",
         "--heartbeat-timeout", "600000", "--no-kcp"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1.0)
    if gw.poll() is not None:
        print("FAIL: Gateway 启动失败")
        sys.exit(1)

    # Phase 1: 建立连接
    print(f"[1/3] 建立 {TARGET_CONNS} 个连接...")
    conns = []
    fail = 0
    t0 = time.time()
    for i in range(TARGET_CONNS):
        try:
            s = socket.socket()
            s.settimeout(5)
            s.connect(("127.0.0.1", GW_PORT))
            conns.append(s)
        except Exception:
            fail += 1
        if (i + 1) % 2000 == 0:
            print(f"      {i+1}/{TARGET_CONNS} ({time.time()-t0:.1f}s, 失败 {fail})")
    connect_time = time.time() - t0
    print(f"      完成: {len(conns)} 成功, {fail} 失败, {connect_time:.1f}s")
    print()

    # Phase 2: 分批发 PING，全部发完后再收 PONG
    print(f"[2/3] 发送 {len(conns)} 个 PING...")
    t0 = time.time()
    sent = 0
    for i in range(0, len(conns), BATCH):
        batch = conns[i:i+BATCH]
        for s in batch:
            try:
                s.sendall(pack_msg(MSG_PING))
                sent += 1
            except Exception:
                pass
    send_time = time.time() - t0
    print(f"      发送完成: {sent} 个, {send_time:.2f}s")
    print()

    # 等 Gateway 处理
    print(f"      等待 Gateway 处理 (2s)...")
    time.sleep(2.0)

    # Phase 3: 收 PONG
    print(f"[3/3] 接收 PONG...")
    t0 = time.time()
    success = 0
    timeout_count = 0
    error_count = 0
    for i in range(0, len(conns), BATCH):
        batch = conns[i:i+BATCH]
        for s in batch:
            r = try_recv_msg(s, timeout=3)
            if r is None:
                timeout_count += 1
            elif r == MSG_PONG:
                success += 1
            else:
                error_count += 1
        if (i + BATCH) % 2000 == 0 or i + BATCH >= len(conns):
            print(f"      {min(i+BATCH, len(conns))}/{len(conns)} "
                  f"(成功 {success}, 超时 {timeout_count}, 错误 {error_count})")

    recv_time = time.time() - t0
    total_time = connect_time + send_time + 2.0 + recv_time
    qps = success / (send_time + 2.0 + recv_time) if (send_time + 2.0 + recv_time) > 0 else 0

    print()
    print(f"=== 测试结果 ===")
    print(f"  连接数:   {len(conns)}/{TARGET_CONNS}")
    print(f"  建连耗时: {connect_time:.1f}s ({len(conns)/connect_time:.0f} conns/s)")
    print(f"  发送:     {sent} PING ({send_time:.2f}s)")
    print(f"  接收:     {success} PONG ({recv_time:.1f}s)")
    print(f"  超时:     {timeout_count}")
    print(f"  错误:     {error_count}")
    print(f"  成功率:   {success/len(conns)*100:.1f}%")
    print(f"  吞吐量:   {qps:.0f} QPS")
    print(f"  总耗时:   {total_time:.1f}s")

    for s in conns:
        try: s.close()
        except: pass
    gw.terminate()
    gw.wait()

    rate = success / len(conns) if conns else 0
    if rate >= 0.95:
        print(f"\n  === PASS ===")
        sys.exit(0)
    else:
        print(f"\n  === FAIL (需 >= 95%) ===")
        sys.exit(1)

if __name__ == "__main__":
    main()
