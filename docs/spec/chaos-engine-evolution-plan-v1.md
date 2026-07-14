# ChaosEngine 架构演进计划 v1

> **基于：** `docs/spec/chaos-engine-architecture-review-v1.md` 架构审查报告
> **目标：** 高性能全球同服 MMO 引擎
> **日期：** 2026-07-15

---

## 总览

| 阶段 | 主题 | 任务数 | 预估工期 |
|------|------|--------|---------|
| Phase 1 | 基础设施加固 + 微服务化 | 7 | 2-3 个月 |
| Phase 2 | 分布式能力 | 8 | 3-4 个月 |
| Phase 3 | 性能与安全 | 6 | 2-3 个月 |
| Phase 4 | 可观测性与运维 | 5 | 1-2 个月 |
| **合计** | | **26** | **8-12 个月** |

---

## Phase 1: 基础设施加固 + 微服务化（2-3 个月）

> **目标：** 修复致命缺陷，使架构可水平扩展；同时搭建微服务基础设施（协程RPC + 服务发现 + 服务治理），为后续好友/公会/邮件等微服务提供可用的框架接口。

### 1.1 DBProxy 替换 mongosh -> libmongoc 原生 C 驱动

| 项 | 内容 |
|---|---|
| **问题** | `handler.lua` 通过 `io.popen("mongosh ...")` 每次 fork 进程，<10 writes/s |
| **目标** | 使用 libmongoc C 原生驱动，10,000+ writes/s |
| **改动文件** | 新建 `src_c/dbproxy/ce_dbproxy_native.c/h`，废弃 `src_lua/dbproxy/handler.lua` |
| **依赖** | 无 |
| **优先级** | P0 |
| **预估** | 2 周 |

**关键 API：**
```c
CeDbproxyCtx* ce_dbproxy_init(const char* mongo_uri, int pool_size);
CeResult ce_dbproxy_save(CeDbproxyCtx* ctx, uint64_t player_id,
                          const uint8_t* data, uint32_t len);
CeResult ce_dbproxy_load(CeDbproxyCtx* ctx, uint64_t player_id,
                          uint8_t** out_data, uint32_t* out_len);
CeResult ce_dbproxy_batch_save(CeDbproxyCtx* ctx,
                                const uint64_t* ids, const uint8_t** datas,
                                const uint32_t* lens, int count);
```

### 1.2 ECS 实例化（全局单例 → 结构体）

| 项 | 内容 |
|---|---|
| **问题** | `g_ecs` 全局静态，无法多实例（每 Cell 需独立 ECS 世界） |
| **目标** | `CeEcsWorld*` 实例化，支持每 Cell 一个独立 ECS 世界 |
| **改动文件** | `src_c/ecs/ce_ecs.c` `src_c/public_api/ce_ecs.h` |
| **依赖** | 无 |
| **优先级** | P0 |
| **预估** | 3 周 |

**改造：**
- `g_ecs` → `CeEcsWorld*`，所有 API 增加 `CeEcsWorld* world` 首参
- Archetype 数组从固定 256 → 动态扩容
- System 数组从固定 64 → 动态扩容

### 1.3 Cell 管理器实例化（全局单例 → 结构体）

| 项 | 内容 |
|---|---|
| **问题** | `g_cell` 全局静态，`ce_cell_assign_process` 只标记不通信 |
| **目标** | `CeCellManager*` 实例化，支持跨进程 Cell 分配 |
| **改动文件** | 新建 `src_c/server/ce_cell_manager.c/h`，重构 `ce_cell.c` |
| **依赖** | 1.2 (ECS 实例化) |
| **优先级** | P0 |
| **预估** | 2 周 |

### 1.4 AOI 从十字链表改为 Grid O(1) 查询

