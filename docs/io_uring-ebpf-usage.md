# ChaosEngine io_uring + eBPF 用户使用文档

> **版本：** v1.0 | **日期：** 2026-06-16 | **适用范围：** Linux (Kernel 5.1+)

---

## 目录

1. [概述](#1-概述)
2. [环境要求](#2-环境要求)
3. [CMake 启用方式](#3-cmake-启用方式)
4. [编译](#4-编译)
5. [运行测试程序](#5-运行测试程序)
6. [eBPF 输出解读](#6-ebpf-输出解读)
7. [配置参数](#7-配置参数)
8. [常见问题](#8-常见问题)

---

## 1. 概述

ChaosEngine 集成了 Linux 高性能异步 I/O（io_uring）和内核态可观测性（eBPF），提供：

- **io_uring 异步 I/O**：单线程处理数千并发连接，批量提交减少系统调用，支持零拷贝接收（ZCRX）和 Registered Buffers。
- **eBPF 可观测性**：零开销的函数延迟追踪、TCP 重传监控、I/O 延迟直方图。

两者均为**编译时可选**：通过 CMake option 控制，非 Linux 平台自动跳过，无需修改代码。

---

## 2. 环境要求

| 组件 | 最低版本 | 说明 |
|------|---------|------|
| Linux Kernel | 5.1+ | io_uring 基础；6.0+ 支持 ZCRX |
| liburing | 2.0+ | io_uring 用户态库 |
| libbpf | 1.0+ | eBPF 用户态加载库 |
| BTF | `/sys/kernel/btf/vmlinux` | eBPF CO-RE 所需 |
| clang | 11+ | 编译 BPF 程序（仅 eBPF） |
| bpftool | 5.x+ | （可选）调试 BPF 程序 |

### 检查当前环境

```bash
# 检查内核版本
uname -r

# 检查 liburing
pkg-config --modversion liburing

# 检查 libbpf
pkg-config --modversion libbpf

# 检查 BTF（eBPF 可用性）
ls -la /sys/kernel/btf/vmlinux

# 检查 io_uring 内核配置
grep CONFIG_IO_URING /boot/config-$(uname -r)
```

---

## 3. CMake 启用方式

### 3.1 自动检测（推荐）

在 Linux 上，CMake 会**自动检测** liburing 和 libbpf 是否安装。如果找到，自动启用对应功能：

```bash
cd chaos-engine
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

CMake 输出示例：

```
-- eBPF: enabled (libbpf 1.6.3)
-- io_uring: enabled (liburing 2.14)
```

### 3.2 手动控制

如需强制启用或禁用，使用 CMake option：

```bash
# 启用 io_uring + eBPF
cmake .. -DCHAOS_USE_IO_URING=ON -DCHAOS_USE_EBPF=ON

# 仅启用 io_uring，禁用 eBPF
cmake .. -DCHAOS_USE_IO_URING=ON -DCHAOS_USE_EBPF=OFF

# 全部禁用（使用 POSIX fallback）
cmake .. -DCHAOS_USE_IO_URING=OFF -DCHAOS_USE_EBPF=OFF
```

### 3.3 编译宏

编译时自动定义的宏：

| 宏 | 含义 |
|----|------|
| `CHAOS_HAS_IO_URING` | io_uring 后端已启用，`ce_async_uring.c` 被编译 |
| `CHAOS_HAS_EBPF` | eBPF 可观测性已启用，`ce_ebpf.c` 被编译 |

未定义时，对应功能使用 stub 实现（POSIX fallback / 空操作）。

---

## 4. 编译

### 4.1 完整构建

```bash
cd chaos-engine
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

构建产物：

| 产物 | 路径 | 说明 |
|------|------|------|
| `libengine_core.a` | `build/lib/` | 引擎核心静态库（含 io_uring + eBPF） |
| `chaos_async_echo` | `build/bin/` | io_uring 异步 Echo 服务器测试 |
| `chaos_server` | `build/bin/` | TCP Echo 服务器（POSIX 模式） |
| `ce_ebpf_kern.o` | `src_c/network/` | BPF 内核态字节码（运行时加载） |

### 4.2 仅编译引擎核心

```bash
cd build
make engine_core -j$(nproc)
```

### 4.3 编译 BPF 程序（手动）

BPF 程序需要单独编译（使用 clang）：

```bash
cd src_c/network
clang -O2 -target bpf -c ce_ebpf_kern.c -o ce_ebpf_kern.o
```

> **注意：** 运行时 `ce_ebpf_init()` 会从工作目录加载 `ce_ebpf_kern.o`，确保该文件在可访问路径下。

---

## 5. 运行测试程序

### 5.1 io_uring 异步 Echo 服务器

```bash
# 启动服务器（监听 7778 端口）
./build/bin/chaos_async_echo
```

输出示例：

```
========================================
  ChaosEngine Async Echo Server
  Backend: io_uring
  ZCRX: YES
  Listening on port 7778
========================================

[+] Client accepted (fd=8, slot=0)
```

在另一个终端测试：

```bash
# 使用 netcat 连接
nc localhost 7778
# 输入任意文本，服务器会回显
```

按 `Ctrl+C` 停止服务器：

```
Shutting down... (total echoes: 5)
Async echo server shut down cleanly.
```

### 5.2 eBPF 可观测性测试

```bash
# 需要 root 或 CAP_BPF 权限
sudo ./build/bin/chaos_ebpf_test
```

> **注意：** 当前 eBPF 测试程序未作为独立 CMake 目标。可通过以下方式编译运行：
> ```bash
> cd build && make chaos_ebpf_test -j$(nproc)
> sudo ./bin/chaos_ebpf_test
> ```

输出示例：

```
========================================
  ChaosEngine eBPF Observability Test
  BTF available: YES
========================================

✅ eBPF context created
✅ Function trace: ce_ecs_update
✅ TCP retransmit monitor
✅ I/O latency trace

Running for 2 seconds to collect data...
TCP retransmits: 0
I/O latency: P50=16us P90=64us P99=256us

🎉 eBPF observability test complete!
```

### 5.3 验证后端

```bash
# 检查编译时启用的后端
./build/bin/chaos_async_echo 2>&1 | grep Backend
# 输出: Backend: io_uring  或  Backend: posix
```

---

## 6. eBPF 输出解读

### 6.1 延迟直方图（函数追踪）

`ce_ebpf_dump_latency(ctx, "ce_ecs_update")` 输出示例：

```
=== Latency Histogram: ce_ecs_update ===
  <= 1 us: 150 samples
  <= 2 us: 320 samples
  <= 4 us: 210 samples
  <= 8 us: 85 samples
  <= 16 us: 42 samples
  <= 32 us: 8 samples
  <= 64 us: 3 samples
  Total samples: 818
```

**解读：**

- 使用**对数分桶**（1, 2, 4, 8, 16, 32, 64, ... μs），覆盖微秒到秒级延迟。
- `<= 2 us: 320 samples` 表示有 320 次 `ce_ecs_update` 调用在 2 微秒内完成。
- 如果高延迟桶（>64 μs）样本数多，说明函数存在性能瓶颈。
- 桶上限为 2^20 μs（约 1 秒），超过此值的延迟归入最大桶。

### 6.2 I/O 延迟百分位

`ce_ebpf_get_io_latency_stats(ctx, &p50, &p90, &p99)` 输出：

```
I/O latency: P50=16us P90=64us P99=256us
```

**解读：**

- **P50（中位数）**：50% 的 `recvfrom` 调用延迟 ≤ 16 μs。
- **P90**：90% 的调用延迟 ≤ 64 μs。
- **P99**：99% 的调用延迟 ≤ 256 μs。

**健康指标：**

| 指标 | 良好 | 警告 | 异常 |
|------|------|------|------|
| P50 | < 50 μs | 50-200 μs | > 200 μs |
| P90 | < 200 μs | 200-500 μs | > 500 μs |
| P99 | < 1 ms | 1-5 ms | > 5 ms |

### 6.3 TCP 重传计数

`ce_ebpf_get_retransmit_count(ctx)` 返回累计 TCP 重传次数：

```
TCP retransmits: 0
```

**解读：**

- **0**：网络健康，无丢包。
- **少量（<10/min）**：正常波动，TCP 的拥塞控制行为。
- **持续增长**：网络拥塞或链路质量问题，需要排查。

---

## 7. 配置参数

### 7.1 io_uring 参数

| 参数 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| `queue_depth` | 256 | 1 - 32768 | io_uring SQ 队列深度。更大的值支持更多并发 I/O 请求，但消耗更多内核内存。 |
| `BUFFER_SIZE` | 4096 | 256 - 65536 | 每个连接的接收缓冲区大小（字节）。 |
| `MAX_CLIENTS` | 32 | 1 - 1024 | 最大并发客户端数。 |

**调整示例：**

```c
// 高并发场景（1000 连接）
CeAsyncContext* async = ce_async_init(4096);  // 更大的队列深度
#define MAX_CLIENTS  1000
#define BUFFER_SIZE  8192  // 更大的缓冲区
```

### 7.2 eBPF 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `CE_EBPF_MAX_PROGS` | 16 | 最大 BPF 程序数 |
| `func_latency_hist.max_entries` | 1024 | 函数延迟直方图桶数 |
| `io_latency_hist.max_entries` | 256 | I/O 延迟直方图桶数 |

### 7.3 运行时检测

```c
// 检查当前使用的后端
const char* backend = ce_async_backend_name();
// "io_uring" 或 "posix"

// 检查 ZCRX 支持
CeBool has_zcrx = ce_async_has_zcrx();

// 检查 eBPF 可用性
CeBool ebpf_ok = ce_ebpf_available();
```

---

## 8. 常见问题

### 8.1 权限不足

**症状：**

```
❌ Failed to init eBPF (may need root/CAP_BPF)
```

**原因：** 加载 BPF 程序需要 `CAP_BPF` 或 `CAP_SYS_ADMIN` 权限。

**解决：**

```bash
# 方法 1：使用 sudo
sudo ./build/bin/chaos_ebpf_test

# 方法 2：授予 CAP_BPF capability
sudo setcap cap_bpf+ep ./build/bin/chaos_ebpf_test

# 方法 3：调整内核参数（不推荐生产环境）
sudo sysctl -w kernel.unprivileged_bpf_disabled=0
```

### 8.2 内核不支持 io_uring

**症状：**

```
Backend: posix
```

**原因：** 内核版本 < 5.1 或 liburing 未安装。

**解决：**

```bash
# 检查内核版本
uname -r  # 需要 >= 5.1

# 安装 liburing 开发包
sudo apt install liburing-dev  # Debian/Ubuntu
sudo dnf install liburing-devel  # Fedora

# 重新编译
cd build && cmake .. && make -j$(nproc)
```

### 8.3 BTF 不可用（eBPF 无法使用）

**症状：**

```
BTF available: NO
⚠️  BTF not available, skipping eBPF tests
```

**原因：** 内核未启用 `CONFIG_DEBUG_INFO_BTF`。

**解决：**

```bash
# 检查内核配置
grep CONFIG_DEBUG_INFO_BTF /boot/config-$(uname -r)
# 应输出: CONFIG_DEBUG_INFO_BTF=y

# 如果未启用，需要重新编译内核或升级到支持 BTF 的内核版本
```

### 8.4 BPF 对象文件找不到

**症状：**

```
Failed to open BPF object: No such file or directory
```

**原因：** `ce_ebpf_kern.o` 不在工作目录或 `src_c/network/` 下。

**解决：**

```bash
# 确保从项目根目录运行
cd /path/to/chaos-engine
sudo ./build/bin/chaos_ebpf_test

# 或手动编译 BPF 程序
cd src_c/network
clang -O2 -target bpf -c ce_ebpf_kern.c -o ce_ebpf_kern.o
```

### 8.5 编译错误：liburing/libbpf 未找到

**症状：**

```
Package 'liburing' not found
Package 'libbpf' not found
```

**解决：**

```bash
# Debian/Ubuntu
sudo apt install liburing-dev libbpf-dev clang

# Fedora
sudo dnf install liburing-devel libbpf-devel clang

# 从源码编译 libbpf（如果包管理器版本过旧）
git clone https://github.com/libbpf/libbpf.git
cd libbpf/src && make && sudo make install
```

### 8.6 macOS / Windows 上编译

io_uring 和 eBPF 是 Linux 专属技术。在 macOS/Windows 上编译时：

- CMake 自动跳过 liburing/libbpf 检测
- 使用 POSIX fallback（`select()` 模式）
- eBPF 函数全部为 stub 实现（返回 NULL/CE_ERR/0）
- 无需安装任何额外依赖

### 8.7 性能不如预期

**排查步骤：**

1. 确认后端为 `io_uring`（非 `posix`）：
   ```bash
   ./build/bin/chaos_async_echo 2>&1 | grep Backend
   ```

2. 检查 ZCRX 是否启用：
   ```bash
   ./build/bin/chaos_async_echo 2>&1 | grep ZCRX
   ```

3. 增大队列深度：
   ```c
   ce_async_init(4096);  // 从 256 增加到 4096
   ```

4. 使用 Registered Buffers 减少内存拷贝：
   ```c
   ce_async_register_buffers(ctx, buffer_pool, 4096, 64);
   ```

---

## 参考

- [io_uring 内核文档](https://kernel.docs.kernel.org/io_uring.html)
- [liburing GitHub](https://github.com/axboe/liburing)
- [libbpf GitHub](https://github.com/libbpf/libbpf)
- [eBPF 官方文档](https://ebpf.io/what-is-ebpf/)
- [开发者文档](io_uring-ebpf-dev.md)
- [技术规格书](../docs/spec/chaos-engine-io_uring-ebpf-spec-v0.1.md)
