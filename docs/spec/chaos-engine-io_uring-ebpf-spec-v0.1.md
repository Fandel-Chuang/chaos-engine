# ChaosEngine io_uring + eBPF 技术分析规格书 v0.1

> **状态：** 草案（Phase 1-3 已实现，Phase 4-5 延后） | **日期：** 2026-06-15（初稿）/ 2026-06-16（更新） | **作者：** zhongfangdao
>
> **主题：** Linux 高性能网络 I/O（io_uring）+ 可观测性/网络优化（eBPF），Windows 对应 iocp
>
> ---
>
> ## 实现状态（2026-06-16 更新）
>
> | Phase | 内容 | 状态 |
> |-------|------|:----:|
> | Phase 1 | io_uring 基础集成（ce_async_io.h, ce_async_uring.c, POSIX fallback） | ✅ 已完成 |
> | Phase 2 | io_uring 高级特性（Registered Buffers, ZCRX, 批量提交） | ✅ 已完成 |
> | Phase 3 | eBPF 可观测性（kprobe 延迟追踪, TCP 重传监控, I/O 延迟直方图） | ✅ 已完成 |
> | Phase 4 | eBPF 网络优化（XDP, BPF Stream Parser, BPF LSM） | ⏳ 延后 |
> | Phase 5 | 集成与文档（服务端主循环切换, 性能测试, 用户/开发者文档） | 🔧 部分完成 |
>
> **已实现文件：**
> - `src_c/network/ce_async_io.h` — 异步 I/O 抽象层接口
> - `src_c/network/ce_async_uring.c` — io_uring 后端（257 行）
> - `src_c/network/ce_async_posix.c` — POSIX fallback（240 行）
> - `src_c/network/ce_ebpf.h` — eBPF 可观测性接口
> - `src_c/network/ce_ebpf.c` — eBPF 用户态加载器（275 行）
> - `src_c/network/ce_ebpf_kern.c` — BPF 内核态程序（152 行）
> - `src_c/runtime/ce_async_echo_main.c` — io_uring Echo 测试
> - `src_c/runtime/ce_ebpf_test_main.c` — eBPF 可观测性测试
>
> **文档：**
> - `docs/io_uring-ebpf-usage.md` — 用户使用文档
> - `docs/io_uring-ebpf-dev.md` — 开发者文档
> - `openspec/changes/io-uring-ebpf/` — 变更提案/设计/任务/规格
>
> **API 差异（实现 vs 原始设计）：**
> - `ce_ebpf_init()` 返回 `CeEbpfContext*`（非 `CeResult`），失败返回 NULL
> - `ce_ebpf_trace_function()` 增加 `CeEbpfContext* ctx` 参数
> - `ce_ebpf_dump_latency()` 增加 `CeEbpfContext* ctx` 参数，返回 `int`（采样数）
> - `ce_ebpf_get_io_latency_stats()` 增加 `CeEbpfContext* ctx` 参数
> - 新增 `ce_async_backend_name()` 函数（调试用）
>
> ---

---

## 目录

