#!/usr/bin/env python3
"""
ChaosEngine io_uring 异步 I/O 性能基准测试客户端

纯 Python 标准库实现（socket + threading + time），无外部依赖。

测试场景：
  - 单连接延迟测试：ping-pong N 次，统计 P50/P90/P99
  - 多连接并发 QPS 测试：N 个连接同时收发，统计总 QPS
  - 稳定性测试：长时间运行验证无崩溃

用法:
  python3 scripts/bench_async_io.py --mode uring --connections 100 --duration 30 --payload 64
  python3 scripts/bench_async_io.py --mode posix --connections 1 --duration 10 --payload 1024 --latency
  python3 scripts/bench_async_io.py --mode uring --connections 1000 --duration 60 --payload 64 --stability
"""

import socket
import threading
import time
import argparse
import json
import csv
import sys
import os
import math
from collections import defaultdict


# ============================================================
# 配置
# ============================================================

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 7778


# ============================================================
# 统计收集器
# ============================================================

class LatencyStats:
    """线程安全的延迟统计收集器"""

    def __init__(self):
        self.lock = threading.Lock()
        self.samples = []

    def record(self, latency_us):
        with self.lock:
            self.samples.append(latency_us)

    def percentile(self, pct):
        with self.lock:
            if not self.samples:
                return 0.0
            s = sorted(self.samples)
            idx = int(math.ceil(pct / 100.0 * len(s))) - 1
            return s[max(0, min(idx, len(s) - 1))]

    def summary(self):
        with self.lock:
            if not self.samples:
                return {
                    "count": 0, "min": 0, "max": 0,
                    "avg": 0, "p50": 0, "p90": 0, "p99": 0
                }
            s = sorted(self.samples)
            n = len(s)
            return {
                "count": n,
                "min": s[0],
                "max": s[-1],
                "avg": sum(s) / n,
                "p50": self._pct(s, 50),
                "p90": self._pct(s, 90),
                "p99": self._pct(s, 99),
            }

    @staticmethod
    def _pct(sorted_samples, pct):
        n = len(sorted_samples)
        idx = int(math.ceil(pct / 100.0 * n)) - 1
        return sorted_samples[max(0, min(idx, n - 1))]


class ThroughputStats:
    """线程安全的吞吐量统计收集器"""

    def __init__(self):
        self.lock = threading.Lock()
        self.total_requests = 0
        self.total_bytes = 0
        self.errors = 0
        self.start_time = None

    def start(self):
        with self.lock:
            self.start_time = time.time()

    def record(self, success, nbytes=0):
        with self.lock:
            if success:
                self.total_requests += 1
                self.total_bytes += nbytes
            else:
                self.errors += 1

    def snapshot(self):
        with self.lock:
            elapsed = time.time() - self.start_time if self.start_time else 0.001
            return {
                "total_requests": self.total_requests,
                "total_bytes": self.total_bytes,
                "errors": self.errors,
                "elapsed_s": elapsed,
                "qps": self.total_requests / elapsed if elapsed > 0 else 0,
                "throughput_mbps": (self.total_bytes / elapsed / 1024 / 1024) if elapsed > 0 else 0,
            }


# ============================================================
# 单连接延迟测试
# ============================================================

def latency_test(host, port, payload_size, num_pings, timeout=5):
    """
    单连接 ping-pong 延迟测试。
    发送一个请求，等待回显，记录往返时间。
    """
    stats = LatencyStats()
    payload = b"X" * payload_size

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        sock.connect((host, port))
    except Exception as e:
        print(f"[ERROR] Failed to connect: {e}", file=sys.stderr)
        return stats.summary()

    try:
        for i in range(num_pings):
            t0 = time.perf_counter()
            try:
                sock.sendall(payload)
                # 接收回显 — 可能需要多次 recv 才能收完
                received = b""
                while len(received) < payload_size:
                    chunk = sock.recv(payload_size - len(received))
                    if not chunk:
                        raise ConnectionError("Server closed connection")
                    received += chunk
                t1 = time.perf_counter()
                latency_us = (t1 - t0) * 1_000_000
                stats.record(latency_us)
            except socket.timeout:
                stats.record(-1)  # 标记超时
                print(f"[WARN] Ping {i} timed out", file=sys.stderr)
            except Exception as e:
                print(f"[ERROR] Ping {i} failed: {e}", file=sys.stderr)
                break
    finally:
        sock.close()

    return stats.summary()