| 项 | 内容 |
|---|---|
| **问题** | 十字链表 O(n) 遍历，万级实体性能崩溃；仅 2D，无 Z 轴 |
| **目标** | Grid-based AOI，O(1) 查询；支持 2D/3D |
| **改动文件** | 新建 `src_c/server/ce_aoi_grid.c/h`，逐步替换 `ce_aoi.c` |
| **依赖** | 1.3 (Cell 实例化) |
| **优先级** | P0 |
| **预估** | 2 周 |

### 1.5 复制系统动态扩容

| 项 | 内容 |
|---|---|
| **问题** | 固定 `mailbox_keys[4096]`，脏标表 4096，满时静默丢数据 |
| **目标** | 动态哈希表，支持百万级实体 |
| **改动文件** | `src_c/replication/ce_replication_internal.h` `ce_replication.c` |
| **依赖** | 1.2 (ECS 实例化) |
| **优先级** | P0 |
| **预估** | 1 周 |

### 1.6 Game Server 不再直接 accept 客户端

| 项 | 内容 |
|---|---|
| **问题** | `ce_server_main.c` 直接 `listen(7777)` 接受客户端，绕过 Gateway |
| **目标** | Game Server 只接受 Gateway 转发的连接，不直接面向客户端 |
| **改动文件** | `src_c/runtime/ce_server_main.c` `src_c/server/ce_game_session.c` |
| **依赖** | 无（Gateway 增强可并行） |
| **优先级** | P0 |
| **预估** | 1 周 |

### 1.7 ★ 微服务基础设施：协程 RPC + 服务注册/发现/治理

| 项 | 内容 |
|---|---|
| **问题** | 当前所有游戏逻辑耦合在 Game Server 进程内，无 RPC 框架，无法拆分独立微服务 |
| **目标** | 搭建可用的微服务架构接口：协程 RPC + 服务注册/发现 + 服务治理（健康检查/负载均衡/熔断限流），**不实现具体业务功能** |
| **改动文件** | 新建 `src_c/rpc/` 模块 |
| **依赖** | 无 |
| **优先级** | P0 |
| **预估** | 4 周 |

> **范围说明：** 本任务只搭建微服务框架接口（协程调度器 + RPC 传输 + 服务注册/发现 + 治理策略），提供 `echo` / `ping` 示例验证链路通畅。好友/公会/邮件等具体业务功能在框架就绪后按需实现，不在本阶段。

**设计要点：**

**A. 协程调度器（ucontext 实现，集成 io_uring）**

```c
// src_c/rpc/ce_coroutine.h
typedef struct CeCoroutine CeCoroutine;
typedef void (*CeCoEntryFn)(void* arg);

CeCoroutine* ce_co_create(CeCoEntryFn fn, void* arg, size_t stack_size);
void         ce_co_yield(void);           // 让出 CPU，回到调度器
void         ce_co_resume(CeCoroutine* co); // 恢复指定协程
void         ce_co_destroy(CeCoroutine* co);

// 协程调度器（集成 io_uring 事件循环）
typedef struct CeScheduler CeScheduler;
CeScheduler* ce_sched_create(void);
void         ce_sched_run(CeScheduler* sched);   // 阻塞运行
// io_uring 完成事件 → 唤醒对应挂起的协程
void         ce_sched_on_io_complete(CeScheduler* sched, uint64_t co_id);
```

**B. 协程化 RPC（同步写法，异步执行）**