1. [环境基线](#1-环境基线)
2. [io_uring 在 ChaosEngine 中的应用](#2-io_uring-在-chaosengine-中的应用)
3. [eBPF 在 ChaosEngine 中的应用](#3-ebpf-在-chaosengine-中的应用)
4. [跨平台策略：Linux io_uring ↔ Windows iocp](#4-跨平台策略linux-io_uring--windows-iocp)
5. [架构设计](#5-架构设计)
6. [API 设计](#6-api-设计)
7. [实现路线图](#7-实现路线图)
8. [风险与注意事项](#8-风险与注意事项)

---

## 1. 环境基线

### 1.1 当前系统

| 组件 | 版本/状态 |
|------|-----------|
| Linux Kernel | 7.0.0-22-generic |
| liburing | 2.14（运行时 + 开发包） |
| libbpf | 1.6.3（运行时 + 开发包） |
| bpftool | 7.7.0 |
| BTF | ✅ `/sys/kernel/btf/vmlinux` |
| BPF JIT | ✅ `CONFIG_BPF_JIT_ALWAYS_ON=y` |
| io_uring BPF | ✅ `CONFIG_IO_URING_BPF=y` |
| io_uring ZCRX | ✅ `CONFIG_IO_URING_ZCRX=y`（零拷贝接收） |

### 1.2 关键内核特性

```
CONFIG_IO_URING=y              # io_uring 核心
CONFIG_IO_URING_BPF=y          # io_uring + BPF 联动（可编程 I/O）
CONFIG_IO_URING_ZCRX=y         # 零拷贝网络接收
CONFIG_BPF_JIT_ALWAYS_ON=y     # BPF JIT 始终开启
CONFIG_BPF_SYSCALL=y           # BPF 系统调用
CONFIG_BPF_STREAM_PARSER=y     # BPF 流解析器（协议解析加速）
CONFIG_BPF_EVENTS=y            # BPF 事件（perf 集成）
```

### 1.3 当前网络层现状

| 文件 | 行数 | 实现 |
|------|------|------|
| `ce_network.h` | 76 | TCP/UDP Socket API（POSIX） |
| `ce_network.c` | 267 | Linux/POSIX 实现（阻塞 + 非阻塞） |

**现状问题：**
- 使用 `accept()`/`recv()`/`send()` 阻塞/非阻塞模式
- 无异步 I/O 框架，每个连接需要独立线程或忙轮询
- 无零拷贝路径，数据在内核态和用户态之间多次拷贝
- 无可观测性基础设施

---

## 2. io_uring 在 ChaosEngine 中的应用

### 2.1 核心价值

```
传统 POSIX I/O:
  用户态 → syscall → 内核态 → 拷贝 → 用户态
  每次 I/O 操作至少 2 次上下文切换

io_uring:
  用户态 ←→ 共享环形缓冲区 (SQ/CQ) ←→ 内核态
  批量提交 + 批量完成，零/少上下文切换
```

| 指标 | POSIX (epoll) | io_uring |
|------|---------------|----------|
| 上下文切换 | 每次 I/O 至少 1 次 | 批量操作 0 次 |
| 内存拷贝 | 2 次（内核↔用户） | 1 次（registered buffers） |
| 系统调用 | 每次 I/O 1 次 | 批量提交 1 次 |
| 最大 QPS（echo） | ~50K | ~200K+ |
| 延迟（P99） | ~500μs | ~100μs |

### 2.2 应用场景

#### 场景 1：服务端网络 I/O（核心）

```
当前: ce_server_main.c 中 accept() + recv() + send() 循环
目标: io_uring 驱动的异步 accept/recv/send，单线程处理数千连接
```

**收益：**
- 单线程处理数千并发连接（游戏服务器典型场景）
- 减少线程切换开销（MMO 服务器通常有大量空闲连接）
- 批量 accept 减少惊群效应

#### 场景 2：零拷贝网络接收（ZCRX）

```
CONFIG_IO_URING_ZCRX=y

传统路径:
  NIC → sk_buff → 内核缓冲区 → 用户态缓冲区（2 次拷贝）

ZCRX 路径:
  NIC → 用户态缓冲区（0 次拷贝，直接 DMA 到用户态页）
```

**收益：**
- 大包（>4KB）吞吐量提升 30-50%
- CPU 利用率降低 20-30%
- 特别适合游戏服务器中大量小包的场景

#### 场景 3：异步文件 I/O（资源加载）

```
当前: fopen()/fread() 同步阻塞
目标: io_uring 异步读取资源文件（纹理、模型、配置）
```

**收益：**
- 资源预加载不阻塞主线程
- 批量文件读取合并为单次提交
- 编辑器导入大文件时体验更流畅

#### 场景 4：Registered Buffers（内存池固定）

```
io_uring 允许预先注册内存缓冲区：
- 避免每次 I/O 的内核页锁定/解锁
- 减少内存拷贝
- 适合固定大小的网络包缓冲区
```

### 2.3 不做的事

- ❌ 不替换所有 POSIX I/O（保留 fallback 路径）
- ❌ 不在非 Linux 平台使用 io_uring
- ❌ 不强制要求内核 5.1+（编译时检测 + 运行时 fallback）

---

## 3. eBPF 在 ChaosEngine 中的应用

### 3.1 核心价值

```
eBPF = 内核态可编程沙箱
- 无需修改内核代码
- 无需加载内核模块
- JIT 编译为原生指令，性能接近内核原生代码
- 安全：验证器保证不会崩溃内核
```

### 3.2 应用场景

#### 场景 1：网络包过滤与负载均衡（XDP/TC）

```
XDP (eXpress Data Path):
  NIC 驱动层直接处理包，在内核网络栈之前

应用：
  - DDoS 防护：在网卡层丢弃恶意包
  - 连接分发：根据协议类型/玩家 ID 分发到不同服务线程
  - 协议加速：在 XDP 层解析自定义协议头，跳过内核协议栈
```

```
数据流:
  NIC → XDP(eBPF) → 用户态（跳过内核网络栈）
  延迟：~10μs（传统路径 ~50μs）
```

#### 场景 2：可观测性（perf_event / kprobe / tracepoint）

```
应用：
  - 函数级延迟追踪：kprobe 挂载到 ce_ecs_update、ce_aoi_move 等关键函数
  - 内存分配追踪：追踪 ce_memory 分配器的分配/释放模式
  - I/O 延迟直方图：记录每次 recv/send 的延迟分布
  - 丢包检测：kprobe 挂载到 tcp_drop/tcp_retransmit_skb
```

**收益：**
- 零开销的生产环境 profiling（eBPF 开销 <1%）
- 无需重新编译即可添加追踪点
- 与 Prometheus/Grafana 集成（通过 BPF CO-RE）

#### 场景 3：自定义协议加速（BPF Stream Parser）

```
CONFIG_BPF_STREAM_PARSER=y

应用：
  - TCP 流中解析 ChaosEngine 自定义协议头
  - 在 socket 层完成消息分帧，减少用户态拷贝
  - 与 io_uring 联动：BPF 解析 → io_uring 直接投递到正确缓冲区
```

#### 场景 4：安全沙箱（BPF LSM）

```
CONFIG_BPF_LSM=y

应用：
  - 限制 Lua 脚本的文件系统访问
  - 限制插件的系统调用
  - 防止恶意脚本读取敏感文件
```

### 3.3 不做的事

- ❌ 不依赖 eBPF 做核心业务逻辑（eBPF 是旁路增强）
- ❌ 不在非 Linux 平台使用 eBPF
- ❌ 不要求用户编译内核模块

---

## 4. 跨平台策略：Linux io_uring ↔ Windows iocp

### 4.1 抽象层设计

```
┌─────────────────────────────────────────────────────────┐
│                  ce_async_io (抽象层)                     │
│  统一异步 I/O 接口：accept/recv/send/read/write          │
├─────────────────────────┬───────────────────────────────┤
│   ce_async_io_uring.c   │   ce_async_io_iocp.c          │
│   Linux io_uring 后端    │   Windows IOCP 后端           │
│   (liburing)            │   (I/O Completion Ports)      │
├─────────────────────────┴───────────────────────────────┤
│              平台检测 + 编译时选择                        │
│   #ifdef __linux__ → io_uring                           │
│   #ifdef _WIN32    → iocp                               │
│   #else            → POSIX fallback (epoll/kqueue)      │
└─────────────────────────────────────────────────────────┘
```

### 4.2 各平台能力矩阵

| 能力 | Linux io_uring | Windows iocp | macOS kqueue | POSIX fallback |
|------|:---:|:---:|:---:|:---:|
| 异步 TCP accept | ✅ | ✅ | ❌ | ❌ |
| 异步 TCP recv/send | ✅ | ✅ | ❌ | ❌ |
| 异步文件 I/O | ✅ | ✅ | ✅ (aio) | ❌ |
| 零拷贝接收 | ✅ ZCRX | ❌ | ❌ | ❌ |
| Registered Buffers | ✅ | ❌ | ❌ | ❌ |
| 批量提交 | ✅ | ❌ | ❌ | ❌ |
| eBPF 可观测性 | ✅ | ❌ | ❌ | ❌ |
| eBPF 包过滤 | ✅ XDP | ❌ | ❌ | ❌ |

### 4.3 实现优先级

| 优先级 | 平台 | 说明 |
|--------|------|------|
| P0 | Linux io_uring | 当前开发环境，服务端部署主力 |
| P1 | POSIX fallback | 兼容旧内核、macOS、其他 Unix |
| P2 | Windows iocp | 客户端 + Windows 服务器 |

---

## 5. 架构设计

### 5.1 模块分层

```
┌─────────────────────────────────────────────────────────┐
│                   应用层                                  │
│  ce_server_main.c  /  ce_client_main.c                  │
├─────────────────────────────────────────────────────────┤
│                  ce_network (网络层)                      │
│  ce_socket_accept/connect/send/recv                      │
│  ce_net_send_message/recv_message (长度前缀协议)          │
├─────────────────────────────────────────────────────────┤
│               ce_async_io (新增异步 I/O 层)               │
│  ce_async_init / ce_async_submit / ce_async_wait         │
│  ce_async_accept / ce_async_recv / ce_async_send         │
├─────────────────────────┬───────────────────────────────┤
│   ce_async_uring.c      │   ce_async_posix.c            │
│   (io_uring 后端)        │   (POSIX fallback)            │
├─────────────────────────┴───────────────────────────────┤
│                  ce_ebpf (新增 eBPF 层)                   │
│  ce_ebpf_observe / ce_ebpf_filter / ce_ebpf_trace       │
└─────────────────────────────────────────────────────────┘
```

### 5.2 io_uring 事件循环模型

```
单线程事件循环（适合游戏服务器）：

void server_loop(void) {
    ce_async_init(4096);  // 4096 entries SQ

    // 预注册监听 socket
    ce_async_accept_prep(listen_sock);

    while (running) {
        // 1. 提交所有待处理的 I/O 请求
        ce_async_submit();

        // 2. 等待完成事件（可设超时，期间处理 ECS 更新）
        int n = ce_async_wait(1);  // 至少等待 1 个完成事件

        // 3. 处理完成事件
        for (int i = 0; i < n; i++) {
            CeAsyncEvent* ev = ce_async_get_event(i);
            switch (ev->type) {
            case CE_ASYNC_ACCEPT:
                on_new_connection(ev->client_fd);
                ce_async_recv_prep(ev->client_fd, buf, size);
                break;
            case CE_ASYNC_RECV:
                on_data_received(ev->fd, ev->buf, ev->nbytes);
                ce_async_recv_prep(ev->fd, buf, size);  // 继续接收
                break;
            case CE_ASYNC_SEND:
                on_data_sent(ev->fd, ev->nbytes);
                break;
            }
        }

        // 4. ECS 更新 + Cell 管理（在等待 I/O 的间隙执行）
        ce_ecs_update(dt);
        ce_cell_update();
    }
}
```

### 5.3 eBPF 可观测性架构

```
┌─────────────────────────────────────────────────────────┐
│                   用户态                                  │
│  ce_ebpf_observe_init()                                 │
│  ce_ebpf_observe_read()  ← 从 BPF map 读取统计数据       │
├─────────────────────────────────────────────────────────┤
│                   内核态 (eBPF)                           │
│  ┌──────────────────────────────────────────────────┐   │
│  │ kprobe:ce_ecs_update                             │   │
│  │   → 记录函数调用延迟（直方图）                      │   │
│  ├──────────────────────────────────────────────────┤   │
│  │ kprobe:tcp_retransmit_skb                        │   │
│  │   → 记录 TCP 重传事件（按连接分组）                 │   │
│  ├──────────────────────────────────────────────────┤   │
│  │ tracepoint:syscalls:sys_enter_recvfrom            │   │
│  │   → 记录 recv 延迟分布                            │   │
│  ├──────────────────────────────────────────────────┤   │
│  │ BPF_MAP_TYPE_HASH: io_latency                    │   │
│  │ BPF_MAP_TYPE_HISTOGRAM: func_duration             │   │
│  │ BPF_MAP_TYPE_PERF_EVENT_ARRAY: events             │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

---

## 6. API 设计

### 6.1 异步 I/O 层 (ce_async_io.h)

```c
/*
 * ChaosEngine 异步 I/O 抽象层
 * Linux: io_uring | Windows: iocp | Fallback: POSIX
 */

#ifndef CE_ASYNC_IO_H
#define CE_ASYNC_IO_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明句柄 ---- */

typedef struct CeAsyncContext CeAsyncContext;

/* ---- 事件类型 ---- */

typedef enum CeAsyncEventType {
    CE_ASYNC_ACCEPT = 0,
    CE_ASYNC_RECV,
    CE_ASYNC_SEND,
    CE_ASYNC_READ,     /* 文件读取 */
    CE_ASYNC_WRITE,    /* 文件写入 */
    CE_ASYNC_CLOSE,
    CE_ASYNC_ERROR,
} CeAsyncEventType;

typedef struct CeAsyncEvent {
    CeAsyncEventType type;
    int              fd;           /* 关联的文件描述符 */
    int              client_fd;    /* accept 时的新连接 fd */
    void*            buf;          /* 数据缓冲区 */
    int              nbytes;       /* 实际传输字节数 */
    int              error;        /* 错误码（0 = 成功） */
    void*            user_data;    /* 用户自定义数据 */
} CeAsyncEvent;

/* ---- 生命周期 ---- */

/** 初始化异步 I/O 上下文 */
CeAsyncContext* ce_async_init(int queue_depth);

/** 关闭异步 I/O 上下文 */
void ce_async_shutdown(CeAsyncContext* ctx);

/* ---- 操作提交 ---- */

/** 提交 accept 请求 */
void ce_async_accept(CeAsyncContext* ctx, int listen_fd, void* user_data);

/** 提交 recv 请求 */
void ce_async_recv(CeAsyncContext* ctx, int fd, void* buf, int size, void* user_data);

/** 提交 send 请求 */
void ce_async_send(CeAsyncContext* ctx, int fd, const void* buf, int size, void* user_data);

/** 提交文件读取请求 */
void ce_async_read(CeAsyncContext* ctx, int fd, void* buf, int size, off_t offset, void* user_data);

/** 提交关闭请求 */
void ce_async_close(CeAsyncContext* ctx, int fd);

/* ---- 事件处理 ---- */

/** 提交所有待处理的请求到内核 */
int ce_async_submit(CeAsyncContext* ctx);

/** 等待完成事件（返回事件数量） */
int ce_async_wait(CeAsyncContext* ctx, int min_events, int timeout_ms);

/** 获取第 N 个完成事件 */
const CeAsyncEvent* ce_async_get_event(CeAsyncContext* ctx, int index);

/* ---- 高级特性（io_uring 专属） ---- */

/** 注册固定缓冲区（减少内存拷贝） */
CeResult ce_async_register_buffers(CeAsyncContext* ctx, void* buf, int buf_size, int buf_count);

/** 是否支持零拷贝接收 */
CeBool ce_async_has_zcrx(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_ASYNC_IO_H */
```

### 6.2 eBPF 可观测性层 (ce_ebpf.h)

```c
/*
 * ChaosEngine eBPF 可观测性层
 * Linux 专属，编译时可选
 */

#ifndef CE_EBPF_H
#define CE_EBPF_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 ---- */

/** 初始化 eBPF 可观测性（加载 BPF 程序到内核） */
CeResult ce_ebpf_init(void);

/** 关闭 eBPF 可观测性（卸载 BPF 程序） */
void ce_ebpf_shutdown(void);

/* ---- 函数追踪 ---- */

/** 开始追踪指定函数的延迟 */
CeResult ce_ebpf_trace_function(const char* func_name);

/** 读取函数延迟直方图（输出到日志） */
void ce_ebpf_dump_latency_histogram(const char* func_name);

/* ---- 网络观测 ---- */

/** 开始记录 TCP 重传事件 */
CeResult ce_ebpf_trace_tcp_retransmit(void);

/** 获取 TCP 重传统计 */
int ce_ebpf_get_retransmit_count(int fd);

/* ---- I/O 观测 ---- */

/** 开始记录 I/O 延迟 */
CeResult ce_ebpf_trace_io_latency(void);

/** 获取 I/O 延迟统计（P50/P90/P99，微秒） */
void ce_ebpf_get_io_latency_stats(int* p50, int* p90, int* p99);

#ifdef __cplusplus
}
#endif

#endif /* CE_EBPF_H */
```

---

## 7. 实现路线图

```
Phase 1: io_uring 基础集成 (~8h)
  ├─ 1.1 ce_async_io.h 接口定义
  ├─ 1.2 ce_async_uring.c 实现（accept/recv/send）
  ├─ 1.3 POSIX fallback 实现（复用现有 ce_network.c）
  ├─ 1.4 单元测试：单连接 echo、多连接并发
  └─ 1.5 提交: [feat](network): io_uring 异步 I/O 层

Phase 2: io_uring 高级特性 (~6h)
  ├─ 2.1 Registered Buffers（内存池固定）
  ├─ 2.2 零拷贝接收（ZCRX）
  ├─ 2.3 批量提交优化
  ├─ 2.4 性能基准测试（对比 POSIX）
  └─ 2.5 提交: [feat](network): io_uring 零拷贝 + 批量优化

Phase 3: eBPF 可观测性 (~6h)
  ├─ 3.1 ce_ebpf.h 接口定义
  ├─ 3.2 BPF 程序：函数延迟追踪（kprobe）
  ├─ 3.3 BPF 程序：TCP 重传监控
  ├─ 3.4 BPF 程序：I/O 延迟直方图
  ├─ 3.5 用户态读取 BPF map 数据
  └─ 3.6 提交: [feat](network): eBPF 可观测性框架

Phase 4: eBPF 网络优化 (~8h)
  ├─ 4.1 XDP 包过滤（DDoS 基础防护）
  ├─ 4.2 BPF Stream Parser（协议加速）
  ├─ 4.3 io_uring + BPF 联动（BPF 解析 → io_uring 投递）
  └─ 4.4 提交: [feat](network): eBPF XDP + 协议加速

Phase 5: 集成 + 文档 (~4h)
  ├─ 5.1 服务端主循环切换到 io_uring 事件循环
  ├─ 5.2 性能测试报告
  ├─ 5.3 更新 spec 文档
  └─ 5.4 提交: [feat](engine): 服务端切换到 io_uring + eBPF
```

---

## 8. 风险与注意事项

### 8.1 内核版本依赖

| 特性 | 最低内核版本 | 当前内核 | 状态 |
|------|-------------|----------|------|
| io_uring 基础 | 5.1 | 7.0 | ✅ |
| io_uring BPF | 5.13 | 7.0 | ✅ |
| io_uring ZCRX | 6.0 | 7.0 | ✅ |
| BPF CO-RE | 5.4 | 7.0 | ✅ |
| BPF Stream Parser | 4.19 | 7.0 | ✅ |

### 8.2 编译时检测

```cmake
# CMakeLists.txt
option(CHAOS_USE_IO_URING "Enable io_uring async I/O (Linux)" ON)
option(CHAOS_USE_EBPF "Enable eBPF observability (Linux)" ON)

if(CHAOS_USE_IO_URING AND LINUX)
    find_package(PkgConfig)
    pkg_check_modules(LIBURING liburing)
    if(LIBURING_FOUND)
        add_definitions(-DCHAOS_HAS_IO_URING)
        target_link_libraries(engine_core PRIVATE ${LIBURING_LIBRARIES})
    endif()
endif()
```

### 8.3 运行时 Fallback

```c
// 运行时检测 io_uring 是否可用
CeBool ce_async_uring_available(void) {
    // 尝试创建 io_uring 实例
    struct io_uring ring;
    return io_uring_queue_init(1, &ring, 0) == 0
           ? (io_uring_queue_exit(&ring), CE_TRUE)
           : CE_FALSE;
}
```

### 8.4 安全注意事项

- eBPF 程序需要 `CAP_BPF` 或 `CAP_SYS_ADMIN` 权限
- 生产环境建议使用 signed BPF 程序
- io_uring 需要 `kernel.io_uring_group` 或 `CAP_SYS_ADMIN`（当前系统值为 -1，表示无限制）

---

> **下一步：** 审阅确认后，按 Phase 1 开始实现 io_uring 异步 I/O 层。
