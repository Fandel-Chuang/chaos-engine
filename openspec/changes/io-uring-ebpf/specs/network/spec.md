# Network — io_uring 异步 I/O + eBPF 可观测性（v0.1 Delta）

> 来源: chaos-engine-io_uring-ebpf-spec-v0.1.md | 状态: 部分实现 | 变更类型: ADDED

## 概述

新增异步 I/O 抽象层和 eBPF 可观测性层，为 ChaosEngine 网络子系统提供高性能异步 I/O（Linux io_uring）和零开销内核态可观测性（eBPF kprobe/tracepoint）。异步 I/O 层支持多后端切换（io_uring / POSIX fallback / 未来 Windows IOCP），eBPF 层提供函数延迟追踪、TCP 重传监控、I/O 延迟直方图。

---

## ADDED Requirements

### Requirement: 异步 I/O 抽象层接口

`ce_async_io.h` SHALL 定义统一的异步 I/O 接口，屏蔽底层实现差异（io_uring / POSIX / IOCP），提供 accept、recv、send、read、write、close 六种异步操作。

接口 MUST 包含以下组件：
- 不透明句柄 `CeAsyncContext`：封装异步 I/O 上下文
- 事件类型枚举 `CeAsyncEventType`：`CE_ASYNC_ACCEPT`、`CE_ASYNC_RECV`、`CE_ASYNC_SEND`、`CE_ASYNC_READ`、`CE_ASYNC_WRITE`、`CE_ASYNC_CLOSE`、`CE_ASYNC_ERROR`
- 事件结构体 `CeAsyncEvent`：包含 type、fd、client_fd、buf、nbytes、error、user_data 字段
- 生命周期函数：`ce_async_init(queue_depth)`、`ce_async_shutdown(ctx)`
- 操作提交函数：`ce_async_accept`、`ce_async_recv`、`ce_async_send`、`ce_async_read`、`ce_async_close`
- 事件处理函数：`ce_async_submit(ctx)`、`ce_async_wait(ctx, min_events, timeout_ms)`、`ce_async_get_event(ctx, index)`
- 高级特性函数：`ce_async_register_buffers`、`ce_async_has_zcrx`、`ce_async_backend_name`

#### Scenario: 初始化异步 I/O 上下文

- **WHEN** 调用 `ce_async_init(4096)` 在支持 io_uring 的 Linux 系统上
- **THEN** 返回非 NULL 的 `CeAsyncContext*` 句柄
- **AND** 内部创建了 io_uring 实例，队列深度为 4096
- **AND** 日志输出 "io_uring initialized (depth=4096, features=0x...)"

#### Scenario: 初始化失败时的降级

- **WHEN** 调用 `ce_async_init(256)` 在不支持 io_uring 的系统上（编译时 `CHAOS_HAS_IO_URING` 未定义）
- **THEN** 返回非 NULL 的 `CeAsyncContext*` 句柄（POSIX fallback）
- **AND** 日志输出 "POSIX fallback initialized"

#### Scenario: 提交异步 accept 请求

- **WHEN** 调用 `ce_async_accept(ctx, listen_fd, user_data)` 后调用 `ce_async_submit(ctx)`
- **THEN** accept 请求被提交到内核
- **AND** 后续 `ce_async_wait` 在有新连接时返回 `CE_ASYNC_ACCEPT` 事件

#### Scenario: 提交异步 recv 请求

- **WHEN** 调用 `ce_async_recv(ctx, fd, buf, 4096, user_data)` 后调用 `ce_async_submit(ctx)`
- **THEN** recv 请求被提交到内核
- **AND** 后续 `ce_async_wait` 在数据到达时返回 `CE_ASYNC_RECV` 事件，包含实际接收字节数

#### Scenario: 获取后端名称

- **WHEN** 调用 `ce_async_backend_name()` 在 io_uring 可用时
- **THEN** 返回字符串 `"io_uring"`
- **WHEN** 调用 `ce_async_backend_name()` 在 io_uring 不可用时
- **THEN** 返回字符串 `"posix"`