```c
// src_c/rpc/ce_rpc.h
typedef struct CeRpcServer CeRpcServer;
typedef struct CeRpcClient CeRpcClient;

// RPC 处理函数签名（在服务端协程中执行）
typedef CeResult (*CeRpcHandlerFn)(const uint8_t* req, uint32_t req_len,
                                     uint8_t** resp, uint32_t* resp_len);

// 服务端: 创建 RPC 服务并注册方法
CeRpcServer* ce_rpc_server_create(const char* service_name, int port);
CeResult ce_rpc_register(CeRpcServer* srv, const char* method,
                          CeRpcHandlerFn handler);
CeResult ce_rpc_server_run(CeRpcServer* srv);  // 阻塞，内部 io_uring + 协程

// 客户端: 协程化调用 (当前协程挂起等待响应，不阻塞线程)
CeRpcClient* ce_rpc_client_create(CeServiceRegistry* reg);
CeResult ce_rpc_call(CeRpcClient* cli, const char* service,
                      const char* method,
                      const uint8_t* req, uint32_t req_len,
                      uint8_t** resp, uint32_t* resp_len);

// 异步 RPC (回调模式，适用于非协程上下文)
typedef void (*CeRpcCallbackFn)(CeResult result,
                                  const uint8_t* resp, uint32_t resp_len,
                                  void* user_data);
CeResult ce_rpc_call_async(CeRpcClient* cli, const char* service,
                             const char* method,
                             const uint8_t* req, uint32_t req_len,
                             CeRpcCallbackFn callback, void* user_data);
```

**C. 服务注册/发现**

```c
// src_c/rpc/ce_service_registry.h
typedef struct CeServiceRegistry CeServiceRegistry;

// 连接注册中心 (etcd 或内置简易注册中心)
CeServiceRegistry* ce_registry_connect(const char* addr);

// 服务注册 (微服务启动时注册)
CeResult ce_registry_register(CeServiceRegistry* reg,
                                const char* service_name,
                                const char* host, int port,
                                const char* metadata);  // JSON: {"version":"1.0","weight":100}

// 服务发现 (RPC 调用前查询目标地址，支持负载均衡策略)
CeResult ce_registry_lookup(CeServiceRegistry* reg,
                              const char* service_name,
                              char* out_host, int* out_port);

// 服务注销 (微服务下线时调用)
CeResult ce_registry_deregister(CeServiceRegistry* reg,
                                  const char* service_name);
```

**D. 服务治理（健康检查 + 负载均衡 + 熔断限流）**

```c
// src_c/rpc/ce_governance.h

// 健康检查: 注册中心定期探活，失败自动摘除
CeResult ce_registry_health_check(CeServiceRegistry* reg,
                                    const char* service_name);

// 负载均衡策略
typedef enum {
    CE_LB_ROUND_ROBIN,    // 轮询
    CE_LB_RANDOM,         // 随机
    CE_LB_LEAST_CONN,     // 最少连接
    CE_LB_WEIGHTED,       // 加权（按 metadata.weight）
    CE_LB_CONSISTENT_HASH // 一致性哈希（按 entity_id 粘性）
} CeLbStrategy;
CeResult ce_registry_set_lb_strategy(CeServiceRegistry* reg,
                                       const char* service_name,
                                       CeLbStrategy strategy);

// 熔断器: 连续失败 N 次自动熔断，一段时间后半开探测
typedef struct CeCircuitBreaker CeCircuitBreaker;
CeCircuitBreaker* ce_cb_create(int fail_threshold, int recovery_timeout_ms);
CeBool   ce_cb_allow_request(CeCircuitBreaker* cb);  // 是否放行
void     ce_cb_record_success(CeCircuitBreaker* cb);
void     ce_cb_record_failure(CeCircuitBreaker* cb);

// 限流器: per-service 令牌桶
typedef struct CeRateLimiter CeRateLimiter;
CeRateLimiter* ce_rl_create(int max_qps);
CeBool   ce_rl_try_acquire(CeRateLimiter* rl);  // 尝试获取令牌
```

**E. 协程调度集成 io_uring**

```
RPC 请求流程（协程模式）:
  1. Game Server 协程调用 ce_rpc_call("echo_service", "ping", ...)
  2. RPC Client 查注册中心发现目标地址 → io_uring 异步发送请求
  3. 当前协程 yield 挂起，io_uring 事件循环继续处理其他连接
  4. 响应到达 → io_uring 完成事件 → 唤醒挂起的协程
  5. ce_rpc_call 返回结果

优势: 单线程内数万并发 RPC，不阻塞，无需回调地狱
```

