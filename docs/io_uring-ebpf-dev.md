# ChaosEngine io_uring + eBPF 开发者文档

> **版本：** v1.0 | **日期：** 2026-06-16 | **目标读者：** ChaosEngine 贡献者

---

## 目录

1. [架构概览](#1-架构概览)
2. [代码导航](#2-代码导航)
3. [如何添加新 BPF 程序](#3-如何添加新-bpf-程序)
4. [如何添加新异步 I/O 后端](#4-如何添加新异步-io-后端)
5. [跨平台注意事项](#5-跨平台注意事项)
6. [内部设计细节](#6-内部设计细节)

---

## 1. 架构概览

### 1.1 三层架构

```
┌─────────────────────────────────────────────────────────┐
│                   应用层                                  │
│  ce_server_main.c  /  ce_async_echo_main.c              │
│  ce_ebpf_test_main.c                                    │
├─────────────────────────────────────────────────────────┤
│               ce_async_io.h (抽象层)                      │
│  统一接口: ce_async_init / submit / wait / get_event     │
│  操作接口: ce_async_accept / recv / send / read / close  │
├─────────────────────────┬───────────────────────────────┤
│   ce_async_uring.c      │   ce_async_posix.c            │
│   io_uring 后端          │   POSIX fallback              │
│   (liburing 2.x)        │   (select + 非阻塞 socket)     │
├─────────────────────────┴───────────────────────────────┤
│                  ce_ebpf (eBPF 可观测性层)                │
│  ┌──────────────────────────────────────────────────┐   │
│  │ ce_ebpf.h          — 用户态 API                    │   │
│  │ ce_ebpf.c          — 用户态加载器 (libbpf)         │   │
│  │ ce_ebpf_kern.c     — BPF 内核态程序                │   │
│  │ ce_ebpf_kern.o     — 编译后的 BPF 字节码           │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### 1.2 设计原则

1. **抽象层隔离**：`ce_async_io.h` 定义统一接口，调用方不感知底层实现。
2. **编译时选择**：通过 `#ifdef CHAOS_HAS_IO_URING` / `#ifdef CHAOS_HAS_EBPF` 条件编译。
3. **运行时降级**：io_uring 初始化失败时自动使用 POSIX fallback；eBPF 不可用时所有函数返回 stub。
4. **单线程事件循环**：I/O 和 ECS 更新在同一线程，避免锁竞争。
5. **向后兼容**：现有 `ce_network.h/c` 保持不变，异步 I/O 层为新增模块。

### 1.3 数据流

```
用户代码
  │
  ├─ ce_async_accept(ctx, fd, user_data)   ──→ 准备 SQE（io_uring）或记录操作（POSIX）
  ├─ ce_async_recv(ctx, fd, buf, sz, ud)   ──→ 准备 SQE 或记录操作
  ├─ ce_async_send(ctx, fd, buf, sz, ud)   ──→ 准备 SQE 或记录操作
  │
  ├─ ce_async_submit(ctx)                  ──→ io_uring_submit() 或标记已提交
  │
  ├─ ce_async_wait(ctx, min, timeout)      ──→ peek CQE + wait CQE 或 select()
  │
  └─ ce_async_get_event(ctx, i)            ──→ 返回 CeAsyncEvent*
       │
       └─ 处理 ev->type: ACCEPT / RECV / SEND / ...
```

---

## 2. 代码导航

### 2.1 文件结构

```
src_c/network/
├── ce_async_io.h            # 异步 I/O 抽象层接口（公共头文件）
├── ce_async_uring.c         # io_uring 后端实现（CHAOS_HAS_IO_URING）
├── ce_async_posix.c         # POSIX fallback 实现（!CHAOS_HAS_IO_URING）
├── ce_ebpf.h                # eBPF 可观测性接口（公共头文件）
├── ce_ebpf.c                # eBPF 用户态加载器（CHAOS_HAS_EBPF）
├── ce_ebpf_kern.c           # BPF 内核态程序源码
├── ce_ebpf_kern.o           # 编译后的 BPF 字节码（运行时加载）
├── ce_network.h             # 原有网络层接口（不变）
└── ce_network.c             # 原有网络层实现（不变）

src_c/runtime/
├── ce_async_echo_main.c     # io_uring 异步 Echo 服务器测试
└── ce_ebpf_test_main.c      # eBPF 可观测性测试

src_c/CMakeLists.txt         # CMake 构建配置（io_uring/eBPF 检测）

docs/
├── io_uring-ebpf-usage.md   # 用户使用文档
├── io_uring-ebpf-dev.md     # 本文档
└── spec/
    └── chaos-engine-io_uring-ebpf-spec-v0.1.md  # 技术规格书

openspec/changes/io-uring-ebpf/
├── proposal.md              # 变更提案
├── design.md                # 设计决策
├── tasks.md                 # 任务清单
├── specs/network/spec.md    # 需求规格
└── README.md                # 变更概述
```

### 2.2 关键结构体

#### CeAsyncContext（io_uring 后端）

```c
struct CeAsyncContext {
    struct io_uring  ring;                          // liburing 实例
    CeAsyncEvent     events[CE_ASYNC_MAX_EVENTS];   // 完成事件数组（256）
    int              event_count;                    // 当前事件数
    int              pending_count;                  // 待提交 SQE 数
    CeAsyncOpCtx     op_ctx_pool[CE_ASYNC_MAX_EVENTS]; // 操作上下文池
    int              op_ctx_used;                    // 已用上下文数
    CeBool           buffers_registered;             // 是否已注册缓冲区
};
```

#### CeAsyncContext（POSIX fallback）

```c
struct CeAsyncContext {
    CeAsyncOp   ops[CE_ASYNC_MAX_FDS];    // 操作数组（1024）
    int         op_count;                  // 操作数
    CeAsyncEvent events[CE_ASYNC_MAX_EVENTS]; // 完成事件数组（256）
    int         event_count;               // 事件数
};
```

#### CeEbpfContext

```c
struct CeEbpfContext {
    struct bpf_object*  obj;                // BPF 对象（ce_ebpf_kern.o）
    struct bpf_program* progs[CE_EBPF_MAX_PROGS]; // BPF 程序列表（16）
    int                 prog_count;
    struct bpf_link*    links[CE_EBPF_MAX_PROGS]; // BPF attach 链接（16）
    int                 link_count;
    CeBool              loaded;             // 是否已加载到内核
};
```

### 2.3 BPF Maps

| Map 名称 | 类型 | Key | Value | 用途 |
|----------|------|-----|-------|------|
| `func_latency_hist` | HASH | `__u64` (延迟桶 μs) | `__u64` (计数) | 函数延迟直方图 |
| `func_entry_ts` | HASH | `__u32` (tid) | `__u64` (时间戳 ns) | 函数进入时间戳 |
| `tcp_retransmit_count` | ARRAY | `__u32` (0) | `__u64` (计数) | TCP 重传计数器 |
| `io_latency_hist` | HASH | `__u64` (延迟桶 μs) | `__u64` (计数) | I/O 延迟直方图 |
| `io_start_ts` | HASH | `__u32` (tid) | `__u64` (时间戳 ns) | I/O 开始时间戳 |

---

## 3. 如何添加新 BPF 程序

### 3.1 概述

添加新 BPF 程序需要修改三个文件：
1. **`ce_ebpf_kern.c`**：编写内核态 BPF 程序
2. **`ce_ebpf.h`**：声明用户态 API
3. **`ce_ebpf.c`**：实现用户态加载和读取逻辑

### 3.2 模板：添加 kprobe 程序

**步骤 1：在 `ce_ebpf_kern.c` 中添加 BPF 程序**

```c
/* ---- Map: 新功能的 BPF map ---- */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);    /* 根据需求选择 key 类型 */
    __type(value, __u64);  /* 根据需求选择 value 类型 */
} my_new_map SEC(".maps");

/* kprobe: 挂载到目标函数 */
SEC("kprobe/target_function_name")
int kprobe_my_trace(struct pt_regs* ctx) {
    /* 获取当前线程 ID */
    __u32 tid = bpf_get_current_pid_tgid();

    /* 获取时间戳 */
    __u64 ts = bpf_ktime_get_ns();

    /* 读取函数参数（可选） */
    // int arg0 = PT_REGS_PARM1(ctx);

    /* 更新 BPF map */
    __u64* count = bpf_map_lookup_elem(&my_new_map, &tid);
    if (count) {
        __sync_fetch_and_add(count, 1);
    } else {
        __u64 one = 1;
        bpf_map_update_elem(&my_new_map, &tid, &one, BPF_ANY);
    }

    return 0;
}
```

**步骤 2：在 `ce_ebpf.h` 中声明 API**

```c
/** 开始追踪新功能 */
CeResult ce_ebpf_trace_my_feature(CeEbpfContext* ctx);

/** 读取新功能统计 */
int ce_ebpf_get_my_feature_stats(CeEbpfContext* ctx);
```

**步骤 3：在 `ce_ebpf.c` 中实现加载逻辑**

```c
CeResult ce_ebpf_trace_my_feature(CeEbpfContext* ctx) {
    if (!ctx || !ctx->obj) return CE_ERR;

    /* 确保 BPF 程序已加载 */
    if (!ctx->loaded) {
        int ret = bpf_object__load(ctx->obj);
        if (ret < 0) {
            CE_LOG_ERROR("EBPF", "Failed to load BPF object: %d", ret);
            return CE_ERR;
        }
        ctx->loaded = CE_TRUE;
    }

    /* 查找并 attach kprobe */
    struct bpf_program* prog = bpf_object__find_program_by_name(
        ctx->obj, "kprobe_my_trace");
    if (!prog) {
        CE_LOG_WARN("EBPF", "kprobe_my_trace not found");
        return CE_ERR;
    }

    struct bpf_link* link = bpf_program__attach(prog);
    if (!link) {
        CE_LOG_WARN("EBPF", "Failed to attach: %s", strerror(errno));
        return CE_ERR;
    }

    ctx->links[ctx->link_count++] = link;
    CE_LOG_INFO("EBPF", "Attached: kprobe_my_trace");
    return CE_OK;
}

int ce_ebpf_get_my_feature_stats(CeEbpfContext* ctx) {
    if (!ctx || !ctx->obj) return 0;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "my_new_map");
    if (!map) return 0;

    /* 遍历 map 并聚合数据 */
    int total = 0;
    __u32 key = 0, next_key;
    __u64 value;

    while (bpf_map_get_next_key(bpf_map__fd(map), &key, &next_key) == 0) {
        if (bpf_map_lookup_elem(bpf_map__fd(map), &next_key, &value) == 0) {
            total += (int)value;
        }
        key = next_key;
    }

    return total;
}
```

**步骤 4：在 `ce_ebpf.c` 的 stub 部分添加空实现**

```c
#else /* !CHAOS_HAS_EBPF — stubs */

CeResult ce_ebpf_trace_my_feature(CeEbpfContext* ctx) {
    (void)ctx; return CE_ERR;
}
int ce_ebpf_get_my_feature_stats(CeEbpfContext* ctx) {
    (void)ctx; return 0;
}
```

**步骤 5：重新编译 BPF 程序**

```bash
cd src_c/network
clang -O2 -target bpf -c ce_ebpf_kern.c -o ce_ebpf_kern.o
```

### 3.3 模板：添加 tracepoint 程序

```c
/* 在 ce_ebpf_kern.c 中 */

/* tracepoint: 系统调用入口 */
SEC("tracepoint/syscalls/sys_enter_write")
int trace_write_entry(struct trace_event_raw_sys_enter* ctx) {
    __u32 tid = bpf_get_current_pid_tgid();
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&io_start_ts, &tid, &ts, BPF_ANY);
    return 0;
}

/* tracepoint: 系统调用出口 */
SEC("tracepoint/syscalls/sys_exit_write")
int trace_write_exit(struct trace_event_raw_sys_exit* ctx) {
    __u32 tid = bpf_get_current_pid_tgid();
    __u64* start_ts = bpf_map_lookup_elem(&io_start_ts, &tid);
    if (!start_ts) return 0;

    __u64 delta_ns = bpf_ktime_get_ns() - *start_ts;
    __u64 delta_us = delta_ns / 1000;

    /* 对数分桶 */
    __u64 bucket = 1;
    while (bucket < delta_us && bucket < (1ULL << 20)) {
        bucket <<= 1;
    }

    __u64* count = bpf_map_lookup_elem(&io_latency_hist, &bucket);
    if (count) {
        __sync_fetch_and_add(count, 1);
    }

    bpf_map_delete_elem(&io_start_ts, &tid);
    return 0;
}
```

### 3.4 BPF 程序编写要点

1. **License 必须**：`char LICENSE[] SEC("license") = "GPL";`
2. **循环限制**：BPF verifier 要求循环必须有界。对数分桶的 `while` 最多 20 次迭代，安全。
3. **Map 类型选择**：
   - `BPF_MAP_TYPE_HASH`：键值对，适合按 tid/fd 分组。
   - `BPF_MAP_TYPE_ARRAY`：固定大小数组，适合全局计数器。
   - `BPF_MAP_TYPE_PERF_EVENT_ARRAY`：高性能事件流，适合实时数据传输。
4. **原子操作**：使用 `__sync_fetch_and_add` 保证并发安全。
5. **避免大循环**：BPF verifier 限制指令数（通常 1M 条），避免遍历大数组。

---

## 4. 如何添加新异步 I/O 后端

### 4.1 概述

异步 I/O 抽象层设计支持多后端。添加新后端（如 Windows IOCP、macOS kqueue）需要：

1. 实现 `CeAsyncContext` 结构体和所有接口函数
2. 在 CMake 中添加编译条件
3. 确保接口签名与 `ce_async_io.h` 一致

### 4.2 必须实现的接口

```c
/* 生命周期 */
CeAsyncContext* ce_async_init(int queue_depth);
void ce_async_shutdown(CeAsyncContext* ctx);

/* 操作提交（非阻塞） */
void ce_async_accept(CeAsyncContext* ctx, int listen_fd, void* user_data);
void ce_async_recv(CeAsyncContext* ctx, int fd, void* buf, int size, void* user_data);
void ce_async_send(CeAsyncContext* ctx, int fd, const void* buf, int size, void* user_data);
void ce_async_read(CeAsyncContext* ctx, int fd, void* buf, int size, off_t offset, void* user_data);
void ce_async_close(CeAsyncContext* ctx, int fd);

/* 事件处理 */
int ce_async_submit(CeAsyncContext* ctx);
int ce_async_wait(CeAsyncContext* ctx, int min_events, int timeout_ms);
const CeAsyncEvent* ce_async_get_event(CeAsyncContext* ctx, int index);

/* 高级特性（可选） */
CeResult ce_async_register_buffers(CeAsyncContext* ctx, void* buf, int buf_size, int buf_count);
CeBool ce_async_has_zcrx(void);
const char* ce_async_backend_name(void);
```

### 4.3 模板：Windows IOCP 后端

```c
/* ce_async_iocp.c — Windows IOCP 后端 */

#ifdef _WIN32

#include "network/ce_async_io.h"
#include <windows.h>

#define CE_ASYNC_MAX_EVENTS 256

struct CeAsyncContext {
    HANDLE        iocp;                           // I/O Completion Port
    CeAsyncEvent  events[CE_ASYNC_MAX_EVENTS];    // 完成事件数组
    int           event_count;
    /* ... 其他 IOCP 相关字段 ... */
};

CeAsyncContext* ce_async_init(int queue_depth) {
    CeAsyncContext* ctx = calloc(1, sizeof(CeAsyncContext));
    if (!ctx) return NULL;

    ctx->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, queue_depth);
    if (!ctx->iocp) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

void ce_async_shutdown(CeAsyncContext* ctx) {
    if (!ctx) return;
    CloseHandle(ctx->iocp);
    free(ctx);
}

void ce_async_accept(CeAsyncContext* ctx, int listen_fd, void* user_data) {
    /* 使用 AcceptEx + overlapped I/O */
    // ...
}

void ce_async_recv(CeAsyncContext* ctx, int fd, void* buf, int size, void* user_data) {
    /* 使用 WSARecv + overlapped I/O */
    // ...
}

void ce_async_send(CeAsyncContext* ctx, int fd, const void* buf, int size, void* user_data) {
    /* 使用 WSASend + overlapped I/O */
    // ...
}

int ce_async_submit(CeAsyncContext* ctx) {
    /* IOCP 模式下操作在提交时即已投递，submit 为 no-op */
    return 0;
}

int ce_async_wait(CeAsyncContext* ctx, int min_events, int timeout_ms) {
    /* 使用 GetQueuedCompletionStatus 获取完成事件 */
    // ...
    return ctx->event_count;
}

const CeAsyncEvent* ce_async_get_event(CeAsyncContext* ctx, int index) {
    if (index < 0 || index >= ctx->event_count) return NULL;
    return &ctx->events[index];
}

CeResult ce_async_register_buffers(CeAsyncContext* ctx, void* buf, int buf_size, int buf_count) {
    (void)ctx; (void)buf; (void)buf_size; (void)buf_count;
    return CE_ERR;  /* IOCP 不支持 Registered Buffers */
}

CeBool ce_async_has_zcrx(void) {
    return CE_FALSE;
}

const char* ce_async_backend_name(void) {
    return "iocp";
}

#endif /* _WIN32 */
```

### 4.4 CMake 集成

```cmake
# 在 src_c/CMakeLists.txt 中添加

if(WIN32)
    option(CHAOS_USE_IOCP "Enable IOCP async I/O (Windows)" ON)
    if(CHAOS_USE_IOCP)
        target_compile_definitions(engine_core PRIVATE CHAOS_HAS_IOCP)
        target_link_libraries(engine_core PRIVATE ws2_32)
    endif()
endif()
```

### 4.5 后端选择逻辑

```c
// 在 ce_async_io.h 或统一入口中：

#if defined(CHAOS_HAS_IO_URING)
    // 使用 ce_async_uring.c
#elif defined(CHAOS_HAS_IOCP)
    // 使用 ce_async_iocp.c
#else
    // 使用 ce_async_posix.c
#endif
```

---

## 5. 跨平台注意事项

### 5.1 编译宏矩阵

| 平台 | `CHAOS_HAS_IO_URING` | `CHAOS_HAS_EBPF` | 后端 |
|------|:---:|:---:|------|
| Linux (liburing + libbpf) | ✅ | ✅ | io_uring + eBPF |
| Linux (仅 liburing) | ✅ | ❌ | io_uring |
| Linux (无 liburing) | ❌ | ❌ | POSIX fallback |
| macOS | ❌ | ❌ | POSIX fallback |
| Windows | ❌ | ❌ | POSIX fallback（未来 IOCP） |

### 5.2 条件编译模式

```c
// ce_async_uring.c — 仅在 io_uring 可用时编译
#ifdef CHAOS_HAS_IO_URING
// ... 完整实现 ...
#endif

// ce_async_posix.c — 仅在 io_uring 不可用时编译
#ifndef CHAOS_HAS_IO_URING
// ... POSIX fallback 实现 ...
#else
// 空符号避免空编译单元警告
static int __ce_async_posix_unused = 0;
#endif

// ce_ebpf.c — 仅在 eBPF 可用时编译
#ifdef CHAOS_HAS_EBPF
// ... 完整实现 ...
#else
// ... stub 实现 ...
#endif
```

### 5.3 平台特定代码指南

1. **使用 `#ifdef` 而非 `#if`**：避免未定义宏的警告。
2. **Stub 实现必须存在**：确保非目标平台也能编译通过。
3. **`(void)param` 消除未使用参数警告**：stub 中必须使用。
4. **CMake 自动检测**：通过 `pkg_check_modules` 检测依赖，非 Linux 平台自动跳过。
5. **头文件保护**：所有平台相关头文件使用标准的 `#ifndef` 保护。

### 5.4 POSIX fallback 限制

| 特性 | io_uring | POSIX fallback |
|------|:---:|:---:|
| 批量提交 | ✅ | ❌（逐个执行） |
| 零拷贝接收 | ✅ ZCRX | ❌ |
| Registered Buffers | ✅ | ❌ |
| 最大并发 fd | 无限制 | FD_SETSIZE (1024) |
| 异步文件 I/O | ✅ | ✅（pread） |
| 性能 | 200K+ QPS | ~50K QPS |

---

## 6. 内部设计细节

### 6.1 io_uring 操作上下文池

每个异步操作（accept/recv/send 等）需要携带元数据（类型、fd、缓冲区、user_data）。这些元数据存储在 `CeAsyncOpCtx` 池中，通过 `io_uring_sqe_set_data` 关联到 SQE。

```
提交阶段:
  ce_async_accept(ctx, fd, user_data)
    → alloc_op_ctx(ctx)          // 从池中分配
    → io_uring_get_sqe(&ring)    // 获取 SQE
    → io_uring_prep_accept(sqe)  // 准备操作
    → io_uring_sqe_set_data(sqe, op) // 绑定上下文

完成阶段:
  ce_async_wait(ctx, ...)
    → io_uring_peek_cqe(&ring, &cqe)
    → op = io_uring_cqe_get_data(cqe) // 取回上下文
    → 填充 CeAsyncEvent
    → io_uring_cqe_seen(&ring, cqe)
    → reset_op_ctx(ctx)          // 重置池
```

### 6.2 eBPF 延迟分桶算法

使用对数分桶（1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, ... μs）：

```c
__u64 bucket = 1;
while (bucket < delta_us && bucket < (1ULL << 20)) {
    bucket <<= 1;  // 乘以 2
}
// 结果：delta_us=3 → bucket=4, delta_us=100 → bucket=128
```

**优点：**
- 覆盖范围广（1 μs ~ 1 s），桶数少（最多 20 个）
- 低延迟区域精度高，高延迟区域覆盖广
- BPF verifier 友好（循环次数有界）

### 6.3 百分位计算

`ce_ebpf_get_io_latency_stats` 从 `io_latency_hist` map 计算 P50/P90/P99：

1. 遍历所有桶，收集 (bucket, count) 对
2. 按 bucket 值排序
3. 计算累计分布，找到对应百分位的桶

```c
cum += counts[i];
if (cum >= total * 50 / 100)  p50 = buckets[i];  // 50% 分位
if (cum >= total * 90 / 100)  p90 = buckets[i];  // 90% 分位
if (cum >= total * 99 / 100)  p99 = buckets[i];  // 99% 分位
```

### 6.4 事件循环模型

```c
while (running) {
    // 1. 提交所有待处理的 I/O 请求
    ce_async_submit(async);

    // 2. 等待完成事件（可设超时，期间处理 ECS 更新）
    int n = ce_async_wait(async, 1, 100);  // 至少 1 个事件，100ms 超时

    // 3. 处理完成事件
    for (int i = 0; i < n; i++) {
        const CeAsyncEvent* ev = ce_async_get_event(async, i);
        switch (ev->type) {
        case CE_ASYNC_ACCEPT: /* 处理新连接 */ break;
        case CE_ASYNC_RECV:   /* 处理接收数据 */ break;
        case CE_ASYNC_SEND:   /* 处理发送完成 */ break;
        }
    }

    // 4. ECS 更新 + Cell 管理（在 I/O 等待间隙执行）
    ce_ecs_update(dt);
    ce_cell_update();
}
```

---

## 参考

- [io_uring 内核文档](https://kernel.docs.kernel.org/io_uring.html)
- [liburing 手册](https://manpages.debian.org/unstable/liburing-dev/io_uring.7.en.html)
- [libbpf API 文档](https://libbpf.readthedocs.io/)
- [BPF CO-RE 指南](https://nakryiko.com/posts/bpf-portability-and-co-re/)
- [用户使用文档](io_uring-ebpf-usage.md)
- [技术规格书](../docs/spec/chaos-engine-io_uring-ebpf-spec-v0.1.md)
- [设计决策文档](../openspec/changes/io-uring-ebpf/design.md)
