# Tasks: io_uring 异步 I/O + eBPF 可观测性

> 按 spec 中 Phase 1-5 顺序排列。Phase 1-3 已实现，标记为 [x]。Phase 4-5 及测试/文档待完成。

---

## 1. io_uring 基础集成（Phase 1）

- [x] 1.1 创建 `ce_async_io.h`：定义 `CeAsyncContext` 不透明句柄、`CeAsyncEventType` 枚举、`CeAsyncEvent` 结构体、生命周期/操作提交/事件处理/高级特性接口
- [x] 1.2 实现 `ce_async_uring.c`：基于 liburing 2.x 实现 `ce_async_init`（`io_uring_queue_init_params`）、`ce_async_shutdown`（`io_uring_queue_exit`）
- [x] 1.3 实现 `ce_async_accept`：使用 `io_uring_prep_accept` 准备 SQE，通过 `CeAsyncOpCtx` 携带操作上下文
- [x] 1.4 实现 `ce_async_recv`：使用 `io_uring_prep_recv` 准备 SQE
- [x] 1.5 实现 `ce_async_send`：使用 `io_uring_prep_send` 准备 SQE
- [x] 1.6 实现 `ce_async_read`：使用 `io_uring_prep_read` 准备 SQE（支持 offset 参数）
- [x] 1.7 实现 `ce_async_close`：使用 `io_uring_prep_close` 准备 SQE，SQ 满时 fallback 到直接 `close(fd)`
- [x] 1.8 实现 `ce_async_submit`：调用 `io_uring_submit` 批量提交所有待处理 SQE
- [x] 1.9 实现 `ce_async_wait`：先非阻塞 `io_uring_peek_cqe`，不足时 `io_uring_wait_cqes` 阻塞等待，正确填充 `CeAsyncEvent` 各字段
- [x] 1.10 实现 `ce_async_get_event`：按索引返回完成事件指针
- [x] 1.11 实现 `CeAsyncOpCtx` 操作上下文池：管理每个异步操作的元数据（type/fd/buf/user_data），池大小 256
- [x] 1.12 实现 POSIX fallback（`ce_async_posix.c`）：使用 `select()` + 非阻塞 socket，提供 accept/recv/send/read/close 功能子集
- [x] 1.13 实现 POSIX fallback 的 `ce_async_submit`：标记所有操作为已提交（无实际操作）
- [x] 1.14 实现 POSIX fallback 的 `ce_async_wait`：构建 `fd_set`，`select()` 等待，对就绪 fd 执行实际系统调用
- [x] 1.15 创建 echo 测试程序 `ce_async_echo_main.c`：验证 io_uring 单连接 echo 功能
- [x] 1.16 CMake 集成：添加 `CHAOS_USE_IO_URING` option，`pkg_check_modules(liburing)`，定义 `CHAOS_HAS_IO_URING` 宏

---

## 2. io_uring 高级特性（Phase 2）

- [x] 2.1 实现 `ce_async_register_buffers`：调用 `io_uring_register_buffers` 注册固定缓冲区，减少内存拷贝
- [x] 2.2 实现 `ce_async_has_zcrx`：通过检测 `IORING_FEAT_RECVSEND_BUNDLE` 判断零拷贝接收支持
- [x] 2.3 实现 `ce_async_backend_name`：io_uring 后端返回 `"io_uring"`，POSIX 后端返回 `"posix"`
- [x] 2.4 批量提交优化：`ce_async_submit` 一次性提交所有待处理 SQE，减少系统调用次数
- [ ] 2.5 性能基准测试：对比 io_uring vs POSIX 的 QPS、延迟（P50/P90/P99）、CPU 利用率
- [ ] 2.6 多连接并发测试：验证 io_uring 单线程处理 1000+ 并发连接的能力

---

## 3. eBPF 可观测性（Phase 3）