**F. 示例验证（验证链路通畅，非业务功能）**

```c
// src_c/rpc/example/echo_service.c — 内置 echo 服务，验证框架可用
int main() {
    CeRpcServer* srv = ce_rpc_server_create("echo_service", 9200);
    ce_rpc_register(srv, "ping",  handle_ping);   // 返回 "pong"
    ce_rpc_register(srv, "echo",  handle_echo);   // 原样返回
    CeServiceRegistry* reg = ce_registry_connect("127.0.0.1:2379");
    ce_registry_register(reg, "echo_service", "0.0.0.0", 9200, "");
    ce_rpc_server_run(srv);
}

// src_c/rpc/example/rpc_client_test.c — 验证协程 RPC 调用链路
void test_coroutine(CeCoroutine* co, void* arg) {
    CeRpcClient* cli = arg;
    uint8_t* resp; uint32_t len;
    ce_rpc_call(cli, "echo_service", "ping", NULL, 0, &resp, &len);
    // resp == "pong" → 链路验证通过
    assert(strcmp((char*)resp, "pong") == 0);
}
```

**技术选型：**
- 协程实现: ucontext (POSIX) 或 libco (腾讯开源，C 协程库)
- 序列化: FlatBuffers (已有依赖) 或 Protocol Buffers
- 传输: io_uring 异步 TCP（复用现有 `ce_async_uring.c`）
- 服务注册中心: etcd（生产）或内置简易注册中心（开发/测试，与 Phase 2 Router Raft 复用）

**验收标准：**
- [ ] `echo_service` 注册到注册中心，客户端可发现并调用
- [ ] 协程 RPC 调用链路通畅（ping → pong）
- [ ] 异步 RPC 回调模式正常工作
- [ ] 服务健康检查：kill echo_service 后注册中心自动摘除
- [ ] 负载均衡：启动 3 个 echo_service 实例，请求按策略分发
- [ ] 熔断器：模拟连续失败后自动熔断，恢复后半开探测

---

## Phase 2: 分布式能力（3-4 个月）

> **目标：** 实现 Cell 集群、跨服迁移、Ghost 同步

### 2.1 Router 从 Lua 迁移到 C（io_uring）

| 项 | 内容 |
|---|---|
| **问题** | Lua Router 单线程，高 QPS 路由查询性能不足 |
| **目标** | 纯 C Router，io_uring 事件驱动 |
| **改动文件** | 新建 `src_c/router/`，迁移 `src_lua/router/` 逻辑 |

| **依赖** | Phase 1 |
| **优先级** | P0 |
| **预估** | 4 周 |

### 2.2 Router 增加 Raft 共识

| 项 | 内容 |
|---|---|
| **问题** | Leader 选举是"最小 node_id wins"伪选举，非真正共识 |
| **目标** | Raft Leader 选举 + 日志复制 |
| **改动文件** | 新建 `src_c/router/ce_raft.c/h` |
| **依赖** | 2.1 |
| **优先级** | P0 |
| **预估** | 4 周 |

### 2.3 实体路由表（entity_id → game_server_id）

| 项 | 内容 |
|---|---|
| **问题** | 无全局实体位置映射，Gateway 无法智能路由 |
| **目标** | 全局 entity_id → server_id 映射，持久化到 Raft 日志 |
| **改动文件** | 新建 `src_c/router/ce_router_entity_table.c/h` |
| **依赖** | 2.2 |
| **优先级** | P0 |
| **预估** | 2 周 |

### 2.4 Cell 迁移协议

| 项 | 内容 |
|---|---|
| **问题** | `ce_cell_split` 后 entity_count=0，实体数据丢失 |
| **目标** | 完整迁移协议：序列化→传输→反序列化→AOI 重建→Router 通知 |
| **改动文件** | 新建 `src_c/server/ce_cell_migration.c/h` |
| **依赖** | 2.3, Phase 1 |
| **优先级** | P0 |
| **预估** | 4 周 |