---

### Requirement: io_uring 后端实现

`ce_async_uring.c` SHALL 基于 liburing 2.x 实现完整的异步 I/O 后端，支持 accept、recv、send、read、write、close 六种操作，以及 Registered Buffers 和 ZCRX 检测。

实现 MUST：
- 使用 `io_uring_queue_init_params` 初始化 io_uring 实例，并记录 features 标志
- 使用操作上下文池（`CeAsyncOpCtx`）管理每个异步操作的元数据（类型、fd、缓冲区、user_data）
- 在 `ce_async_submit` 中调用 `io_uring_submit` 批量提交所有待处理的 SQE
- 在 `ce_async_wait` 中先非阻塞 peek CQE，不足时阻塞等待更多
- 正确填充 `CeAsyncEvent`：accept 事件设置 client_fd，recv/send/read/write 事件设置 nbytes，错误时设置 error
- `ce_async_register_buffers` 调用 `io_uring_register_buffers` 注册固定缓冲区
- `ce_async_has_zcrx` 通过检测 `IORING_FEAT_RECVSEND_BUNDLE` 判断零拷贝接收支持

#### Scenario: 批量提交与完成处理

- **WHEN** 连续调用 `ce_async_accept`、`ce_async_recv`、`ce_async_send` 各一次，然后调用 `ce_async_submit(ctx)`
- **THEN** 三个 SQE 被一次性提交到内核（单次 `io_uring_submit` 调用）
- **AND** 返回值为 3（提交的请求数）

#### Scenario: 等待完成事件

- **WHEN** 调用 `ce_async_wait(ctx, 1, 1000)` 且至少有一个 I/O 操作已完成
- **THEN** 返回已完成事件数量（>= 1）
- **AND** 通过 `ce_async_get_event(ctx, i)` 可获取每个事件的详细信息

#### Scenario: Registered Buffers 注册

- **WHEN** 调用 `ce_async_register_buffers(ctx, buf, 4096, 64)` 在 io_uring 后端
- **THEN** 返回 `CE_OK`
- **AND** 64 个 4096 字节的缓冲区被注册到 io_uring 实例
- **AND** 日志输出 "Registered 64 buffers (4096 bytes each)"

#### Scenario: ZCRX 检测

- **WHEN** 调用 `ce_async_has_zcrx()` 在内核支持 `IORING_FEAT_RECVSEND_BUNDLE` 的系统上
- **THEN** 返回 `CE_TRUE`
- **WHEN** 调用 `ce_async_has_zcrx()` 在不支持的系统上
- **THEN** 返回 `CE_FALSE`

---

### Requirement: POSIX fallback 后端

`ce_async_posix.c` SHALL 在 `CHAOS_HAS_IO_URING` 未定义时编译，使用 `select()` + 非阻塞 socket 提供异步 I/O 功能子集。

实现 MUST：
- 使用操作数组（`CeAsyncOp`）记录所有待处理操作
- 在 `ce_async_wait` 中构建 `fd_set`，调用 `select()` 等待就绪
- 对就绪的 fd 执行实际的 `accept()`/`recv()`/`send()`/`pread()`/`close()` 系统调用
- `ce_async_register_buffers` 返回 `CE_ERR`（不支持）
- `ce_async_has_zcrx` 返回 `CE_FALSE`
- `ce_async_backend_name` 返回 `"posix"`

#### Scenario: POSIX fallback 的 accept 处理

- **WHEN** 在 POSIX fallback 后端调用 `ce_async_accept(ctx, listen_fd, NULL)` 后 `ce_async_submit` + `ce_async_wait`
- **THEN** 当有新连接时，返回 `CE_ASYNC_ACCEPT` 事件
- **AND** `ev->client_fd` 为新连接的 fd

#### Scenario: POSIX fallback 不支持 Registered Buffers