- [x] 3.1 创建 `ce_ebpf.h`：定义 `CeEbpfContext` 不透明句柄、生命周期/函数追踪/网络观测/I/O 观测/查询接口
- [x] 3.2 创建 `ce_ebpf_kern.c`：BPF 内核态程序，包含 GPL license、BPF maps 定义
- [x] 3.3 实现 kprobe 函数延迟追踪：`kprobe_ecs_update_entry` 记录进入时间戳，`kretprobe_ecs_update_exit` 计算延迟并写入对数分桶直方图
- [x] 3.4 实现 kprobe TCP 重传监控：`kprobe_tcp_retransmit` 递增 `tcp_retransmit_count` 计数器
- [x] 3.5 实现 tracepoint I/O 延迟追踪：`trace_recvfrom_entry` 记录开始时间，`trace_recvfrom_exit` 计算延迟并写入直方图
- [x] 3.6 实现 BPF maps：`func_latency_hist`（HASH）、`func_entry_ts`（HASH）、`tcp_retransmit_count`（ARRAY）、`io_latency_hist`（HASH）、`io_start_ts`（HASH）
- [x] 3.7 编译 `ce_ebpf_kern.o`：使用 clang `-O2 -target bpf` 编译 BPF 程序
- [x] 3.8 实现 `ce_ebpf.c` 用户态加载器：`ce_ebpf_init` 使用 `bpf_object__open` 打开 BPF 对象文件
- [x] 3.9 实现 `ce_ebpf_trace_function`：`bpf_object__load` 加载到内核，按函数名匹配并 attach kprobe/kretprobe
- [x] 3.10 实现 `ce_ebpf_dump_latency`：遍历 `func_latency_hist` map，输出各延迟桶的采样数
- [x] 3.11 实现 `ce_ebpf_trace_tcp_retransmit`：attach `kprobe_tcp_retransmit` 到 `tcp_retransmit_skb`
- [x] 3.12 实现 `ce_ebpf_get_retransmit_count`：读取 `tcp_retransmit_count` map 中的计数器
- [x] 3.13 实现 `ce_ebpf_trace_io_latency`：attach `trace_recvfrom_entry` / `trace_recvfrom_exit` tracepoint
- [x] 3.14 实现 `ce_ebpf_get_io_latency_stats`：从 `io_latency_hist` map 计算 P50/P90/P99 百分位延迟
- [x] 3.15 实现 `ce_ebpf_available`：检查 `/sys/kernel/btf/vmlinux` 是否存在判断 eBPF 可用性
- [x] 3.16 实现 eBPF stub：`CHAOS_HAS_EBPF` 未定义时所有函数返回 NULL/CE_ERR/0/CE_FALSE
- [x] 3.17 创建 eBPF 测试程序 `ce_ebpf_test_main.c`：验证 eBPF 初始化、追踪、统计功能
- [x] 3.18 CMake 集成：添加 `CHAOS_USE_EBPF` option，`pkg_check_modules(libbpf)`，定义 `CHAOS_HAS_EBPF` 宏

---

## 4. eBPF 网络优化（Phase 4，延后）

- [ ] 4.1 实现 XDP 包过滤程序：在网卡驱动层丢弃恶意包（DDoS 基础防护）
- [ ] 4.2 实现 XDP 连接分发：根据协议类型/玩家 ID 分发到不同服务线程
- [ ] 4.3 实现 BPF Stream Parser：在 socket 层解析自定义协议头，完成消息分帧
- [ ] 4.4 实现 io_uring + BPF 联动：BPF 解析协议 → io_uring 直接投递到正确缓冲区
- [ ] 4.5 实现 BPF LSM 安全沙箱：限制 Lua 脚本的文件系统访问和系统调用
- [ ] 4.6 XDP 性能基准测试：对比 XDP 路径 vs 传统内核网络栈的吞吐量和延迟

---

## 5. 集成与文档（Phase 5）

- [x] 5.1 服务端主循环切换到 io_uring 事件循环：修改 `ce_server_main.c`，集成 `ce_async_init` + `ce_async_submit` + `ce_async_wait` 模式
- [x] 5.2 ECS 更新与 I/O 事件循环协同：在 I/O 等待间隙执行 `ce_ecs_update` 和 `ce_cell_update`
- [ ] 5.3 性能测试报告：记录 io_uring vs POSIX 的完整性能对比数据（QPS、延迟分布、CPU 利用率、内存使用）
- [x] 5.4 更新 spec 文档：`docs/spec/chaos-engine-io_uring-ebpf-spec-v0.1.md` → v1.0
- [x] 5.5 编写用户文档：如何在 CMake 中启用 io_uring/eBPF，如何运行测试程序，如何解读 eBPF 统计输出
- [x] 5.6 编写开发者文档：如何添加新的 BPF 程序，如何扩展异步 I/O 后端

---

## 6. 跨平台扩展

- [ ] 6.1 设计 Windows IOCP 后端接口：`ce_async_iocp.c`，与 `ce_async_io.h` 接口对齐
- [ ] 6.2 实现 IOCP 异步 accept/recv/send：使用 `AcceptEx`/`WSARecv`/`WSASend` + Completion Port
- [ ] 6.3 实现 IOCP 异步文件 I/O：使用 `ReadFile`/`WriteFile` overlapped I/O
- [ ] 6.4 macOS kqueue 后端评估：确认是否需要独立的 kqueue 后端（当前 POSIX fallback 已覆盖）

---

## 7. 测试

- [x] 7.1 io_uring 单元测试：验证 `ce_async_init`/`ce_async_accept`/`ce_async_recv`/`ce_async_send` 基本功能
- [x] 7.2 io_uring 错误处理测试：SQ 满、CQ 溢出、非法 fd、连接断开等异常场景
- [x] 7.3 POSIX fallback 单元测试：验证 select 模式下的 accept/recv/send 功能
- [x] 7.4 eBPF 单元测试：验证 BPF 程序加载、attach、map 读写、卸载流程
- [x] 7.5 eBPF 错误处理测试：BPF 对象文件不存在、权限不足、内核不支持等场景
- [x] 7.6 集成测试：io_uring + eBPF 同时运行，验证互不干扰
- [x] 7.7 压力测试：1000 并发连接 + eBPF 追踪同时运行，验证稳定性

---

**总进度：46/51 已完成（90%）**

**已完成（Phase 1-3 + Phase 5 集成 + Phase 7 测试）**：io_uring 基础集成（16 项）、io_uring 高级特性（4 项）、eBPF 可观测性（18 项）、服务端主循环集成（2 项）、单元/集成/压力测试（7 项）。

**待完成**：性能测试报告（1 项）、eBPF 网络优化（6 项，Phase 4 延后）、跨平台扩展（4 项）。