### 2.5 Ghost 实体管理

| 项 | 内容 |
|---|---|
| **问题** | 无跨进程 AOI，边界实体无法查询邻居 Cell |
| **目标** | Ghost 影子副本：边界实体属性复制到邻居 Cell |
| **改动文件** | 新建 `src_c/server/ce_ghost_manager.c/h` |
| **依赖** | 2.4, 1.4 |
| **优先级** | P0 |
| **预估** | 3 周 |

### 2.6 跨 Cell AOI 查询

| 项 | 内容 |
|---|---|
| **问题** | `ce_cell_query_nearby` 直接委托全局 AOI，跨 Cell 只是注释 |
| **目标** | 跨 Cell 边界 AOI 查询，合并本地 + Ghost 结果 |
| **改动文件** | `ce_aoi_grid.c` 增加 `ce_aoi_grid_query_cross_cell()` |
| **依赖** | 2.5 |
| **优先级** | P0 |
| **预估** | 2 周 |

### 2.7 DBProxy 分片

| 项 | 内容 |
|---|---|
| **问题** | 单 DBProxy 主备，无分片 |
| **目标** | 按 `player_id` 哈希分片，每个 DBProxy 管理一部分玩家 |
| **改动文件** | `src_c/dbproxy/ce_dbproxy_native.c` 增加分片路由 |
| **依赖** | 1.1 |
| **优先级** | P1 |
| **预估** | 3 周 |

### 2.8 事件总线（Redis Stream）

| 项 | 内容 |
|---|---|
| **问题** | 无异步事件总线，所有操作同步执行 |
| **目标** | Redis Stream 发布/订阅，跨服务事件通知 |
| **改动文件** | 新建 `src_c/eventbus/ce_eventbus.c/h` |
| **依赖** | 无 |
| **优先级** | P1 |
| **预估** | 2 周 |

---

## Phase 3: 性能与安全（2-3 个月）

> **目标：** 多线程 ECS、安全层、全球加速

### 3.1 ECS 多线程调度器

| 项 | 内容 |
|---|---|
| **问题** | `ce_ecs_update` 单线程线性执行所有 System |
| **目标** | System 依赖图 + Work-stealing 线程池 |
| **改动文件** | 新建 `src_c/ecs/ce_ecs_scheduler.c/h` |
| **依赖** | Phase 1 (ECS 实例化) |
| **优先级** | P0 |
| **预估** | 4 周 |

### 3.2 TLS/DTLS 传输加密

| 项 | 内容 |
|---|---|
| **问题** | 无加密、无认证、无防重放 |
| **目标** | TLS 1.3 (TCP) / DTLS 1.3 (KCP/UDP) |
| **改动文件** | `src_c/network/` 增加 TLS 层 |
| **依赖** | 无 |
| **优先级** | P0 |
| **预估** | 3 周 |

### 3.3 JWT 认证

| 项 | 内容 |
|---|---|
| **问题** | `MSG_JOIN_REQUEST` 无 token 认证载荷 |
| **目标** | JWT Token 签发/验证 + Refresh Token |
| **改动文件** | 新建 `src_c/auth/ce_jwt.c/h` |
| **依赖** | 无 |
| **优先级** | P0 |
| **预估** | 2 周 |

### 3.4 QUIC 协议支持

| 项 | 内容 |
|---|---|
| **问题** | 跨区走裸 TCP，跨洲延迟 200ms+ |
| **目标** | QUIC 0-RTT + 多路复用 + 连接迁移 |
| **改动文件** | 新建 `src_c/network/ce_quic.c/h` (基于 quiche/lsquic) |
| **依赖** | 无 |
| **优先级** | P1 |
| **预估** | 4 周 |

### 3.5 客户端预测 + reconciliation

