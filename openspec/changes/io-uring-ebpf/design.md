# Design: io_uring 异步 I/O + eBPF 可观测性

## Context

ChaosEngine v0.1 的网络层基于 POSIX `accept()`/`recv()`/`send()` 阻塞/非阻塞模式（`ce_network.h/c`，共 343 行），存在以下技术债务：

1. **无异步 I/O 框架**：每个连接需要独立线程或忙轮询，无法高效处理数千并发连接。
2. **多次内存拷贝**：数据在内核态和用户态之间至少拷贝 2 次，CPU 利用率低。
3. **无可观测性**：无法在生产环境以零开销方式追踪函数延迟、TCP 重传、I/O 延迟分布。

当前开发环境（Linux Kernel 7.0 + liburing 2.14 + libbpf 1.6.3）已完全具备 io_uring 和 eBPF 的内核支持。引入这两项技术可带来 4x QPS 提升（50K → 200K+）和 <1% 开销的可观测性。

## Goals / Non-Goals

**Goals:**

1. **异步 I/O 抽象层**：统一接口，支持 io_uring（Linux）和 POSIX fallback（跨平台），未来扩展 Windows IOCP。
2. **高性能 io_uring 后端**：批量提交、Registered Buffers、ZCRX 检测，单线程事件循环模型。
3. **eBPF 可观测性**：kprobe 函数延迟追踪、TCP 重传监控、tracepoint I/O 延迟直方图。
4. **编译时可选**：通过 CMake option + `#ifdef` 条件编译，非 Linux 平台零开销。
5. **向后兼容**：现有 `ce_network.h/c` 保持不变，异步 I/O 层为新增模块。

**Non-Goals:**

- ❌ 不替换所有 POSIX I/O（保留 fallback 路径）
- ❌ 不在非 Linux 平台使用 io_uring 或 eBPF
- ❌ 不强制要求内核 5.1+（编译时检测 + 运行时 fallback）
- ❌ 不依赖 eBPF 做核心业务逻辑（eBPF 是旁路增强）
- ❌ 当前版本不做 XDP 包过滤、BPF Stream Parser、BPF LSM（延后到 Phase 4）
- ❌ 当前版本不做服务端主循环切换到 io_uring（延后到 Phase 5）
- ❌ 当前版本不做 Windows IOCP 后端（P2 优先级）

## Decisions

### Decision 1: 异步 I/O 后端选择 — io_uring vs epoll vs 自定义

**选择**：Linux 使用 io_uring（liburing），非 Linux 使用 POSIX select fallback。

**理由**：
- io_uring 性能最优：批量提交减少系统调用（单次 submit 可提交数百个 I/O 请求），零拷贝路径减少内存拷贝，单线程可处理数千连接。
- 基准数据：io_uring 可达 200K+ QPS（echo），P99 延迟 ~100μs；POSIX epoll 约 50K QPS，P99 延迟 ~500μs。
- POSIX select fallback 保证跨平台兼容性，代码简单（~240 行），覆盖 macOS、BSD 等平台。

**替代方案**：
- ❌ epoll：仍需每次 I/O 调用 `recv()`/`send()` 系统调用，无法批量提交，无零拷贝。
- ❌ 仅 io_uring：失去跨平台能力，Windows/macOS 无法编译。

### Decision 2: 抽象层设计 — 不透明句柄 + 统一事件模型

**选择**：`CeAsyncContext` 不透明句柄封装后端差异，`CeAsyncEvent` 统一事件结构。

**理由**：
- 调用方无需关心底层是 io_uring 还是 select，接口完全一致。
- 事件驱动模型天然适合游戏服务器的事件循环（ECS 更新在 I/O 等待间隙执行）。
- 操作提交（`ce_async_accept/recv/send`）与事件处理（`ce_async_submit/wait`）分离，支持批量提交。

**替代方案**：
- ❌ 回调模型：增加用户代码复杂度，C 语言中回调管理容易出错。
- ❌ 暴露底层实现：失去后端可替换性，未来切换到 IOCP 需要大量修改调用方代码。

