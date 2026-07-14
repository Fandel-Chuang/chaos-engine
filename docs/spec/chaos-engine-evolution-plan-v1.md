# ChaosEngine 架构演进计划 v1

> **基于：** `docs/spec/chaos-engine-architecture-review-v1.md` 架构审查报告
> **目标：** 高性能全球同服 MMO 引擎
> **日期：** 2026-07-15

---

## 总览

| 阶段 | 主题 | 任务数 | 预估工期 |
|------|------|--------|---------|
| Phase 1 | 基础设施加固 + 微服务化 | 8 | 2-3 个月 |
| Phase 2 | 分布式能力 | 8 | 3-4 个月 |
| Phase 3 | 性能与安全 | 6 | 2-3 个月 |
| Phase 4 | 可观测性与运维 | 4 | 1-2 个月 |
| **合计** | | **26** | **8-12 个月** |

---

## Phase 1: 基础设施加固 + 微服务化（2-3 个月）

> **目标：** 修复致命缺陷，使架构可水平扩展；同时引入微服务 RPC 框架，将好友/公会/邮件等功能拆分为独立微服务。

### 1.1 DBProxy 替换 mongosh → libmongoc 原生 C 驱动

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

### 1.7 ★ 微服务 RPC 框架（协程 RPC + 服务注册/发现）

| 项 | 内容 |
|---|---|
| **问题** | 当前所有游戏逻辑耦合在 Game Server 进程内，无法独立部署 |
| **目标** | C 语言协程 RPC 框架，支持微服务独立上线/扩缩容 |
| **改动文件** | 新建 `src_c/rpc/` 模块 |
| **依赖** | 无 |
| **优先级** | P0 |
| **预估** | 3 周 |

**设计要点：**

**A. 协程 RPC 核心**

```c
// src_c/rpc/ce_coroutine.h - 协程调度器
typedef struct CeCoroutine CeCoroutine;
typedef void (*CeCoEntryFn)(void* arg);

CeCoroutine* ce_co_create(CeCoEntryFn fn, void* arg, size_t stack_size);
void         ce_co_yield(void);           // 让出 CPU
void         ce_co_resume(CeCoroutine* co);
void         ce_co_destroy(CeCoroutine* co);

// src_c/rpc/ce_rpc.h - 协程化 RPC
typedef struct CeRpcServer CeRpcServer;
typedef struct CeRpcClient CeRpcClient;

// 服务端: 注册 RPC 方法
CeRpcServer* ce_rpc_server_create(const char* service_name, int port);
CeResult ce_rpc_register(CeRpcServer* srv, const char* method,
                          CeRpcHandlerFn handler);

// 客户端: 协程化调用 (当前协程挂起等待响应，不阻塞线程)
CeRpcClient* ce_rpc_client_create(const char* registry_addr);
CeResult ce_rpc_call(CeRpcClient* cli, const char* service,
                      const char* method,
                      const uint8_t* req, uint32_t req_len,
                      uint8_t** resp, uint32_t* resp_len);
// 异步 RPC (回调模式)
CeResult ce_rpc_call_async(CeRpcClient* cli, const char* service,
                             const char* method,
                             const uint8_t* req, uint32_t req_len,
                             CeRpcCallbackFn callback, void* user_data);
```

**B. 服务注册/发现**

```c
// src_c/rpc/ce_service_registry.h
typedef struct CeServiceRegistry CeServiceRegistry;

// 服务注册 (微服务启动时向 Registry 注册)
CeResult ce_registry_register(CeServiceRegistry* reg,
                                const char* service_name,
                                const char* host, int port,
                                const char* metadata);  // JSON 元数据

// 服务发现 (RPC 调用前查询目标地址)
CeResult ce_registry_lookup(CeServiceRegistry* reg,
                              const char* service_name,
                              char* out_host, int* out_port);

// 健康检查 + 自动注销
CeResult ce_registry_heartbeat(CeServiceRegistry* reg,
                                 const char* service_name);
```

**C. 协程调度集成 io_uring**

```
RPC 请求流程:
  1. Game Server 协程调用 ce_rpc_call("friend_service", "get_list", ...)
  2. 当前协程 yield 挂起，io_uring 异步发送请求
  3. io_uring 事件循环继续处理其他连接
  4. 响应到达 → io_uring 完成事件 → 唤醒挂起的协程
  5. ce_rpc_call 返回结果

优势: 单线程内数万并发 RPC，不阻塞，无需回调地狱
```

**技术选型：**
- 协程实现: ucontext (POSIX) 或 libco (腾讯开源，C 协程库)
- 序列化: FlatBuffers (已有依赖) 或 Protocol Buffers
- 传输: io_uring 异步 TCP（复用现有 `ce_async_uring.c`）
- 服务发现: etcd / 内置 Raft（与 Phase 2 Router 共识层复用）

### 1.8 ★ 微服务拆分：好友/公会/邮件服务独立部署