# ============================================================
# 多连接并发 QPS 测试
# ============================================================

def qps_worker(conn_id, host, port, payload_size, duration, stop_event,
               tput_stats, lat_stats, warmup=2):
    """单个连接的工作线程：持续发送并接收回显"""
    payload = b"X" * payload_size
    local_requests = 0
    local_errors = 0
    local_bytes = 0

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)
        sock.connect((host, port))
    except Exception as e:
        tput_stats.record(False)
        return

    # 设置较短超时以便及时响应 stop_event
    sock.settimeout(0.5)

    try:
        while not stop_event.is_set():
            try:
                t0 = time.perf_counter()
                sock.sendall(payload)
                received = b""
                while len(received) < payload_size:
                    chunk = sock.recv(payload_size - len(received))
                    if not chunk:
                        raise ConnectionError("EOF")
                    received += chunk
                t1 = time.perf_counter()
                lat_us = (t1 - t0) * 1_000_000
                lat_stats.record(lat_us)
                local_requests += 1
                local_bytes += payload_size
            except socket.timeout:
                continue  # 检查 stop_event
            except Exception:
                local_errors += 1
                break
    finally:
        sock.close()
        # 批量提交统计
        for _ in range(local_requests):
            tput_stats.record(True, payload_size)
        for _ in range(local_errors):
            tput_stats.record(False)


def qps_test(host, port, payload_size, num_conns, duration, warmup=2):
    """
    多连接并发 QPS 测试。
    启动 N 个连接，每个连接持续发送/接收，统计总 QPS。
    """
    tput_stats = ThroughputStats()
    lat_stats = LatencyStats()
    stop_event = threading.Event()

    print(f"  Starting {num_conns} connections...")
    threads = []
    for i in range(num_conns):
        t = threading.Thread(
            target=qps_worker,
            args=(i, host, port, payload_size, duration, stop_event,
                  tput_stats, lat_stats, warmup),
            daemon=True
        )
        t.start()
        threads.append(t)
        # 错开连接建立，避免 SYN flood
        if num_conns > 100:
            time.sleep(0.001)

    tput_stats.start()
    print(f"  Running for {duration}s...")

    # 等待测试时长
    time.sleep(duration)
    stop_event.set()

    # 等待所有线程结束
    for t in threads:
        t.join(timeout=3)

    result = tput_stats.snapshot()
    result["latency"] = lat_stats.summary()
    result["connections"] = num_conns
    result["payload_size"] = payload_size
    return result


# ============================================================
# 稳定性测试
# ============================================================