### Decision 3: io_uring 事件循环模型 — 单线程 + 批量提交

**选择**：单线程事件循环，在 I/O 等待间隙执行 ECS 更新和 Cell 管理。

```
while (running) {
    ce_async_submit();           // 批量提交所有 SQE
    int n = ce_async_wait(1);    // 等待至少 1 个完成事件
    for (i = 0; i < n; i++) {    // 处理完成事件
        ev = ce_async_get_event(i);
        switch (ev->type) { ... }
    }
    ce_ecs_update(dt);           // ECS 更新
    ce_cell_update();            // Cell 管理
}
```

**理由**：
- 游戏服务器通常有大量空闲连接（玩家登录但不活跃），单线程处理数千连接足够。
- ECS 更新和 I/O 处理在同一线程，避免锁竞争。
- 批量提交减少系统调用：一次 `io_uring_submit` 可提交数百个 SQE。

**替代方案**：
- ❌ 多线程 I/O：增加线程切换开销，需要锁保护共享状态，复杂度高。
- ❌ 忙轮询（IORING_SETUP_SQPOLL）：消耗 CPU，不适合游戏服务器（有 ECS 计算任务）。

### Decision 4: eBPF 程序类型 — kprobe + tracepoint

**选择**：kprobe 追踪函数调用（入口/返回），tracepoint 追踪系统调用。

**理由**：
- kprobe 可挂载到任意内核函数和用户态函数（如 `ce_ecs_update`），灵活度最高。
- kretprobe 可获取函数返回时间，计算函数延迟。
- tracepoint 是稳定的内核接口，比 kprobe 更可靠（内核版本升级不会破坏 tracepoint）。
- 组合使用覆盖全面：kprobe 追踪自定义函数，tracepoint 追踪标准系统调用。

**替代方案**：
- ❌ 仅 tracepoint：无法追踪自定义函数（如 `ce_ecs_update`）。
- ❌ fentry/fexit（BPF trampoline）：需要内核 5.5+，且对用户态函数支持有限。
- ❌ USDT：需要重新编译插入探针，不如 kprobe 灵活。

### Decision 5: BPF 程序加载 — libbpf CO-RE

**选择**：使用 libbpf 加载预编译的 BPF 对象文件（`ce_ebpf_kern.o`），依赖 BTF 和 CO-RE（Compile Once, Run Everywhere）。

**理由**：
- 无需针对每个内核版本重新编译 BPF 程序。
- libbpf 是标准 BPF 加载库，API 稳定。
- BTF 在当前内核（7.0）中已可用（`/sys/kernel/btf/vmlinux`）。

**替代方案**：
- ❌ BCC（BPF Compiler Collection）：需要运行时编译，依赖 LLVM/Clang 运行时，部署复杂。
- ❌ 手动 bpf() 系统调用：过于底层，代码量大且易出错。

### Decision 6: 跨平台策略 — 编译时选择 + 运行时检测

**选择**：编译时通过 `#ifdef __linux__` / `#ifdef _WIN32` 选择后端，运行时通过 `ce_ebpf_available()` 和 io_uring 初始化结果检测可用性。

```
#ifdef __linux__
  → io_uring (liburing) 或 POSIX fallback（运行时检测 io_uring 是否可用）
#elif _WIN32
  → IOCP（未来实现）
#else
  → POSIX fallback (select)
#endif
```

**理由**：
- 编译时选择避免链接不存在的库（Windows 上无 liburing）。
- 运行时检测处理 Linux 内核版本差异（旧内核不支持 io_uring）。
- 各平台能力矩阵清晰：