| 项 | 内容 |
|---|---|
| **问题** | 当前 `src_lua/services/friend.lua` 耦合在 Game Server 内，无法独立扩缩容 |
| **目标** | 好友/公会/邮件拆为独立微服务进程，通过协程 RPC 与 Game Server 通信 |
| **改动文件** | 新建 `src_c/services/friend_service.c` `guild_service.c` `mail_service.c` |
| **依赖** | 1.7 (RPC 框架) |
| **优先级** | P1 |
| **预估** | 3 周 |

**架构：**

```
                 ┌──────────────────┐
                 │   Game Server    │
                 │   (战斗/移动/AOI) │
                 └────────┬─────────┘
                          │ 协程 RPC
           ┌──────────────┼──────────────┐
           │              │              │
  ┌────────▼───────┐ ┌───▼────────┐ ┌──▼───────────┐
  │ Friend Service │ │Guild Service│ │ Mail Service │
  │ (独立进程)      │ │(独立进程)    │ │(独立进程)     │
  │ 好友列表/申请   │ │公会管理/成员 │ │邮件收发/附件  │
  │ 黑名单          │ │公会战        │ │系统邮件      │
  └───────┬────────┘ └───┬────────┘ └──┬───────────┘
          │              │              │
          └──────────────┼──────────────┘
                         │
                 ┌───────▼────────┐
                 │   Service      │
                 │   Registry     │
                 │   (etcd/Raft)  │
                 └────────────────┘
```

**微服务进程模板：**
```c
// src_c/services/friend_service_main.c
int main(int argc, char** argv) {
    // 1. 初始化 RPC 服务端
    CeRpcServer* srv = ce_rpc_server_create("friend_service", 9200);

    // 2. 注册 RPC 方法
    ce_rpc_register(srv, "get_friend_list",   handle_get_friend_list);
    ce_rpc_register(srv, "add_friend",        handle_add_friend);
    ce_rpc_register(srv, "remove_friend",     handle_remove_friend);
    ce_rpc_register(srv, "get_block_list",    handle_block_list);

    // 3. 连接服务注册中心
    CeServiceRegistry* reg = ce_registry_connect("127.0.0.1:2379");
    ce_registry_register(reg, "friend_service", "0.0.0.0", 9200, "");

    // 4. 启动 io_uring 事件循环 (内置协程调度)
    ce_rpc_server_run(srv);  // 阻塞，内部 io_uring + 协程
    return 0;
}
```

**Game Server 调用示例（协程模式）：**
```c
// Game Server 内某协程处理玩家请求
void handle_player_get_friends(CeCoroutine* co, void* arg) {
    uint64_t player_id = *(uint64_t*)arg;

    // 协程 RPC 调用 — 当前协程挂起，不阻塞线程
    FriendList* list;
    CeResult ret = ce_rpc_call(g_rpc_client, "friend_service",
                                "get_friend_list",
                                &player_id, sizeof(player_id),
                                (uint8_t**)&list, NULL);
    if (ret == CE_OK) {
        // 发送给客户端
        send_friend_list_to_client(player_id, list);
    }
}
```

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

### 4.4 K8s 部署 + 自动扩缩容

| 项 | 内容 |
|---|---|
| **目标** | Helm Chart + HPA 自动扩缩容 + 故障自愈 |
| **改动文件** | 新建 `deploy/k8s/` |
| **依赖** | Phase 2 |
| **优先级** | P2 |
| **预估** | 3 周 |

---

## 依赖关系图

```
Phase 1 (基础设施 + 微服务化)
├── 1.1 DBProxy libmongoc ──────────────┐
├── 1.2 ECS 实例化 ──┬── 1.3 Cell 实例化 ── 1.4 AOI Grid ──┐
│                    └── 1.5 复制系统动态扩容                │
├── 1.6 Game Server 不直接 accept                            │
├── 1.7 ★ 微服务 RPC 框架 ─── 1.8 ★ 微服务拆分              │
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
组 D (微服务):  1.7 RPC 框架 → 1.8 微服务拆分
```

A/B/C/D 四组无相互依赖，可完全并行开发。

---

## 新增模块清单

| 模块 | 目录 | 阶段 |
|------|------|------|
| 协程库 | `src_c/rpc/ce_coroutine.c/h` | Phase 1 |
| RPC 框架 | `src_c/rpc/ce_rpc.c/h` | Phase 1 |
| 服务注册中心 | `src_c/rpc/ce_service_registry.c/h` | Phase 1 |
| 好友服务 | `src_c/services/friend_service.c` | Phase 1 |
| 公会服务 | `src_c/services/guild_service.c` | Phase 1 |
| 邮件服务 | `src_c/services/mail_service.c` | Phase 1 |
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

---

## 验收标准

每个任务完成后需通过 LoopEngine 四域闭环验证：
- ✅ dev: 编译无 warning，无内存泄漏（Valgrind）
- ✅ test: 单元测试覆盖新增模块
- ✅ verify: 集群冒烟测试通过
- ✅ client_ui: 客户端 Vulkan 启动正常