- **WHEN** 调用 `ce_async_register_buffers(ctx, buf, 4096, 64)` 在 POSIX fallback 后端
- **THEN** 返回 `CE_ERR`

---

### Requirement: eBPF 可观测性接口

`ce_ebpf.h` SHALL 定义 eBPF 可观测性接口，提供函数延迟追踪、TCP 重传监控、I/O 延迟直方图功能。

接口 MUST 包含：
- 不透明句柄 `CeEbpfContext`
- 生命周期函数：`ce_ebpf_init()`、`ce_ebpf_shutdown(ctx)`
- 函数追踪：`ce_ebpf_trace_function(ctx, func_name)`、`ce_ebpf_dump_latency(ctx, func_name)`
- 网络观测：`ce_ebpf_trace_tcp_retransmit(ctx)`、`ce_ebpf_get_retransmit_count(ctx)`
- I/O 观测：`ce_ebpf_trace_io_latency(ctx)`、`ce_ebpf_get_io_latency_stats(ctx, p50, p90, p99)`
- 查询：`ce_ebpf_available()`

#### Scenario: eBPF 可用性检测

- **WHEN** 调用 `ce_ebpf_available()` 在存在 `/sys/kernel/btf/vmlinux` 的系统上
- **THEN** 返回 `CE_TRUE`
- **WHEN** 调用 `ce_ebpf_available()` 在不存在 BTF 的系统上
- **THEN** 返回 `CE_FALSE`

#### Scenario: 初始化 eBPF 上下文

- **WHEN** 调用 `ce_ebpf_init()` 在支持 eBPF 的系统上
- **THEN** 返回非 NULL 的 `CeEbpfContext*` 句柄
- **AND** 内部通过 `bpf_object__open` 打开了 BPF 对象文件 `ce_ebpf_kern.o`
- **AND** 日志输出 "BPF object opened: ..."

#### Scenario: 函数延迟追踪

- **WHEN** 调用 `ce_ebpf_trace_function(ctx, "ce_ecs_update")`
- **THEN** BPF 程序被加载到内核（`bpf_object__load`）
- **AND** kprobe/kretprobe 被 attach 到 `ce_ecs_update` 函数
- **AND** 日志输出 "Attached: kprobe_ecs_update_entry" 和 "Attached: kretprobe_ecs_update_exit"

#### Scenario: 读取函数延迟直方图

- **WHEN** 调用 `ce_ebpf_dump_latency(ctx, "ce_ecs_update")` 在追踪已启动后
- **THEN** 从 BPF map `func_latency_hist` 读取延迟桶数据
- **AND** 日志输出各延迟桶的采样数（如 "<= 16 us: 42 samples"）
- **AND** 返回总采样数

#### Scenario: TCP 重传监控

- **WHEN** 调用 `ce_ebpf_trace_tcp_retransmit(ctx)`
- **THEN** kprobe 被 attach 到内核函数 `tcp_retransmit_skb`
- **AND** 每次 TCP 重传时 `tcp_retransmit_count` map 中的计数器递增
- **AND** 通过 `ce_ebpf_get_retransmit_count(ctx)` 可获取当前重传总数

#### Scenario: I/O 延迟统计

- **WHEN** 调用 `ce_ebpf_trace_io_latency(ctx)` 后调用 `ce_ebpf_get_io_latency_stats(ctx, &p50, &p90, &p99)`
- **THEN** tracepoint 被 attach 到 `sys_enter_recvfrom` / `sys_exit_recvfrom`
- **AND** p50、p90、p99 被填充为微秒级的延迟百分位值
- **AND** 无 I/O 操作时 p50/p90/p99 均为 0

#### Scenario: eBPF 不可用时的 stub 行为

- **WHEN** 编译时 `CHAOS_HAS_EBPF` 未定义，调用任意 eBPF 函数
- **THEN** `ce_ebpf_init()` 返回 NULL
- **AND** 所有追踪函数返回 `CE_ERR`
- **AND** 所有统计函数返回 0
- **AND** `ce_ebpf_available()` 返回 `CE_FALSE`