| 能力 | Linux io_uring | Windows iocp | POSIX fallback |
|------|:---:|:---:|:---:|
| 异步 TCP accept/recv/send | ✅ | ✅（未来） | ✅（select） |
| 异步文件 I/O | ✅ | ✅（未来） | ✅（pread） |
| 零拷贝接收 | ✅ ZCRX | ❌ | ❌ |
| Registered Buffers | ✅ | ❌ | ❌ |
| 批量提交 | ✅ | ❌ | ❌ |
| eBPF 可观测性 | ✅ | ❌ | ❌ |

**替代方案**：
- ❌ 运行时多态（函数指针表）：增加间接调用开销，C 语言中实现复杂。
- ❌ 仅支持 Linux：失去跨平台能力。

### Decision 7: eBPF 延迟分桶策略 — 对数分桶

**选择**：使用对数分桶（1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, ... μs），上限 2^20 μs（~1 秒）。

**理由**：
- 对数分桶覆盖范围广（微秒到秒级），桶数少（最多 20 个桶）。
- 适合延迟分布分析：低延迟区域精度高（1, 2, 4 μs），高延迟区域覆盖广。
- BPF verifier 限制循环次数，对数分桶的 while 循环最多 20 次迭代，安全。

**替代方案**：
- ❌ 线性分桶（每 1μs 一个桶）：桶数过多（>1000），BPF map 内存消耗大。
- ❌ 固定桶数组：需要预定义桶边界，灵活性差。

## Architecture

### 模块分层

```
┌─────────────────────────────────────────────────────────┐
│                   应用层                                  │
│  ce_server_main.c  /  ce_client_main.c                  │
│  ce_async_echo_main.c  /  ce_ebpf_test_main.c           │
├─────────────────────────────────────────────────────────┤
│                  ce_network (网络层，不变)                │
│  ce_socket_accept/connect/send/recv                      │
│  ce_net_send_message/recv_message (长度前缀协议)          │
├─────────────────────────────────────────────────────────┤
│               ce_async_io (新增异步 I/O 层)               │
│  ce_async_init / ce_async_submit / ce_async_wait         │
│  ce_async_accept / ce_async_recv / ce_async_send         │
├─────────────────────────┬───────────────────────────────┤
│   ce_async_uring.c      │   ce_async_posix.c            │
│   (io_uring 后端)        │   (POSIX fallback)            │
│   liburing 2.x          │   select() + 非阻塞 socket     │
├─────────────────────────┴───────────────────────────────┤
│                  ce_ebpf (新增 eBPF 层)                   │
│  ce_ebpf_init / ce_ebpf_trace_function                  │
│  ce_ebpf_trace_tcp_retransmit / ce_ebpf_trace_io_latency│
│  ┌──────────────────────────────────────────────────┐   │
│  │ ce_ebpf_kern.o (BPF 内核态)                       │   │
│  │ kprobe/ce_ecs_update  → 函数延迟直方图             │   │
│  │ kprobe/tcp_retransmit_skb → TCP 重传计数          │   │
│  │ tracepoint/sys_enter_recvfrom → I/O 延迟直方图    │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### io_uring 内部结构

```
CeAsyncContext
├── struct io_uring ring          # liburing 实例
├── CeAsyncEvent events[256]      # 完成事件数组
├── int event_count               # 当前事件数
├── int pending_count             # 待提交 SQE 数
├── CeAsyncOpCtx op_ctx_pool[256] # 操作上下文池
├── int op_ctx_used               # 已用上下文数
└── CeBool buffers_registered     # 是否已注册缓冲区
```

### eBPF 内部结构

```
CeEbpfContext
├── struct bpf_object* obj        # BPF 对象（ce_ebpf_kern.o）
├── struct bpf_program* progs[16] # BPF 程序列表
├── int prog_count
├── struct bpf_link* links[16]    # BPF attach 链接
├── int link_count
└── CeBool loaded                 # 是否已加载到内核
```

### 数据流：eBPF 可观测性

```
用户态 (ce_ebpf.c)                    内核态 (ce_ebpf_kern.o)
───────────────────────────────────── ─────────────────────────────
ce_ebpf_init()
  └→ bpf_object__open("ce_ebpf_kern.o")
                                      [BPF maps 创建]
                                      func_latency_hist (HASH)
                                      func_entry_ts (HASH)
                                      tcp_retransmit_count (ARRAY)
                                      io_latency_hist (HASH)
                                      io_start_ts (HASH)

