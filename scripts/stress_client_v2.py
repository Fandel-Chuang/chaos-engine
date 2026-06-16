#!/usr/bin/env python3
"""
ChaosEngine 多维度压测客户端 v2
- 大包轰炸 (64KB, 10 连接)
- 小包高频 (64B, 20 连接, 200 msg/s)
- 断连重连 (5 连接, 每 3s 断开重连)
- 实时仪表盘查询
"""

import socket
import threading
import time
import random
import sys
import json
import urllib.request

HOST = '127.0.0.1'
PORT = 7777
DASHBOARD = 'http://localhost:9090/api'

class StressStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.sent = 0
        self.recv = 0
        self.errors = 0
        self.bytes_sent = 0
        self.bytes_recv = 0
        self.active_conns = 0

    def add(self, sent, recv, errors, bs, br):
        with self.lock:
            self.sent += sent
            self.recv += recv
            self.errors += errors
            self.bytes_sent += bs
            self.bytes_recv += br

    def set_conns(self, n):
        with self.lock:
            self.active_conns = n

    def snapshot(self):
        with self.lock:
            return (self.sent, self.recv, self.errors,
                    self.bytes_sent, self.bytes_recv, self.active_conns)

stats = StressStats()

def fetch_dashboard():
    """查询仪表盘 API"""
    try:
        req = urllib.request.Request(DASHBOARD + '/stats')
        resp = urllib.request.urlopen(req, timeout=2)
        d = json.loads(resp.read())
        return d.get('data', {})
    except:
        return {}

def large_packet_worker(client_id, stop_event):
    """大包轰炸：64KB 消息"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((HOST, PORT))
    except Exception as e:
        print(f"[Big-{client_id}] Connect failed: {e}")
        return

    msg = bytes([random.randint(0, 255) for _ in range(65536)])
    sent = recv = errors = 0
    bs = br = 0

    try:
        while not stop_event.is_set():
            try:
                sock.sendall(msg)
                sent += 1; bs += 65536
                data = sock.recv(65536)
                recv += 1; br += len(data)
            except socket.timeout:
                errors += 1
            except Exception:
                errors += 1
                break
            time.sleep(0.05)  # ~20 msg/s
    finally:
        sock.close()
        stats.add(sent, recv, errors, bs, br)

def small_packet_worker(client_id, stop_event):
    """小包高频：64B 消息，200 msg/s"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((HOST, PORT))
    except Exception as e:
        print(f"[Small-{client_id}] Connect failed: {e}")
        return

    msg = b'X' * 64
    sent = recv = errors = 0
    bs = br = 0

    try:
        while not stop_event.is_set():
            try:
                sock.sendall(msg)
                sent += 1; bs += 64
                data = sock.recv(64)
                recv += 1; br += len(data)
            except socket.timeout:
                errors += 1
            except Exception:
                errors += 1
                break
            time.sleep(0.005)  # ~200 msg/s
    finally:
        sock.close()
        stats.add(sent, recv, errors, bs, br)

def reconnect_worker(client_id, stop_event):
    """断连重连：每 3 秒断开重连"""
    sent = recv = errors = 0
    bs = br = 0
    msg = b'R' * 256

    while not stop_event.is_set():
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2)
            sock.connect((HOST, PORT))

            # 发几条消息
            for _ in range(10):
                if stop_event.is_set():
                    break
                try:
                    sock.sendall(msg)
                    sent += 1; bs += 256
                    data = sock.recv(256)
                    recv += 1; br += len(data)
                except:
                    errors += 1
                    break
                time.sleep(0.01)

            sock.close()
        except Exception as e:
            errors += 1

        time.sleep(3)  # 断开 3 秒后重连

    stats.add(sent, recv, errors, bs, br)

def monitor_thread(stop_event):
    """实时监控输出"""
    last_sent = last_recv = 0
    while not stop_event.is_set():
        time.sleep(2)
        s, r, e, bs, br, conns = stats.snapshot()
        ds = s - last_sent
        dr = r - last_recv
        last_sent, last_recv = s, r

        # 查询仪表盘
        d = fetch_dashboard()
        fps = d.get('fps', '?')
        uptime = d.get('uptime', 0)

        print(f"\r[Stress] sent={s}(+{ds}/2s) recv={r}(+{dr}/2s) err={e} "
              f"| {bs/1024/1024:.1f}MB↑ {br/1024/1024:.1f}MB↓ "
              f"| FPS={fps:.0f if isinstance(fps, float) else fps} "
              f"| uptime={uptime:.0f}s" if isinstance(uptime, float) else f"| uptime={uptime}",
              end='', flush=True)

def main():
    print(f"=== ChaosEngine 多维度压测 v2 ===")
    print(f"Target: {HOST}:{PORT}")
    print(f"  • 大包轰炸: 10 conns × 64KB × ~20 msg/s = ~12.8 MB/s")
    print(f"  • 小包高频: 20 conns × 64B × ~200 msg/s = ~0.25 MB/s")
    print(f"  • 断连重连: 5 conns, 每 3s 重连")
    print(f"Press Ctrl+C to stop\n")

    stop_event = threading.Event()
    threads = []

    # 启动监控线程
    mt = threading.Thread(target=monitor_thread, args=(stop_event,))
    mt.daemon = True
    mt.start()

    # 大包轰炸
    for i in range(10):
        t = threading.Thread(target=large_packet_worker, args=(i, stop_event))
        t.daemon = True; t.start(); threads.append(t)
        time.sleep(0.02)

    # 小包高频
    for i in range(20):
        t = threading.Thread(target=small_packet_worker, args=(i, stop_event))
        t.daemon = True; t.start(); threads.append(t)
        time.sleep(0.02)

    # 断连重连
    for i in range(5):
        t = threading.Thread(target=reconnect_worker, args=(i, stop_event))
        t.daemon = True; t.start(); threads.append(t)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\nStopping...")
        stop_event.set()
        for t in threads:
            t.join(timeout=3)
        print("Done.")

if __name__ == '__main__':
    main()