---

### Requirement: BPF 内核态程序

`ce_ebpf_kern.c` SHALL 包含编译为 BPF 字节码的内核态程序，提供函数延迟追踪、TCP 重传监控、I/O 延迟追踪三种功能。

BPF 程序 MUST：
- 声明 GPL license（`char LICENSE[] SEC("license") = "GPL"`）
- 使用 BPF maps 存储数据：`func_latency_hist`（HASH，函数延迟直方图）、`func_entry_ts`（HASH，函数进入时间戳）、`tcp_retransmit_count`（ARRAY，TCP 重传计数）、`io_latency_hist`（HASH，I/O 延迟直方图）、`io_start_ts`（HASH，I/O 开始时间戳）
- kprobe/kretprobe 程序使用对数分桶（1, 2, 4, 8, 16, ... μs）记录延迟分布
- 使用 `bpf_get_current_pid_tgid()` 获取线程 ID 作为 map key
- 使用 `bpf_ktime_get_ns()` 获取纳秒级时间戳

#### Scenario: 函数延迟追踪的 BPF 逻辑

- **WHEN** `ce_ecs_update` 函数被调用
- **THEN** `kprobe_ecs_update_entry` 记录当前线程 ID 和进入时间戳到 `func_entry_ts` map
- **AND** 函数返回时 `kretprobe_ecs_update_exit` 计算延迟（ns），转换为 μs，放入对数桶
- **AND** 对应桶的计数器递增

#### Scenario: TCP 重传监控的 BPF 逻辑

- **WHEN** 内核调用 `tcp_retransmit_skb`
- **THEN** `kprobe_tcp_retransmit` 递增 `tcp_retransmit_count` map 中 key=0 的计数器

#### Scenario: I/O 延迟追踪的 BPF 逻辑

- **WHEN** 进程调用 `recvfrom` 系统调用
- **THEN** `trace_recvfrom_entry` 记录线程 ID 和开始时间戳
- **AND** `recvfrom` 返回时 `trace_recvfrom_exit` 计算延迟，放入 `io_latency_hist` 对数桶

---

### Requirement: 编译时条件控制

CMake 构建系统 SHALL 通过 `CHAOS_USE_IO_URING` 和 `CHAOS_USE_EBPF` 选项控制 io_uring 和 eBPF 功能的编译。

构建配置 MUST：
- 定义 `option(CHAOS_USE_IO_URING "Enable io_uring async I/O (Linux)" ON)`
- 定义 `option(CHAOS_USE_EBPF "Enable eBPF observability (Linux)" ON)`
- 通过 `pkg_check_modules` 检测 liburing 和 libbpf 是否可用
- 检测到 liburing 时定义 `CHAOS_HAS_IO_URING` 宏并链接 liburing
- 检测到 libbpf 时定义 `CHAOS_HAS_EBPF` 宏并链接 libbpf
- 非 Linux 平台自动跳过 io_uring 和 eBPF

#### Scenario: Linux 平台编译（liburing + libbpf 可用）

- **WHEN** 在安装了 liburing 和 libbpf 开发包的 Linux 系统上执行 cmake + make
- **THEN** `CHAOS_HAS_IO_URING` 和 `CHAOS_HAS_EBPF` 宏被定义
- **AND** `ce_async_uring.c` 和 `ce_ebpf.c` 被编译
- **AND** `ce_async_posix.c` 编译为空（仅避免空编译单元警告）

#### Scenario: 非 Linux 平台编译

- **WHEN** 在 macOS 或 Windows 上执行 cmake + make
- **THEN** `CHAOS_HAS_IO_URING` 和 `CHAOS_HAS_EBPF` 宏均未定义
- **AND** 仅 `ce_async_posix.c` 被编译（POSIX fallback）
- **AND** eBPF 函数全部为 stub 实现