ce_ebpf_trace_function("ce_ecs_update")
  └→ bpf_object__load()
  └→ bpf_program__attach(kprobe_ecs_update_entry)
  └→ bpf_program__attach(kretprobe_ecs_update_exit)
                                      函数调用时:
                                      kprobe → 记录 tid+ts
                                      kretprobe → 计算延迟 → 写入直方图

ce_ebpf_dump_latency("ce_ecs_update")
  └→ bpf_map_get_next_key(fd, ...)    ← 读取 func_latency_hist
  └→ CE_LOG_INFO(...)                 输出延迟分布
```

## Risks / Trade-offs

### Risk 1: io_uring 内核版本依赖

- **风险**：io_uring 基础功能需要内核 5.1+，高级特性（ZCRX）需要 6.0+。旧内核或非 Linux 平台不可用。
- **缓解**：编译时通过 CMake 检测 liburing 可用性，运行时通过 `io_uring_queue_init` 返回值检测。不可用时自动降级到 POSIX fallback。
- **影响**：低。当前开发环境为 Kernel 7.0，所有特性可用。生产部署通常也使用较新内核。

### Risk 2: eBPF 权限要求

- **风险**：加载 BPF 程序需要 `CAP_BPF` 或 `CAP_SYS_ADMIN` 权限，普通用户无法运行。
- **缓解**：eBPF 功能为可选项（编译时 `CHAOS_USE_EBPF`），不可用时所有函数返回 stub。生产环境通常以 root 或具备相应 capability 的用户运行服务器。
- **影响**：低。开发环境可通过 `sudo` 运行测试程序。

### Risk 3: BPF 程序的可移植性

- **风险**：BPF 程序依赖内核内部结构（如 `struct pt_regs`、`struct trace_event_raw_sys_enter`），不同内核版本可能有差异。
- **缓解**：使用 libbpf CO-RE（Compile Once, Run Everywhere）+ BTF，内核结构偏移在加载时自动重定位。当前内核 7.0 已支持 BTF。
- **影响**：低。CO-RE 是业界标准方案。

### Risk 4: io_uring 操作上下文池溢出

- **风险**：`CeAsyncOpCtx` 池大小为 256（`CE_ASYNC_MAX_EVENTS`），如果单次批量提交超过 256 个操作，池会溢出。
- **缓解**：当前实现中，每次 `ce_async_wait` 后调用 `reset_op_ctx` 重置池。256 对于游戏服务器场景足够（通常不会同时提交数百个 I/O 请求）。
- **影响**：低。如果未来需要更大并发，可调整 `CE_ASYNC_MAX_EVENTS` 常量。

### Risk 5: POSIX fallback 性能

- **风险**：POSIX fallback 使用 `select()`，有 fd 数量限制（`FD_SETSIZE` 通常为 1024），且每次 `ce_async_wait` 需要遍历所有操作。
- **缓解**：POSIX fallback 仅用于非 Linux 平台或旧内核，这些场景下并发量通常不高。Linux 平台优先使用 io_uring。
- **影响**：低。未来可升级 POSIX fallback 为 epoll（Linux）或 kqueue（macOS）。

### Trade-off: 单线程事件循环 vs 多线程

- **选择**：单线程事件循环（I/O + ECS 在同一线程）。
- **代价**：I/O 密集时可能阻塞 ECS 更新；ECS 计算密集时可能延迟 I/O 响应。
- **收益**：无锁竞争，代码简单，适合游戏服务器（I/O 和计算交替进行）。
- **影响**：低。游戏服务器通常 I/O 和计算交替，单线程足够。未来可通过 `IORING_SETUP_SQPOLL` 或独立 I/O 线程扩展。
