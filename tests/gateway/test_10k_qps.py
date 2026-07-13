#!/usr/bin/env python3
"""
1 万并发 QPS 基准测试

策略：
  1. 建立 10000 个连接
  2. 用多线程并发 ping-pong（每个线程处理一批连接）
  3. 统计真实 QPS + P50/P99 延迟
"""

import socket
import struct
import time
import sys
import os
import resource
import subprocess
import threading
import statistics

resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))

HEADER_SIZE = 6
MSG_PING = 0x0001
MSG_PONG = 0x0002
TARGET_CONNS = 10000
NUM_THREADS = 20          # 20 个线程，每个处理 500 连接
MSGS_PER_CONN = 10        # 每个连接发 10 次 ping-pong
GW_PORT = 19000
BE_PORT = 17700

def pack_msg(msg_type, payload=b""):
    return struct.pack(">IH", HEADER_SIZE + len(payload), msg_type) + payload

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf

def recv_msg(sock):
    header = recv_exact(sock, HEADER_SIZE)
    total_len, msg_type = struct.unpack(">IH", header)
    payload = b""
    if total_len > HEADER_SIZE:
        payload = recv_exact(sock, total_len - HEADER_SIZE)
    return msg_type

def worker(conns, msgs_per_conn, results, tid):
    """每个线程对分配到的连接做 ping-pong"""
    latencies = []
    success = 0
    fail = 0

    for _ in range(msgs_per_conn):
        for s in conns:
            t0 = time.monotonic_ns()
            try:
                s.sendall(pack_msg(MSG_PING))
            except Exception:
                fail += 1
                continue

        for s in conns:
            try:
                s.settimeout(5)
                mt = recv_msg(s)
                t1 = time.monotonic_ns()
                if mt == MSG_PONG:
                    success += 1
                    latencies.append((t1 - t0) / 1_000_000)  # ms
                else:
                    fail += 1
            except Exception:
                fail += 1

    results[tid] = (success, fail, latencies)

def main():
    print(f"=== 1 万并发 QPS 基准测试 ===")
    print(f"连接数: {TARGET_CONNS}")
    print(f"线程数: {NUM_THREADS} (每线程 {TARGET_CONNS // NUM_THREADS} 连接)")
    print(f"每连接 ping-pong 次数: {MSGS_PER_CONN}")
    print(f"总消息数: {TARGET_CONNS * MSGS_PER_CONN:,}")
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
            print(f"      {i+1}/{TARGET_CONNS} ({time.time()-t0:.1f}s)")
    connect_time = time.time() - t0
    print(f"      完成: {len(conns)} 成功, {fail} 失败, {connect_time:.1f}s")
    print()

    # Phase 2: 多线程并发 ping-pong
    print(f"[2/3] 并发 ping-pong ({NUM_THREADS} 线程 x {MSGS_PER_CONN} 轮)...")
    conns_per_thread = len(conns) // NUM_THREADS
    threads = []
    results = [None] * NUM_THREADS

    t0 = time.monotonic()
    for tid in range(NUM_THREADS):
        start = tid * conns_per_thread
        end = start + conns_per_thread if tid < NUM_THREADS - 1 else len(conns)
        batch = conns[start:end]
        t = threading.Thread(target=worker, args=(batch, MSGS_PER_CONN, results, tid))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    elapsed = time.monotonic() - t0

    # Phase 3: 汇总结果
    total_success = 0
    total_fail = 0
    all_latencies = []
    for r in results:
        if r:
            total_success += r[0]
            total_fail += r[1]
            all_latencies.extend(r[2])

    total_msgs = TARGET_CONNS * MSGS_PER_CONN
    qps = total_success / elapsed if elapsed > 0 else 0

    if all_latencies:
        all_latencies.sort()
        p50 = all_latencies[len(all_latencies) // 2]
        p99 = all_latencies[int(len(all_latencies) * 0.99)]
        p999 = all_latencies[int(len(all_latencies) * 0.999)]
        avg = statistics.mean(all_latencies)
    else:
        p50 = p99 = p999 = avg = 0

    print(f"      完成: {total_success}/{total_msgs:,} 成功, {total_fail} 失败")
    print(f"      耗时: {elapsed:.2f}s")
    print()
    print(f"[3/3] 测试结果:")
    print(f"  连接数:       {len(conns):,}")
    print(f"  总消息数:     {total_msgs:,}")
    print(f"  成功:         {total_success:,}")
    print(f"  失败:         {total_fail:,}")
    print(f"  成功率:       {total_success/total_msgs*100:.1f}%")
    print(f"  耗时:         {elapsed:.2f}s")
    print(f"  QPS:          {qps:,.0f}")
    print(f"  平均延迟:     {avg:.3f}ms")
    print(f"  P50 延迟:     {p50:.3f}ms")
    print(f"  P99 延迟:     {p99:.3f}ms")
    print(f"  P999 延迟:    {p999:.3f}ms")

    # 清理
    for s in conns:
        try: s.close()
        except: pass
    gw.terminate()
    gw.wait()

    if total_success / total_msgs >= 0.95:
        print(f"\n  === PASS ===")
        sys.exit(0)
    else:
        print(f"\n  === FAIL (成功率 < 95%) ===")
        sys.exit(1)

if __name__ == "__main__":
    main()