| 项 | 内容 |
|---|---|
| **问题** | 纯服务端权威，高延迟下体验差 |
| **目标** | 客户端预测 + 服务端纠正回滚重放 + 快照插值 |
| **改动文件** | `src_c/runtime/ce_client_main.c` `src_c/sync/ce_sync.c` |
| **依赖** | 无 |
| **优先级** | P1 |
| **预估** | 4 周 |

### 3.6 反作弊框架

| 项 | 内容 |
|---|---|
| **问题** | 位置更新无验证，客户端可伪造坐标 |
| **目标** | 服务器端权威校验 + 移动速度检测 + 封包频率限制 |
| **改动文件** | 新建 `src_c/security/ce_anticheat.c/h` |
| **依赖** | 3.3 (JWT) |
| **优先级** | P1 |
| **预估** | 3 周 |

---

## Phase 4: 可观测性与运维（1-2 个月）

> **目标：** 生产级可观测性、自动化运维

### 4.1 OpenTelemetry 分布式追踪

| 项 | 内容 |
|---|---|
| **目标** | 跨服务分布式追踪，请求链路可视化 |
| **改动文件** | 新建 `src_c/observability/ce_trace.c/h` |
| **依赖** | 无 |
| **优先级** | P1 |
| **预估** | 2 周 |

### 4.2 Prometheus 指标采集

| 项 | 内容 |
|---|---|
| **目标** | 各进程内嵌 metrics 端点，Prometheus 拉取 |
| **改动文件** | 新建 `src_c/observability/ce_metrics.c/h` |
| **依赖** | 无 |
| **优先级** | P1 |
| **预估** | 1 周 |

### 4.3 结构化日志（JSON）+ 热更新

| 项 | 内容 |
|---|---|
| **目标** | JSON 结构化日志 + Lua 脚本热重载 + 配置热更新 |
| **改动文件** | `src_c/log/ce_log.c` 增强 |
| **依赖** | 无 |
| **优先级** | P1 |
| **预估** | 2 周 |

### 4.4 K8s 部署

| 项 | 内容 |
|---|---|
| **目标** | Helm Chart 部署全部服务，StatefulSet 管理有状态服务 |
| **改动文件** | 新建 `deploy/k8s/` |
| **依赖** | Phase 2 |
| **优先级** | P2 |
| **预估** | 2 周 |

### 4.5 ★ 自动扩缩容

| 项 | 内容 |
|---|---|
| **问题** | 无弹性伸缩能力，高峰期服务过载、低谷期资源浪费 |
| **目标** | 三层自动扩缩容：服务实例级 + Cell 级 + 跨区级 |
| **改动文件** | 新建 `src_c/autoscale/ce_autoscaler.c/h` + `deploy/k8s/hpa.yaml` |
| **依赖** | 4.2 (Prometheus 指标) + 4.4 (K8s) |
| **优先级** | P1 |
| **预估** | 3 周 |

**三层自动扩缩容设计：**

```
┌──────────────────────────────────────────────────────┐
│                 自动扩缩容架构                         │
│                                                      │
│  ┌────────────────────────────────────────────────┐  │
│  │  Layer 1: 服务实例级 (K8s HPA)                 │  │
│  │  指标: CPU/Memory/QPS/连接数                    │  │
│  │  动作: Gateway/DBProxy/微服务 Pod 自动伸缩      │  │
│  │  实现: K8s Horizontal Pod Autoscaler            │  │
│  └────────────────────────────────────────────────┘  │
│                                                      │
│  ┌────────────────────────────────────────────────┐  │
│  │  Layer 2: Cell 级 (引擎内部)                    │  │
│  │  指标: Cell 实体数/CPU/tick 耗时                │  │
│  │  动作:                                         │  │
│  │    - 过载 → Cell 自动分裂 + 迁移到新 Game Server │  │
│  │    - 空闲 → Cell 自动合并 + 回收 Game Server     │  │
│  │  实现: ce_autoscaler.c (内嵌 Prometheus 推送)   │  │
│  └────────────────────────────────────────────────┘  │
│                                                      │
│  ┌────────────────────────────────────────────────┐  │
│  │  Layer 3: 跨区级 (全局调度)                     │  │
│  │  指标: 区域玩家数/跨区延迟                       │  │
│  │  动作:                                         │  │
│  │    - 高峰 → 跨区迁移玩家到低负载区域              │  │
│  │    - 低谷 → 合并区域减少跨区开销                  │  │
│  │  实现: Router 全局调度器 + Anycast DNS 切换      │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

**API 设计：**

```c
// src_c/autoscale/ce_autoscaler.h