def stability_test(host, port, payload_size, num_conns, duration):
    """
    稳定性测试：长时间运行，验证无崩溃。
    定期报告存活连接数和累计 QPS。
    """
    print(f"  Starting {num_conns} connections for {duration}s stability test...")

    tput_stats = ThroughputStats()
    lat_stats = LatencyStats()
    stop_event = threading.Event()
    active_conns = {"count": 0}
    conn_lock = threading.Lock()

    def stability_worker(cid):
        payload = b"X" * payload_size
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(3)
            sock.connect((host, port))
        except Exception:
            return

        with conn_lock:
            active_conns["count"] += 1

        sock.settimeout(0.5)
        local_req = 0
        try:
            while not stop_event.is_set():
                try:
                    t0 = time.perf_counter()
                    sock.sendall(payload)
                    received = b""
                    while len(received) < payload_size:
                        chunk = sock.recv(payload_size - len(received))
                        if not chunk:
                            raise ConnectionError("EOF")
                        received += chunk
                    t1 = time.perf_counter()
                    lat_stats.record((t1 - t0) * 1_000_000)
                    local_req += 1
                except socket.timeout:
                    continue
                except Exception:
                    break
        finally:
            sock.close()
            with conn_lock:
                active_conns["count"] -= 1
            for _ in range(local_req):
                tput_stats.record(True, payload_size)

    threads = []
    for i in range(num_conns):
        t = threading.Thread(target=stability_worker, args=(i,), daemon=True)
        t.start()
        threads.append(t)
        if num_conns > 100:
            time.sleep(0.001)

    tput_stats.start()

    # 定期报告
    report_interval = max(5, duration // 10)
    next_report = report_interval
    while time.time() - tput_stats.start_time < duration:
        time.sleep(1)
        elapsed = time.time() - tput_stats.start_time
        if elapsed >= next_report:
            snap = tput_stats.snapshot()
            print(f"  [{elapsed:.0f}s] alive={active_conns['count']}/{num_conns} "
                  f"QPS={snap['qps']:.0f} errors={snap['errors']}")
            next_report += report_interval

    stop_event.set()
    for t in threads:
        t.join(timeout=3)

    result = tput_stats.snapshot()
    result["latency"] = lat_stats.summary()
    result["connections"] = num_conns
    result["final_alive"] = active_conns["count"]
    return result


# ============================================================
# 输出格式化
# ============================================================

def format_latency_table(lat):
    """格式化延迟统计为表格字符串"""
    lines = []
    lines.append(f"  Samples:  {lat['count']}")
    lines.append(f"  Min:      {lat['min']:>10.1f} µs")
    lines.append(f"  Avg:      {lat['avg']:>10.1f} µs")
    lines.append(f"  Max:      {lat['max']:>10.1f} µs")
    lines.append(f"  P50:      {lat['p50']:>10.1f} µs")
    lines.append(f"  P90:      {lat['p90']:>10.1f} µs")
    lines.append(f"  P99:      {lat['p99']:>10.1f} µs")
    return "\n".join(lines)


def format_result(result, mode, test_type):
    """格式化测试结果为可读文本"""
    lines = []
    lines.append(f"\n{'='*60}")
    lines.append(f"  Benchmark: {test_type}")
    lines.append(f"  Mode:      {mode}")
    lines.append(f"  Duration:  {result.get('elapsed_s', 0):.1f}s")
    lines.append(f"  Payload:   {result.get('payload_size', '?')} bytes")
    lines.append(f"  Connections: {result.get('connections', '?')}")
    lines.append(f"{'='*60}")

    if "qps" in result:
        lines.append(f"  Total Requests: {result['total_requests']}")
        lines.append(f"  Errors:         {result['errors']}")
        lines.append(f"  QPS:            {result['qps']:.1f}")
        lines.append(f"  Throughput:     {result['throughput_mbps']:.2f} MB/s")

    if "latency" in result and result["latency"]["count"] > 0:
        lines.append(f"  --- Latency ---")
        lines.append(format_latency_table(result["latency"]))

    if "final_alive" in result:
        lines.append(f"  Final Alive:    {result['final_alive']}/{result['connections']}")

    return "\n".join(lines)


def output_json(results, filepath=None):
    """输出 JSON 格式结果"""
    if filepath:
        with open(filepath, "w") as f:
            json.dump(results, f, indent=2)
        print(f"  Results saved to {filepath}")
    else:
        print(json.dumps(results, indent=2))


def output_csv(results, filepath):
    """输出 CSV 格式结果"""
    flat_results = []
    for r in results:
        row = {
            "mode": r.get("mode", ""),
            "test_type": r.get("test_type", ""),
            "connections": r.get("connections", 0),
            "payload_size": r.get("payload_size", 0),
            "duration_s": r.get("elapsed_s", 0),
            "total_requests": r.get("total_requests", 0),
            "errors": r.get("errors", 0),
            "qps": r.get("qps", 0),
            "throughput_mbps": r.get("throughput_mbps", 0),
        }
        lat = r.get("latency", {})
        if lat:
            row.update({
                "lat_count": lat.get("count", 0),
                "lat_min_us": lat.get("min", 0),
                "lat_avg_us": lat.get("avg", 0),
                "lat_max_us": lat.get("max", 0),
                "lat_p50_us": lat.get("p50", 0),
                "lat_p90_us": lat.get("p90", 0),
                "lat_p99_us": lat.get("p99", 0),
            })
        flat_results.append(row)

    with open(filepath, "w", newline="") as f:
        if flat_results:
            writer = csv.DictWriter(f, fieldnames=flat_results[0].keys())
            writer.writeheader()
            writer.writerows(flat_results)
    print(f"  Results saved to {filepath}")


# ============================================================
# 主入口
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description="ChaosEngine io_uring Async I/O Benchmark Client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # 单连接延迟测试
  %(prog)s --mode uring --latency --pings 10000

  # 100 并发 QPS 测试
  %(prog)s --mode uring --connections 100 --duration 30 --payload 64

  # 1000 并发稳定性测试
  %(prog)s --mode uring --connections 1000 --duration 60 --stability

  # 输出 JSON
  %(prog)s --mode uring --connections 100 --duration 30 --output results.json
        """
    )

    parser.add_argument("--mode", choices=["uring", "posix"], default="uring",
                        help="I/O backend mode (default: uring)")
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"Server host (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"Server port (default: {DEFAULT_PORT})")
    parser.add_argument("--connections", "-c", type=int, default=1,
                        help="Number of concurrent connections (default: 1)")
    parser.add_argument("--duration", "-d", type=int, default=30,
                        help="Test duration in seconds (default: 30)")
    parser.add_argument("--payload", "-s", type=int, default=64,
                        help="Payload size in bytes (default: 64)")
    parser.add_argument("--latency", action="store_true",
                        help="Run single-connection latency test")
    parser.add_argument("--pings", type=int, default=10000,
                        help="Number of ping-pong rounds for latency test (default: 10000)")
    parser.add_argument("--stability", action="store_true",
                        help="Run stability test (long-duration, check for crashes)")
    parser.add_argument("--output", "-o", default=None,
                        help="Output file path (.json or .csv)")
    parser.add_argument("--format", choices=["json", "csv"], default="json",
                        help="Output format (default: json)")
    parser.add_argument("--warmup", type=int, default=2,
                        help="Warmup seconds before measuring (default: 2)")

    args = parser.parse_args()

    all_results = []

    # ---- 延迟测试 ----
    if args.latency:
        print(f"\n[Latency Test] mode={args.mode} pings={args.pings} payload={args.payload}")
        lat = latency_test(args.host, args.port, args.payload, args.pings)
        result = {
            "mode": args.mode,
            "test_type": "latency",
            "connections": 1,
            "payload_size": args.payload,
            "pings": args.pings,
            "latency": lat,
        }
        all_results.append(result)
        print(format_result(result, args.mode, "Latency (Ping-Pong)"))

    # ---- QPS 测试 ----
    if not args.latency and not args.stability:
        # 默认：多连接 QPS 测试
        print(f"\n[QPS Test] mode={args.mode} conns={args.connections} "
              f"duration={args.duration}s payload={args.payload}")
        result = qps_test(args.host, args.port, args.payload,
                          args.connections, args.duration, args.warmup)
        result["mode"] = args.mode
        result["test_type"] = "qps"
        all_results.append(result)
        print(format_result(result, args.mode, "Concurrent QPS"))

    # ---- 稳定性测试 ----
    if args.stability:
        print(f"\n[Stability Test] mode={args.mode} conns={args.connections} "
              f"duration={args.duration}s payload={args.payload}")
        result = stability_test(args.host, args.port, args.payload,
                                args.connections, args.duration)
        result["mode"] = args.mode
        result["test_type"] = "stability"
        all_results.append(result)
        print(format_result(result, args.mode, "Stability"))

    # ---- 输出 ----
    if args.output:
        if args.output.endswith(".csv") or args.format == "csv":
            outpath = args.output if args.output.endswith(".csv") else args.output + ".csv"
            output_csv(all_results, outpath)
        else:
            outpath = args.output if args.output.endswith(".json") else args.output + ".json"
            output_json(all_results, outpath)
    elif all_results:
        # 默认打印 JSON 摘要
        print("\n[JSON Summary]")
        output_json(all_results)


if __name__ == "__main__":
    main()
