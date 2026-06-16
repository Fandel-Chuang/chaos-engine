#!/usr/bin/env python3
"""
ChaosEngine 高频压测客户端
连接 TCP Echo Server (7777)，疯狂发送数据，观察仪表盘变化
"""

import socket
import threading
import time
import random
import sys

HOST = '127.0.0.1'
PORT = 7777
NUM_CLIENTS = 20  # 并发连接数
MSG_SIZE = 1024   # 每条消息大小 (bytes)
RATE_PER_CLIENT = 100  # 每个连接每秒发送次数

def stress_worker(client_id, stop_event):
    """单个压测工作线程"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((HOST, PORT))
        print(f"[Client {client_id}] Connected")
    except Exception as e:
        print(f"[Client {client_id}] Connect failed: {e}")
        return

    msg = bytes([random.randint(0, 255) for _ in range(MSG_SIZE)])
    sent = 0
    recv = 0
    errors = 0
    start = time.time()

    try:
        while not stop_event.is_set():
            # 发送
            try:
                sock.sendall(msg)
                sent += 1
            except Exception:
                errors += 1
                break

            # 接收 echo
            try:
                data = sock.recv(MSG_SIZE)
                recv += 1
            except socket.timeout:
                errors += 1
            except Exception:
                errors += 1
                break

            # 限速
            time.sleep(1.0 / RATE_PER_CLIENT)
    except Exception:
        pass
    finally:
        elapsed = time.time() - start
        sock.close()
        rate = sent / elapsed if elapsed > 0 else 0
        print(f"[Client {client_id}] Done: sent={sent}, recv={recv}, errors={errors}, "
              f"rate={rate:.0f} msg/s, elapsed={elapsed:.1f}s")

def main():
    print(f"=== ChaosEngine Stress Client ===")
    print(f"Target: {HOST}:{PORT}")
    print(f"Clients: {NUM_CLIENTS}, Rate: {RATE_PER_CLIENT}/s per client")
    print(f"Msg size: {MSG_SIZE} bytes")
    print(f"Total throughput: ~{NUM_CLIENTS * RATE_PER_CLIENT * MSG_SIZE / 1024 / 1024:.1f} MB/s")
    print(f"Press Ctrl+C to stop\n")

    stop_event = threading.Event()
    threads = []

    for i in range(NUM_CLIENTS):
        t = threading.Thread(target=stress_worker, args=(i, stop_event))
        t.daemon = True
        t.start()
        threads.append(t)
        time.sleep(0.05)  # 错开连接

    try:
        while True:
            time.sleep(1)
            # 打印实时统计
            alive = sum(1 for t in threads if t.is_alive())
            print(f"\r[Active] {alive}/{NUM_CLIENTS} clients", end='', flush=True)
    except KeyboardInterrupt:
        print("\n\nStopping...")
        stop_event.set()
        for t in threads:
            t.join(timeout=5)
        print("Done.")

if __name__ == '__main__':
    main()