typedef struct CeAutoscaler CeAutoscaler;
typedef struct CeAutoscaleConfig {
    int      check_interval_sec;     // 检查间隔（默认 30s）
    int      cell_split_threshold;   // Cell 分裂阈值（实体数，默认 2000）
    int      cell_merge_threshold;   // Cell 合并阈值（实体数，默认 500）
    float    cpu_high_threshold;     // CPU 高水位（0.8 = 80%）
    float    cpu_low_threshold;      // CPU 低水位（0.2 = 20%）
    int      scale_up_cooldown_sec;  // 扩容冷却时间（防止抖动）
    int      scale_down_cooldown_sec;// 缩容冷却时间
} CeAutoscaleConfig;

// 创建自动扩缩容管理器
CeAutoscaler* ce_autoscaler_create(CeAutoscaleConfig* config,
                                     CeCellManager* cell_mgr,
                                     CeServiceRegistry* registry);

// 主检查循环（由 Game Server 主线程定期调用）
CeResult ce_autoscaler_tick(CeAutoscaler* scaler);

// 获取当前扩缩容状态
typedef struct CeAutoscaleStatus {
    int     total_cells;
    int     total_game_servers;
    int     pending_splits;     // 等待分裂的 Cell 数
    int     pending_merges;     // 等待合并的 Cell 数
    float   avg_cpu_usage;      // 平均 CPU 使用率
    float   avg_memory_usage;   // 平均内存使用率
} CeAutoscaleStatus;
CeResult ce_autoscaler_get_status(CeAutoscaler* scaler,
                                    CeAutoscaleStatus* out);
```

**K8s HPA 配置：**

```yaml
# deploy/k8s/hpa-gateway.yaml
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: chaos-gateway-hpa
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: chaos-gateway
  minReplicas: 2
  maxReplicas: 20
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 70
  - type: Resource
    resource:
      name: memory
      target:
        type: Utilization
        averageUtilization: 80
  - type: Pods                   # 自定义指标：连接数
    pods:
      metric:
        name: gateway_connections
      target:
        type: AverageValue
        averageValue: "5000"
