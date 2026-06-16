# io-uring-ebpf

技术调研：io_uring 异步 I/O + eBPF 可观测性

> 来源: chaos-engine-io_uring-ebpf-spec-v0.1.md | 状态: 部分实现 | 变更类型: ADDED

---

## Why

ChaosEngine v0.1 的网络层（`ce_network.h/c`）基于 POSIX `accept()`/`recv()`/`send()` 阻塞/非阻塞模式，存在以下核心瓶颈：

1. **无异步 I/O 框架**：每个连接需要独立线程或忙轮询，数千并发连接时线程切换开销巨大。游戏服务器（MMO）场景中大量空闲连接加剧了这一问题。
2. **多次内存拷贝**：数据在内核态和用户态之间至少拷贝 2 次，无零拷贝路径，CPU 利用率低。
3. **无可观测性基础设施**：无法在生产环境以零开销方式追踪函数延迟、TCP 重传、I/O 延迟分布等关键指标。出现性能问题时只能依赖 printf 日志或 gdb 断点。

当前开发环境（Linux Kernel 7.0 + liburing 2.14 + libbpf 1.6.3）已完全具备 io_uring 和 eBPF 的内核支持（`CONFIG_IO_URING_BPF=y`、`CONFIG_IO_URING_ZCRX=y`、`CONFIG_BPF_JIT_ALWAYS_ON=y`），引入这两项技术可带来数量级的性能提升和零开销的可观测性。

## What Changes

### 新增模块

1. **异步 I/O 抽象层（`ce_async_io.h`）**：统一异步 I/O 接口（accept/recv/send/read/write/close），支持多后端切换。Linux 使用 io_uring（liburing），非 Linux 使用 POSIX fallback（select），未来扩展 Windows IOCP。

2. **io_uring 后端（`ce_async_uring.c`）**：基于 liburing 2.x 实现异步 accept/recv/send/read/write/close，支持 Registered Buffers（内存池固定）、零拷贝接收（ZCRX）检测、单线程事件循环模型。

3. **POSIX fallback 后端（`ce_async_posix.c`）**：当 io_uring 不可用时，使用 select() + 非阻塞 socket 提供功能子集（accept/recv/send/read/close）。

4. **eBPF 可观测性层（`ce_ebpf.h/c` + `ce_ebpf_kern.c`）**：基于 libbpf 的用户态加载器 + BPF 内核态程序，提供：
   - kprobe 函数延迟追踪（直方图）
   - kprobe TCP 重传监控（按连接计数）
   - tracepoint I/O 延迟追踪（P50/P90/P99 统计）

### 架构决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 异步 I/O 后端 | Linux: io_uring, Fallback: POSIX select | io_uring 性能最优（批量提交、零拷贝），POSIX 保证兼容性 |
| eBPF 程序类型 | kprobe + tracepoint | kprobe 追踪内核/用户函数，tracepoint 追踪系统调用，覆盖全面 |
| 编译策略 | CMake option + `#ifdef` 条件编译 | `CHAOS_HAS_IO_URING` / `CHAOS_HAS_EBPF` 控制，非 Linux 平台零开销 |
| 事件循环模型 | 单线程 + 批量提交 | 适合游戏服务器（ECS 更新在 I/O 等待间隙执行） |
| BPF 程序加载 | libbpf CO-RE（Compile Once, Run Everywhere） | 无需针对每个内核版本重新编译 BPF 程序 |

## Impact

### 受影响的代码

| 文件/目录 | 影响类型 | 说明 |
|-----------|----------|------|
| `src_c/network/ce_async_io.h` | **新增** | 异步 I/O 抽象层接口（105 行） |
| `src_c/network/ce_async_uring.c` | **新增** | io_uring 后端实现（257 行） |
| `src_c/network/ce_async_posix.c` | **新增** | POSIX fallback 实现（240 行） |
| `src_c/network/ce_ebpf.h` | **新增** | eBPF 可观测性接口（62 行） |
| `src_c/network/ce_ebpf.c` | **新增** | eBPF 用户态加载器（275 行） |
| `src_c/network/ce_ebpf_kern.c` | **新增** | BPF 内核态程序（152 行） |
| `src_c/network/ce_ebpf_kern.o` | **新增** | BPF 编译产物 |
| `src_c/runtime/ce_async_echo_main.c` | **新增** | io_uring echo 测试程序 |
| `src_c/runtime/ce_ebpf_test_main.c` | **新增** | eBPF 可观测性测试程序 |
| `src_c/network/ce_network.h/c` | 不受影响 | 现有 POSIX 网络层保留，作为 fallback |
| `CMakeLists.txt` | 修改 | 添加 `CHAOS_USE_IO_URING` / `CHAOS_USE_EBPF` 选项和 pkg-config 检测 |

### 不受影响的模块

- ECS、AOI、Cell、Memory、Log、Render 核心逻辑不变
- 非 Linux 平台行为完全不变（编译时自动跳过 io_uring 和 eBPF）
- 不带 `CHAOS_HAS_IO_URING` 编译时，使用 POSIX fallback，功能子集可用

### 已实现范围（Phase 1-3）

- **Phase 1** ✅：io_uring 基础集成 — 接口定义、io_uring 后端（accept/recv/send/read/write/close）、POSIX fallback
- **Phase 2** ✅：io_uring 高级特性 — Registered Buffers、ZCRX 检测、批量提交
- **Phase 3** ✅：eBPF 可观测性 — 函数延迟追踪（kprobe）、TCP 重传监控、I/O 延迟直方图（tracepoint）

### 延后到后续版本

- **Phase 4**：eBPF 网络优化 — XDP 包过滤（DDoS 基础防护）、BPF Stream Parser（协议加速）、io_uring + BPF 联动
- **Phase 5**：服务端主循环切换到 io_uring 事件循环、性能基准测试报告
- Windows IOCP 后端（P2 优先级）
- 单元测试和性能基准测试