```

**验收标准：**
- [ ] K8s HPA: Gateway Pod 在 CPU >70% 时自动扩容，<20% 时自动缩容
- [ ] Cell 自动分裂: Cell 实体数 >2000 时自动分裂并迁移到新 Game Server
- [ ] Cell 自动合并: Cell 实体数 <500 时自动合并并回收 Game Server
- [ ] 扩缩容冷却: 分裂/合并后冷却期内不重复触发
- [ ] Prometheus 自定义指标: gateway_connections / cell_entity_count 正常采集
- [ ] 跨区调度: 区域玩家数超限时触发跨区迁移提示

---

## 依赖关系图

```
Phase 1 (基础设施 + 微服务化)
├── 1.1 DBProxy libmongoc ──────────────┐
├── 1.2 ECS 实例化 ──┬── 1.3 Cell 实例化 ── 1.4 AOI Grid ──┐
│                    └── 1.5 复制系统动态扩容                │
├── 1.6 Game Server 不直接 accept                            │
├── 1.7 ★ 微服务 RPC 框架 (协程RPC+服务治理)                     │
│                                                            │
Phase 2 (分布式能力)                                         │
├── 2.1 Router C 化 ── 2.2 Raft ── 2.3 实体路由表           │
│                                    │                      │
│   2.4 Cell 迁移协议 ◄──────────────┘ ◄───────────────────┘
│   │
│   ├── 2.5 Ghost 管理 ── 2.6 跨 Cell AOI
│   │
├── 2.7 DBProxy 分片 ◄── 1.1
├── 2.8 事件总线
│
Phase 3 (性能与安全)
├── 3.1 ECS 多线程 ◄── Phase 1
├── 3.2 TLS/DTLS
├── 3.3 JWT ── 3.6 反作弊
├── 3.4 QUIC
├── 3.5 客户端预测
│
Phase 4 (可观测性)
├── 4.1 OTel 追踪
├── 4.2 Prometheus
├── 4.3 日志+热更新
├── 4.4 K8s ◄── Phase 2
```

---

## Phase 1 并行任务分组

Phase 1 内部可并行的任务组：

```
组 A (数据层):  1.1 DBProxy libmongoc
组 B (ECS/空间): 1.2 ECS 实例化 → 1.3 Cell 实例化 → 1.4 AOI Grid → 1.5 复制扩容
组 C (网关):    1.6 Game Server 不直接 accept
组 D (微服务):  1.7 协程 RPC + 服务注册/发现 + 服务治理（echo 示例验证）
```

A/B/C/D 四组无相互依赖，可完全并行开发。

---

## 新增模块清单

| 模块 | 目录 | 阶段 |
|------|------|------|
| 协程库 | `src_c/rpc/ce_coroutine.c/h` | Phase 1 |
| RPC 框架 | `src_c/rpc/ce_rpc.c/h` | Phase 1 |
| 服务注册中心 | `src_c/rpc/ce_service_registry.c/h` | Phase 1 |
| 服务治理 | `src_c/rpc/ce_governance.c/h` | Phase 1 |
| RPC 示例 | `src_c/rpc/example/echo_service.c` | Phase 1 |
| DBProxy 原生驱动 | `src_c/dbproxy/ce_dbproxy_native.c/h` | Phase 1 |
| Cell 管理器 | `src_c/server/ce_cell_manager.c/h` | Phase 1 |
| Grid AOI | `src_c/server/ce_aoi_grid.c/h` | Phase 1 |
| C Router | `src_c/router/` | Phase 2 |
| Raft 共识 | `src_c/router/ce_raft.c/h` | Phase 2 |
| 实体路由表 | `src_c/router/ce_router_entity_table.c/h` | Phase 2 |
| Cell 迁移 | `src_c/server/ce_cell_migration.c/h` | Phase 2 |
| Ghost 管理 | `src_c/server/ce_ghost_manager.c/h` | Phase 2 |
| 事件总线 | `src_c/eventbus/ce_eventbus.c/h` | Phase 2 |
| ECS 调度器 | `src_c/ecs/ce_ecs_scheduler.c/h` | Phase 3 |
| TLS/DTLS | `src_c/network/ce_tls.c/h` | Phase 3 |
| JWT 认证 | `src_c/auth/ce_jwt.c/h` | Phase 3 |
| QUIC 传输 | `src_c/network/ce_quic.c/h` | Phase 3 |
| 反作弊 | `src_c/security/ce_anticheat.c/h` | Phase 3 |
| 分布式追踪 | `src_c/observability/ce_trace.c/h` | Phase 4 |
| 指标采集 | `src_c/observability/ce_metrics.c/h` | Phase 4 |
| K8s 部署 | `deploy/k8s/` | Phase 4 |
| 自动扩缩容 | `src_c/autoscale/ce_autoscaler.c/h` | Phase 4 |

---

## 验收标准

每个任务完成后需通过 LoopEngine 四域闭环验证：
- ✅ dev: 编译无 warning，无内存泄漏（Valgrind）
- ✅ test: 单元测试覆盖新增模块
- ✅ verify: 集群冒烟测试通过
- ✅ client_ui: 客户端 Vulkan 启动正常
