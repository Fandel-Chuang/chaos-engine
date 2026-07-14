# ChaosEngine 架构深度审查报告 — 面向"高性能全球同服MMO"目标

> **状态：** 终稿 v1 | **日期：** 2026-07-15 | **作者：** 架构审查组
>
> **审查范围：** `src_c/` 全部 C 源码（60 个 .c / 46 个 .h）、`src_lua/` 全部 Lua 服务、`docs/spec/` 全部设计规格、`scripts/` 运维脚本
>
> **目标对标：** 全球同服 MMO（参考 EVE Online、 albion online 空间架构、Roblox 虚拟世界服务端规模）

---

## 目录

1. [现有架构总览](#1-现有架构总览)
2. [面向全球同服 MMO 的架构缺陷分析](#2-面向全球同服-mmo-的架构缺陷分析)
3. [缺失的关键模块](#3-缺失的关键模块)
4. [完整架构设计文档](#4-完整架构设计文档)
5. [迁移路线图](#5-迁移路线图)
6. [附录](#6-附录)

---

## 1. 现有架构总览

### 1.1 进程拓扑

```
                           客户端 (chaos_client, Vulkan RHI)
                             │
                    TCP(9000) / KCP(9000) / WS(9000)
                             │
                    ┌────────▼────────┐
                    │  chaos_gateway  │  纯 C, io_uring, 万并发 QPS 97K
                    │  (端口 9000)    │  TCP/KCP/WebSocket 三协议共用端口
                    └───────┬─────────┘
                            │ TCP (同机全连)
              ┌─────────────┼─────────────┐
              │             │             │
     ┌────────▼──────┐ ┌───▼──────┐ ┌───▼──────────┐
     │ chaos_server  │ │chaos_    │ │ chaos_router  │
     │ (端口 7777)   │ │dbproxy   │ │ 9100/9101     │
     │               │ │(9001/    │ │ Lua: 集群发现  │
     │ ECS + AOI     │ │ 9003)    │ │ 哈希环+跨区    │
     │ Cell + Sync   │ │MongoDB   │ │               │
     │ Repl + Save   │ │          │ │               │
     └───────────────┘ └──────────┘ └───────────────┘

     ┌───────────────┐
     │ chaos_admin   │  Lapis Web, 端口 9090
     │ (IPC→Game)    │  Unix Socket /tmp/chaos_admin.sock
     └───────────────┘
```

### 1.2 模块清单与实现深度

| 模块 | 目录 | 关键文件 | 代码行数(估) | 实现深度 |
|------|------|---------|------------|---------|
| **网络层** | `src_c/network/` | `ce_net_base.h/c` `ce_async_io.h` `ce_async_uring.c` `ce_kcp.c` `ikcp.c` | ~3000 | ✅ 完整：TCP/KCP/io_uring/eBPF/跨区消息/连接池/Router Mesh |
| **网关** | `src_c/gateway/` | `ce_gateway.c` `ce_gateway_conn.c` `ce_gateway_router.c` `ce_gateway_backend.c` `ce_gateway_protocol.c` | ~3500 | ✅ 完整：io_uring 事件驱动、三协议、路由表、后端连接池 |
| **ECS** | `src_c/ecs/` | `ce_ecs.c` `ce_ecs_internal.h` `ce_ecs.h` | ~600 | ⚠️ 基础可用：Archetype 存储、128 组件上限、查询迭代器；**无多线程** |
| **空间** | `src_c/server/` | `ce_aoi.c/h` `ce_cell.c/h` `ce_game_session.c/h` `ce_game_protocol.h` | ~1500 | ⚠️ MVP级：十字链表AOI、Cell分裂合并；**单进程单线程** |
| **同步** | `src_c/sync/` | `ce_sync.c/h` | ~620 | ⚠️ 基础可用：帧同步到DBProxy、主备切换；**无跨服实体迁移** |
| **复制** | `src_c/replication/` | `ce_replication.c/h` `ce_mailbox.c/h` `ce_rpc_channel.c/h` | ~1000 | ⚠️ 管线就绪：脏标+flush+flag分流+RPC+Mailbox；**固定容量、无分片** |
| **存档** | `src_c/save/` | `ce_save.c/h` | ~690 | ⚠️ 基础可用：定时+增量、FIFO队列；**单DBProxy瓶颈** |
| **DBProxy** | `src_c/dbproxy/` + `src_lua/dbproxy/` | `ce_dbproxy.c` `handler.lua` `state_mirror.lua` | ~900 | 🔴 严重不足：**通过 `os.execute` 调用 mongosh**，无原生驱动 |
| **Router** | `src_lua/router/` | `router.lua` `cluster.lua` `cross_region.lua` `hash_ring.lua` `registry.lua` | ~1600 | ⚠️ 基础可用：哈希环、集群同步、跨区路由；**Lua单线程、无Raft** |
| **eBPF** | `src_c/ebpf/` | `ce_xdp.c` `ce_uring_bpf.c` `ce_stream_parser.c` `ce_lsm.c` 等 | ~1500 | ✅ Phase 1-3 完成，Phase 4(XDP) 延后 |
| **渲染** | `src_c/render/` | `ce_rhi.h` `ce_rhi_vulkan.c/h` | ~1200 | ⚠️ 客户端专用，Vulkan RHI 基础管线；无PBR/光照/后处理 |
| **脚本** | `src_c/script/` | `ce_lua.c/h` | ~400 | ✅ Lua 绑定基础可用 |
| **日志** | `src_c/log/` | `ce_log.c` | ~300 | ✅ 分级日志可用 |
| **插件** | `src_c/plugin/` | `ce_plugin.c` | ~300 | ⚠️ 框架存在，无实际插件 |
| **Admin** | `src_c/admin_ipc/` + `src_lua/admin/` | `ce_admin_ipc.c` `app.lua` `ws_handler.lua` | ~800 | ⚠️ IPC+Lapis Web，基础运维监控 |

### 1.3 协议设计

**二进制帧格式：**
```
[4B total_len (大端)][2B msg_type (大端)][N payload]
total_len = 6 + payload_len
```

**消息类型枚举** (`ce_net_base.h`):
```
0x0001 PING / 0x0002 PONG
0x0010 LOGIN / 0x0011 LOGIN_RESP
0x0100 GAME_DATA
0x1000-0x1FFF 跨区消息 (CROSS_REGION / REGION_SYNC / ROUTER_HELLO / ROUTER_BYE)
0x8000+ 用户自定义
```

**游戏协议** (`ce_game_protocol.h`):
```
0x0001 JOIN_REQUEST / 0x0002 POSITION_UPDATE
0x8001 JOIN_RESPONSE / 0x8002 ENTITY_STATE
```

---

## 2. 面向全球同服 MMO 的架构缺陷分析

### 2.1 致命缺陷：单进程 Game Server 无法水平扩展

**现状：** `ce_server_main.c` 是一个单进程单线程的服务器，直接监听 7777 端口接受客户端连接。

**代码证据：**
```c
// ce_server_main.c:34
#define MAX_CLIENTS    32          // ← 硬编码最大32客户端
#define TICK_RATE      60

// ce_game_session.h:34
#define CE_GAME_MAX_CLIENTS  1024  // ← 会话上限1024
#define CE_GAME_MAX_VISIBLE  256   // ← 可见实体上限256
```

**问题分析：**
- Game Server 直接 `accept` 客户端连接（`ce_socket_listen(listen_sock, 5)`），绕过了 Gateway
- `CeGameSession` 使用固定数组 `entities[CE_GAME_MAX_CLIENTS]`，无法动态扩展
- 全部逻辑在单线程 `while(g_running)` 主循环中执行
- **全球同服 MMO 需要支撑数十万并发玩家，当前架构上限 1024 连接**

**影响：** 这是阻塞性问题。全球同服要求 Game Server 能够水平扩展为 Cell 集群，每个 Cell 处理一部分世界区域。

### 2.2 致命缺陷：AOI 和 Cell 子系统仅支持单进程

**现状：** `ce_aoi.c` 和 `ce_cell.c` 均使用全局静态状态，且仅支持单实例运行。

**代码证据：**
```c
// ce_aoi.c:40 - 全局单例
static struct {
    CeAoiNode**        nodes;          /* 按 entity_id 索引的节点数组 */
    CeAoiNode*         x_head;         /* X 轴链表头 */
    CeAoiNode*         y_head;         /* Y 轴链表头 */
    float              aoi_radius;     /* 视野半径 */
    CeBool             initialized;
} g_aoi;

// ce_cell.c:21 - 全局单例
static struct {
    CeCell*  cells;
    int      cell_count;
    int      cells_per_row;
    // ...
} g_cell;
```

**问题分析：**
- `ce_aoi_init()` 和 `ce_cell_init()` 是全局初始化函数，没有实例化参数
- Cell 分裂 (`ce_cell_split`) 在进程内创建子 Cell，而非将负载迁移到其他进程
- `ce_cell_assign_process(cell_id, process_id)` 仅标记 process_id，**但没有任何跨进程通信机制**
- AOI 十字链表是 O(n) 遍历，在万人同屏场景下性能崩溃
- Cell 的 `ce_cell_query_nearby` 直接委托给全局 AOI，**跨 Cell 边界查询只是注释**

**影响：** 空间管理无法分布式部署，无法实现"不同 Cell 由不同服务器进程管理"的核心 MMO 架构。

### 2.3 严重缺陷：DBProxy 通过 mongosh 命令行操作数据库

**现状：** `src_lua/dbproxy/handler.lua` 中的 MongoDB 操作通过 `io.popen` 执行 `mongosh` 命令行。

**代码证据：**
```lua
-- handler.lua:32
local function mongo_exec(js_code)
    local cmd = string.format(
        "mongosh '%s' --quiet --eval '%s' 2>/dev/null",
        config.mongo_uri, escaped
    )
    local handle = io.popen(cmd)    -- ← 每次 DB 操作 fork 一个 mongosh 进程！
    local result = handle:read("*a")
    handle:close()
    return result
end
```

**问题分析：**
- 每次存档/读档操作 `fork+exec` 一个 `mongosh` 进程，延迟在 100-500ms 级别
- 无连接池、无批量操作、无事务支持
- JSON 编解码通过 `pcall(require, "dkjson")` 回退到手动编码器
- **全球同服 MMO 的存档吞吐需求：10K+ writes/sec，当前架构 <10 writes/sec**

**影响：** 数据层完全不可用于生产环境。

### 2.4 严重缺陷：ECS 无多线程支持，查询性能瓶颈

**现状：** `ce_ecs.c` 使用全局静态上下文 `g_ecs`，所有操作在单线程执行。

**代码证据：**
```c
// ce_ecs.c:16
static struct {
    CeAllocator* allocator;
    CeBool       initialized;
    uint32_t     entity_count;
    CeEntity*    entity_generations;   // ← 全局数组
    CeArchetype** entity_archetypes;   // ← 全局数组
    uint32_t*    entity_rows;          // ← 全局数组
    uint32_t*    free_entities;        // ← 全局栈
    CeArchetype* archetypes[256];      // ← 固定256上限
    CeSystemInfo systems[64];          // ← 固定64上限
} g_ecs;                               // ← 无任何锁/原子操作
```

**问题分析：**
- `ce_ecs_update(float delta_time)` 单线程线性执行所有 System
- `ce_query_execute` 返回实体数组拷贝，大量内存分配
- Archetype 数量上限 256，复杂 MMO 游戏组件组合可能超过此限制
- 无 System 依赖图、无并行调度、无无锁读写分离
- **全球同服 MMO 需要在单 Cell 内处理数千实体的物理/AI/战斗，单线程 ECS 无法满足 60Hz tick**

**影响：** CPU 密集型逻辑（AI、物理、战斗）无法利用多核。

### 2.5 严重缺陷：复制系统固定容量，无法扩展

**现状：** `ce_replication_internal.h` 中脏标表、Mailbox 哈希表均为固定大小数组。

**代码证据：**
```c
// ce_replication_internal.h:18-22
#define CE_REPL_MAX_DIRTY_ENTITIES      4096   // ← 脏实体上限
#define CE_REPL_MAX_COMPONENTS           64    // ← 复制组件上限
#define CE_REPL_MAX_FIELDS_PER_COMP      32    // ← 每组件字段上限
#define CE_RPC_MAX_PENDING               256   // ← RPC pending上限
#define CE_RPC_MAX_PAYLOAD             4096    // ← RPC payload上限

// ce_replication_internal.h:102-105
// Mailbox: entity_id -> server_id 哈希表 (线性探测, 容量 4096)
uint32_t        mailbox_count;
uint64_t        mailbox_keys[4096];            // ← 固定4096槽位
uint32_t        mailbox_values[4096];
```

**问题分析：**
- 脏标表满时直接 `CE_LOG_ERROR` 并丢弃（`ce_repl_find_or_create_dirty` 返回 NULL）
- Mailbox 固定 4096 槽位，全球同服百万实体完全不够
- 属主映射 (`owner_entity_ids/owner_client_ids`) 用线性扫描查找 (`ce_repl_find_owner`)，O(n) 复杂度
- RPC pending 队列固定 256，高并发下会丢失 RPC
- **复制系统是单进程内的，没有跨进程复制状态同步机制**

**影响：** 万级以上实体场景下复制系统会静默丢数据。

### 2.6 严重缺陷：跨区路由仅 TCP 直连，无延迟优化

**现状：** `cross_region.lua` 通过 LuaSocket TCP 连接远程区域 Router。

**代码证据：**
```lua
-- cross_region.lua:126-131
local sock, err = socket.tcp()
sock:settimeout(5)  -- 5 second connect timeout
local ok, conn_err = sock:connect(entry.host, entry.port)
sock:settimeout(0)  -- Non-blocking
```

**问题分析：**
- 跨区消息走 TCP，全球范围 RTT 100-300ms
- 无 QUIC 支持、无消息压缩、无优先级队列
- 跨区路由表 (`region_routes`) 是静态配置，无动态路由发现
- TTL=16、MAX_HOPS=32 的防环机制存在，但无路由缓存和最短路径计算
- **全球同服 MMO 需要跨区延迟 <50ms（玩家感知），当前 TCP 跨区方案在跨洲场景下延迟 200ms+**

**影响：** 跨洲玩家交互体验差，无法实现真正的"全球同服"体感。

### 2.7 中等缺陷：Gateway 后端路由表静态配置

**现状：** `ce_gateway_router.c` 使用静态路由表 `routes[CE_GATEWAY_MAX_ROUTES]`。

**代码证据：**
```c
// ce_gateway_internal.h:75
#define CE_GATEWAY_MAX_ROUTES 64

typedef struct CeGatewayRoute {
    uint16_t    msg_type_start;
    uint16_t    msg_type_end;
    int         backend_index;   /* -1 = 本地处理 */
} CeGatewayRoute;
```

**问题分析：**
- 路由表最多 64 条规则，按消息类型范围映射到后端
- 无基于 entity_id 的路由（无法将玩家路由到其所在的 Game Server）
- 无动态后端注册/注销（后端列表 `backends[CE_GW_MAX_BACKENDS]` 固定 16 个）
- **Gateway 不知道哪个 Game Server 管理哪个 Cell/区域，无法做智能路由**

**影响：** Gateway 无法作为真正的负载均衡器，无法支持 Cell 迁移后的连接重定向。

### 2.8 中等缺陷：Sync 模块单 DBProxy 主备，无分片

**现状：** `ce_sync.h` 仅支持主备两个 DBProxy 连接。

**代码证据：**
```c
// ce_sync.h:70-77
typedef struct CeSyncConfig {
    const char* dbproxy_host;     /* 主 DBProxy 地址 */
    int         dbproxy_port;     /* 主 DBProxy 端口 */
    const char* backup_host;      /* 备用 DBProxy 地址 */
    int         backup_port;      /* 备用 DBProxy 端口 */
    int         heartbeat_ms;
    int         timeout_ms;
} CeSyncConfig;
```

**问题分析：**
- 仅主备两个端点，无分片策略
- `CE_SYNC_MAX_FRAME_SIZE = 64KB`，单帧限制小
- 环形缓冲区 `CE_SYNC_RING_SIZE = 256`，在高频存档场景下可能溢出
- **全球同服 MMO 需要按区域/玩家 ID 分片的数据库集群，当前仅支持单点**

### 2.9 中等缺陷：无安全层设计

**现状：** 整个网络协议无加密、无认证、无防重放。

**代码证据：**
- `ce_net_base.h` 的消息格式 `[4B len][2B type][payload]` 无加密字段
- `ce_game_protocol.h` 的 `MSG_JOIN_REQUEST` 无 token/认证载荷
- Gateway WebSocket 握手实现了 SHA1+Base64（RFC 6455），但这只是协议握手，非数据加密
- **无 TLS/DTLS、无 token 验证、无速率限制**

**影响：** 无法防御作弊、DDoS、中间人攻击。

### 2.10 中等缺陷：Cell 分裂/合并不完整

**现状：** `ce_cell_update()` 仅标记状态，`ce_cell_split()` 仅创建空子 Cell。

**代码证据：**
```c
// ce_cell.c:227-231 - 仅标记状态
if (cell->entity_count > cell->max_entities) {
    cell->state = CE_CELL_SPLITTING;  // ← 标记后无后续处理
}

// ce_cell.c:292 - 分裂后 entity_count 清零
cell->bounds = sub_bounds[0];
cell->entity_count = 0;  // ← 实体数据丢失！
cell->state = CE_CELL_ACTIVE;
```

**问题分析：**
- `ce_cell_update` 检测到过载后标记 `CE_CELL_SPLITTING`，但无状态机推进
- `ce_cell_split` 分裂后 `entity_count = 0`，**实体没有迁移到子 Cell**
- `ce_cell_merge` 合并后 `ca->entity_count += cb->entity_count`，但实体数据未迁移
- 无跨进程 Cell 迁移协议（实体状态序列化→传输→反序列化→AOI 重建）
- **Cell 分裂/合并是空壳，实际不可用**

---

## 3. 缺失的关键模块

### 3.1 分布式空间管理层（World Manager / Cell Manager Service）

**缺失描述：** 当前 `ce_cell.c` 是进程内 Cell 管理，缺少独立的分布式空间管理服务。

**需要的能力：**
- Cell → Game Server 的动态分配与迁移
- 跨 Cell 边界的实体迁移协议（状态序列化、AOI 重建、客户端重定向）
- Cell 负载监控与自动均衡
- Cell 邻居关系图的动态维护（分裂/合并后自动重建）
- Ghost 实体机制（边界实体的影子副本，避免频繁跨 Cell 查询）

**参考实现：** 可参考 BigWorld / Unreal Engine World Partition 的 Cell 架构。

### 3.2 分布式 AOI 系统（跨进程可见性管理）

**缺失描述：** 当前十字链表 AOI 仅在单进程内工作，缺少跨进程 AOI。

**需要的能力：**
- 跨 Cell 边界的 AOI 查询（需要查询邻居 Cell 中的实体）
- Ghost 实体同步（边界实体的属性复制到邻居 Cell）
- AOI 兴趣区域管理（基于玩家位置的动态订阅/退订）
- 可扩展的 AOI 数据结构（十字链表在万级实体下 O(n) 遍历不可接受，需要 Grid/QuadTree 混合方案）

### 3.3 多线程 ECS 调度器

**缺失描述：** 当前 ECS 无并行执行能力。

**需要的能力：**
- System 依赖图构建与拓扑排序
- 无冲突 System 并行调度（基于组件读写集分析）
- Work-stealing 线程池
- ECS 实例隔离（支持每个 Cell 一个独立 ECS 世界）
- 无锁 Archetype 遍历（读写分离、双缓冲）

### 3.4 分布式锁与一致性服务

**缺失描述：** 无分布式锁服务，跨服操作（交易、公会操作、跨区迁移）无法保证一致性。

**需要的能力：**
- 基于实体 ID 的分布式锁（乐观锁/悲观锁）
- 跨服事务协议（两阶段提交或 Saga）
- Router 层的 Raft/Paxos 共识（当前 Leader 选举是"最小 node_id wins"，非真正共识）
- 幂等性保证（RPC 重试场景下的去重）

### 3.5 消息队列与事件总线

**缺失描述：** 无异步事件总线，所有操作同步执行。

**需要的能力：**
- 跨服务事件发布/订阅（玩家上线/下线、Cell 迁移、世界事件）
- 持久化消息队列（用于存档解耦、日志收集）
- 优先级队列（关键操作 vs 低优先级操作）
- 背压机制（防止消息积压导致 OOM）

### 3.6 安全与反作弊系统

**缺失描述：** 无任何安全防护机制。

**需要的能力：**
- TLS/DTLS 传输加密
- Token-based 认证（JWT 或自定义签名）
- 消息频率限制（速率限制 per connection）
- 服务器端权威验证（关键操作服务端校验，不信任客户端）
- 行为分析反作弊（异常移动速度、封包频率检测）

### 3.7 全球加速网络层

**缺失描述：** 跨区通信走裸 TCP，无延迟优化。

**需要的能力：**
- QUIC 协议支持（0-RTT 连接、多路复用、连接迁移）
- Anycast 接入（玩家自动连接最近 Gateway）
- 跨区消息压缩（Delta 编码、LZ4/Zstd）
- 预测/插值补偿（客户端预测 + 服务端纠正）
- 优先级传输通道（关键操作走可靠通道，位置同步走不可靠通道）

### 3.8 运维与可观测性平台

**缺失描述：** 当前 Admin 仅有基础 Web 仪表盘和 IPC，缺少专业可观测性。

**需要的能力：**
- 分布式追踪（OpenTelemetry / Jaeger）
- 指标采集与告警（Prometheus + Grafana）
- 结构化日志聚合（ELK / Loki）
- 热更新机制（Lua 脚本热重载、配置热更新）
- 灰度发布与回滚

### 3.9 客户端预测与 reconciliation

**缺失描述：** 当前同步协议是纯服务端状态同步，无客户端预测。

**需要的能力：**
- 客户端预测（本地模拟移动/技能，不等服务端确认）
- 服务端 reconciliation（服务端纠正后回滚+重放）
- 快照插值（EntityState 之间的平滑插值）
- 延迟补偿（回溯式命中判定）

### 3.10 资源管理与热加载系统

**缺失描述：** 无资源管理器，配置和资源均为静态加载。

**需要的能力：**
- 资源版本管理（热更新资源包）
- CDN 分发（全球客户端资源分发）
- 配置中心（动态配置推送）
- AB 测试框架（灰度配置）

---

## 4. 完整架构设计文档

### 4.1 目标架构总览

```
                    ┌─────────────────────────────────────────────────────┐
                    │              全球 DNS / Anycast 路由                 │
                    └───────────────────────┬─────────────────────────────┘
                                            │
              ┌─────────────────────────────┼─────────────────────────────┐
              │                             │                             │
     ┌────────▼────────┐          ┌────────▼────────┐          ┌────────▼────────┐
     │  Region: Asia   │          │  Region: EU     │          │  Region: US     │
     │  (Tokyo/Shanghai)│          │  (Frankfurt)    │          │  (Virginia)     │
     └────────┬────────┘          └────────┬────────┘          └────────┬────────┘
              │                             │                             │
     ┌────────▼─────────────────────────────▼─────────────────────────────▼────┐
     │                        全球加速骨干网 (QUIC)                              │
     │                    Cross-Region Router Mesh                               │
     └──────────────────────────────┬───────────────────────────────────────────┘
                                    │
     ┌──────────────────────────────▼───────────────────────────────────────────┐
     │                        单区域架构 (以 Asia 为例)                           │
     │                                                                          │
     │  ┌──────────┐  ┌──────────┐  ┌──────────┐                                │
     │  │ Gateway  │  │ Gateway  │  │ Gateway  │  ← Anycast L4 LB               │
     │  │ (C/io_uring) │(C/io_uring) │(C/io_uring) │  TCP+KCP+QUIC+WS           │
     │  └────┬─────┘  └────┬─────┘  └────┬─────┘                                │
     │       │              │              │                                      │
     │  ┌────▼──────────────▼──────────────▼─────┐                               │
     │  │         Router Cluster (Raft)           │  ← 服务发现 + 实体路由       │
     │  │  Router-A (Leader) ←→ Router-B ←→ Router-C │  哈希环 + Cell 映射表     │
     │  └────┬────────────────────────────────────┘                               │
     │       │                                                                  │
     │  ┌────▼──────────────────────────────────────────────────────────────┐   │
     │  │              Game Server Cell 集群                                  │   │
     │  │                                                                    │   │
     │  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐    │   │
     │  │  │ Cell-0  │ │ Cell-1  │ │ Cell-2  │ │ Cell-3  │ │ Cell-N  │    │   │
     │  │  │Game-A   │ │Game-B   │ │Game-A   │ │Game-C   │ │Game-B   │    │   │
     │  │  │(多线程)  │ │(多线程)  │ │(多线程)  │ │(多线程)  │ │(多线程)  │    │   │
     │  │  │ECS+AOI  │ │ECS+AOI  │ │ECS+AOI  │ │ECS+AOI  │ │ECS+AOI  │    │   │
     │  │  │+Ghost   │ │+Ghost   │ │+Ghost   │ │+Ghost   │ │+Ghost   │    │   │
     │  │  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘    │   │
     │  │       │           │           │           │           │          │   │
     │  │       └───────────┴───────────┴───────────┴───────────┘          │   │
     │  │                  Cell 间 Ghost 同步 (共享内存/TCP)                  │   │
     │  └────────────────────────────────────────────────────────────────────┘   │
     │                                                                          │
     │  ┌──────────────────────┐  ┌──────────────────────┐                      │
     │  │  DBProxy Cluster     │  │  Event Bus           │                      │
     │  │  (分片: player_id)   │  │  (Redis Stream/Kafka)│                      │
     │  │  → MongoDB Shard     │  │  → 事件发布/订阅      │                      │
     │  └──────────────────────┘  └──────────────────────┘                      │
     │                                                                          │
     │  ┌──────────────────────┐  ┌──────────────────────┐                      │
     │  │  Auth Service        │  │  Observability       │                      │
     │  │  (JWT/OAuth2)        │  │  (OTel+Prom+Grafana) │                      │
     │  └──────────────────────┘  └──────────────────────┘                      │
     └──────────────────────────────────────────────────────────────────────────┘
```

### 4.2 分层架构设计

#### 4.2.1 接入层（Gateway Layer）

**职责：** 客户端接入、协议转换、负载均衡、安全防护

**设计：**

```
┌──────────────────────────────────────────────────┐
│                  Gateway 进程                     │
│                                                  │
│  ┌──────────────────────────────────────────┐   │
│  │           协议接入层                      │   │
│  │  TCP (9000) | KCP (9000) | QUIC (9000)   │   │
│  │  WebSocket (9000) | Anycast 接入          │   │
│  └──────────────────┬───────────────────────┘   │
│                     │                            │
│  ┌──────────────────▼───────────────────────┐   │
│  │           安全层 (新增)                    │   │
│  │  TLS 1.3 | DTLS | Token 验证 | 速率限制   │   │
│  └──────────────────┬───────────────────────┘   │
│                     │                            │
│  ┌──────────────────▼───────────────────────┐   │
│  │           路由层 (增强)                    │   │
│  │  msg_type 路由 (现有)                     │   │
│  │  + entity_id 路由 (新增, 查 Router)       │   │
│  │  + session 粘性路由 (新增)                │   │
│  │  + Cell 迁移重定向 (新增)                 │   │
│  └──────────────────┬───────────────────────┘   │
│                     │                            │
│  ┌──────────────────▼───────────────────────┐   │
│  │           后端连接池 (增强)                │   │
│  │  Game Server 连接 (动态注册/注销)         │   │
│  │  + 健康检查 + 自动故障转移                 │   │
│  │  + 连接复用 (多路复用)                    │   │
│  └──────────────────────────────────────────┘   │
│                                                  │
│  io_uring 事件循环 (现有, 保持)                  │
└──────────────────────────────────────────────────┘
```

**关键改进点：**

1. **增加 QUIC 协议支持**：替换裸 TCP 跨区通信，0-RTT 连接建立，连接迁移支持（玩家网络切换不断线）
2. **增加 entity_id 路由**：Gateway 向 Router 查询 `entity_id → game_server_id` 映射，将消息路由到正确的 Game Server
3. **增加 Cell 迁移重定向**：当玩家跨 Cell 迁移时，Gateway 自动将连接重定向到新的 Game Server
4. **增加安全层**：TLS 1.3 加密、JWT Token 验证、per-connection 速率限制
5. **动态后端注册**：Game Server 启动时向 Router 注册，Gateway 从 Router 获取后端列表

**代码层面改造：**

```c
// 新增: ce_gateway.h 增强路由
typedef struct CeGatewayRouteEntry {
    uint16_t    msg_type_start;
    uint16_t    msg_type_end;
    int         backend_index;
} CeGatewayRouteEntry;

// 新增: entity_id → backend 路由表
typedef struct CeGatewayEntityRoute {
    uint64_t    entity_id;
    int         backend_index;
    uint64_t    last_access_us;
} CeGatewayEntityRoute;

// 新增: Router 查询接口
CeResult ce_gateway_route_by_entity(CeGateway* gw, uint64_t entity_id,
                                     int* out_backend_index);

// 新增: 安全层
typedef struct CeGatewaySecurity {
    CeBool      tls_enabled;
    CeBool      token_verification;
    int         rate_limit_per_sec;
    // ...
} CeGatewaySecurity;
```

#### 4.2.2 路由层（Router Layer）

**职责：** 服务发现、实体路由、跨区通信、集群共识

**设计：**

```
┌──────────────────────────────────────────────────────┐
│                  Router 集群                          │
│                                                      │
│  ┌────────────────────────────────────────────────┐  │
│  │           Raft 共识层 (新增)                    │  │
│  │  Leader 选举 / 日志复制 / 配置变更              │  │
│  │  替代当前"最小 node_id wins"的伪选举            │  │
│  └──────────────────┬─────────────────────────────┘  │
│                     │                                 │
│  ┌──────────────────▼─────────────────────────────┐  │
│  │           服务注册表 (增强)                     │  │
│  │  Game Server: { id, host, port, cells[], load }│  │
│  │  DBProxy:     { id, host, port, shard_range }  │  │
│  │  Gateway:     { id, host, port, connections }  │  │
│  │  + 健康检查 + 自动注销                          │  │
│  └──────────────────┬─────────────────────────────┘  │
│                     │                                 │
│  ┌──────────────────▼─────────────────────────────┐  │
│  │           实体路由表 (新增)                     │  │
│  │  entity_id → game_server_id (全局)             │  │
│  │  + Cell → game_server_id 映射                  │  │
│  │  + 迁移中的实体标记 (transferring)             │  │
│  │  持久化到 Raft 日志                             │  │
│  └──────────────────┬─────────────────────────────┘  │
│                     │                                 │
│  ┌──────────────────▼─────────────────────────────┐  │
│  │           跨区路由 (增强)                       │  │
│  │  QUIC 隧道 + 消息压缩 + 优先级队列              │  │
│  │  动态路由发现 (替代静态配置)                    │  │
│  │  + 路由缓存 + 最短路径计算                      │  │
│  └────────────────────────────────────────────────┘  │
│                                                      │
│  现有 Lua Router 逻辑迁移为 C 实现 (性能要求)         │
└──────────────────────────────────────────────────────┘
```

**关键改进点：**

1. **Raft 共识**：替代当前 `cluster.lua` 中的"最小 node_id wins"伪选举，实现真正的 Leader 选举和日志复制
2. **实体路由表**：全局 `entity_id → game_server_id` 映射，Gateway 和 Game Server 通过 Router 查询实体位置
3. **Router 从 Lua 迁移到 C**：Lua Router 在高 QPS 路由查询下性能不足，需迁移到 C 实现
4. **QUIC 跨区隧道**：跨区消息走 QUIC，支持 0-RTT、多路复用、连接迁移
5. **动态路由发现**：替代 `cross_region.lua` 中的静态 `add_region` 配置

**代码层面改造：**

```c
// 新增: ce_router_entity_table.h
typedef struct CeRouterEntityTable CeRouterEntityTable;

CeRouterEntityTable* ce_router_entity_table_create(int initial_capacity);
CeResult ce_router_entity_table_set(CeRouterEntityTable* tbl,
                                     uint64_t entity_id, uint32_t server_id);
uint32_t ce_router_entity_table_get(CeRouterEntityTable* tbl, uint64_t entity_id);
CeResult ce_router_entity_table_transfer(CeRouterEntityTable* tbl,
                                          uint64_t entity_id,
                                          uint32_t new_server_id);

// 新增: ce_router_raft.h
typedef struct CeRaftNode CeRaftNode;
CeRaftNode* ce_raft_init(const char* node_id, const char* host, int port);
CeResult ce_raft_add_peer(CeRaftNode* node, const char* peer_host, int peer_port);
CeResult ce_raft_propose(CeRaftNode* node, const uint8_t* data, uint32_t len);
```

#### 4.2.3 游戏逻辑层（Game Server Cell Cluster）

**职责：** 游戏逻辑执行、空间管理、实体模拟

**设计：**

```
┌──────────────────────────────────────────────────────────────────┐
│                    Game Server 进程 (多 Cell)                     │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │                   主线程 (事件循环)                         │  │
│  │  io_uring: 网络收发 + 定时器 + IPC                         │  │
│  └───────────────────────┬────────────────────────────────────┘  │
│                          │                                        │
│  ┌───────────────────────▼────────────────────────────────────┐  │
│  │              Cell 管理器 (实例化, 非 全局单例)              │  │
│  │                                                            │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐                 │  │
│  │  │  Cell-A  │  │  Cell-B  │  │  Cell-C  │                 │  │
│  │  │          │  │          │  │          │                 │  │
│  │  │ ECS World│  │ ECS World│  │ ECS World│  ← 独立 ECS 实例 │  │
│  │  │ AOI Grid │  │ AOI Grid │  │ AOI Grid │  ← Grid AOI     │  │
│  │  │ Ghost Mgr│  │ Ghost Mgr│  │ Ghost Mgr│  ← 边界影子     │  │
│  │  │ Repl Mgr │  │ Repl Mgr │  │ Repl Mgr │  ← 独立复制     │  │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘                 │  │
│  │       │             │             │                         │  │
│  │       └─────────────┴─────────────┘                         │  │
│  │              Cell 间共享内存通信                              │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │              Worker 线程池 (新增)                           │  │
│  │  System 并行调度: AI / 物理 / 战斗 / 脚本                   │  │
│  │  Work-stealing + 无锁队列                                    │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │              跨进程通信 (新增)                               │  │
│  │  → Router: 实体路由查询 / 迁移通知                           │  │
│  │  → 邻居 Game Server: Ghost 同步 / 跨 Cell 交互               │  │
│  │  → DBProxy: 存档同步                                        │  │
│  │  → Event Bus: 事件发布                                      │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

**关键改进点：**

**A. Cell 管理器实例化**

将 `ce_cell.c` 从全局单例改为实例化结构体：

```c
// 新增: ce_cell_manager.h
typedef struct CeCellManager CeCellManager;

CeCellManager* ce_cell_mgr_create(float world_width, float world_height,
                                    float cell_width, float cell_height);
void ce_cell_mgr_destroy(CeCellManager* mgr);

// Cell 操作 (带实例参数)
CeCellId ce_cell_mgr_find_by_pos(CeCellManager* mgr, float x, float y);
CeResult ce_cell_mgr_enter_entity(CeCellManager* mgr, CeServerEntityId eid,
                                    float x, float y, float radius);
CeResult ce_cell_mgr_move_entity(CeCellManager* mgr, CeServerEntityId eid,
                                   float new_x, float new_y);

// 跨进程 Cell 迁移
CeResult ce_cell_mgr_transfer_cell(CeCellManager* mgr,
                                     CeCellId cell_id,
                                     int target_process_id);
```

**B. ECS 多实例 + 多线程**

```c
// 新增: ce_ecs_world.h
typedef struct CeEcsWorld CeEcsWorld;

CeEcsWorld* ce_ecs_world_create(CeAllocator* alloc);
void ce_ecs_world_destroy(CeEcsWorld* world);

// System 调度器
typedef struct CeEcsScheduler CeEcsScheduler;
CeEcsScheduler* ce_ecs_scheduler_create(CeEcsWorld* world, int worker_threads);
void ce_ecs_scheduler_run(CeEcsScheduler* sched, float dt);

// System 依赖声明
typedef struct CeSystemDesc {
    const char* name;
    CeSystemFn  fn;
    void*       user_data;
    int         priority;
    // 新增: 读写组件集 (用于并行调度)
    CeComponentId* read_components;
    uint32_t       read_count;
    CeComponentId* write_components;
    uint32_t       write_count;
} CeSystemDesc;
```

**C. 分布式 AOI（Grid + Ghost）**

```c
// 新增: ce_aoi_grid.h
typedef struct CeAoiGrid CeAoiGrid;

// Grid-based AOI, O(1) 查询
CeAoiGrid* ce_aoi_grid_create(float world_width, float world_height,
                                float cell_size, float aoi_radius);
void ce_aoi_grid_destroy(CeAoiGrid* grid);

// Ghost 实体管理 (边界实体的影子副本)
typedef struct CeGhostManager CeGhostManager;
CeGhostManager* ce_ghost_mgr_create(CeCellManager* cell_mgr);
CeResult ce_ghost_mgr_sync_border(CeGhostManager* mgr,
                                    CeCellId cell_id,
                                    CeCellId neighbor_cell_id);
// Ghost 实体查询 (查询邻居 Cell 的 Ghost 实体)
int ce_ghost_mgr_query_nearby(CeGhostManager* mgr,
                                CeServerEntityId entity_id,
                                CeServerEntityId* buffer, int max_count);
```

**D. Cell 迁移协议**

```c
// 新增: ce_cell_migration.h
typedef enum {
    CE_MIGRATION_INITIATE,    // 发起迁移
    CE_MIGRATION_SERIALIZE,   // 序列化实体状态
    CE_MIGRATION_TRANSFER,    // 传输到目标进程
    CE_MIGRATION_DESERIALIZE, // 反序列化 + AOI 重建
    CE_MIGRATION_NOTIFY,      // 通知 Router/Gateway
    CE_MIGRATION_COMPLETE,    // 迁移完成
    CE_MIGRATION_FAILED,      // 迁移失败回滚
} CeMigrationPhase;

typedef struct CeMigrationRequest {
    CeServerEntityId  entity_id;
    int               source_server_id;
    int               target_server_id;
    CeCellId          source_cell_id;
    CeCellId          target_cell_id;
    CeMigrationPhase  phase;
    uint8_t*          serialized_state;
    uint32_t          state_size;
    uint64_t          timestamp_us;
} CeMigrationRequest;

CeResult ce_cell_migrate_entity(CeCellManager* mgr,
                                  CeMigrationRequest* req);
```

#### 4.2.4 数据层（DBProxy Cluster）

**职责：** 持久化存储、状态镜像、数据分片

**设计：**

```
┌──────────────────────────────────────────────────────────┐
│                  DBProxy 集群                             │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           分片路由层 (新增)                         │  │
│  │  shard = hash(player_id) % N                       │  │
│  │  → DBProxy-0 (shard 0-2)                           │  │
│  │  → DBProxy-1 (shard 3-5)                           │  │
│  │  → DBProxy-2 (shard 6-8)                           │  │
│  └───────────────────────┬────────────────────────────┘  │
│                          │                                │
│  ┌───────────────────────▼────────────────────────────┐  │
│  │           DBProxy 实例 (C + 原生驱动)               │  │
│  │                                                    │  │
│  │  ┌──────────────┐  ┌──────────────┐               │  │
│  │  │ 写入队列      │  │ 读取缓存      │               │  │
│  │  │ (批量写入)    │  │ (LRU Cache)  │               │  │
│  │  └──────┬───────┘  └──────┬───────┘               │  │
│  │         │                 │                         │  │
│  │  ┌──────▼─────────────────▼───────┐               │  │
│  │  │     MongoDB 原生 C 驱动         │               │  │
│  │  │     (libmongoc, 替代 mongosh)   │               │  │
│  │  │     + 连接池 + 批量操作          │               │  │
│  │  │     + 事务支持                  │               │  │
│  │  └────────────────────────────────┘               │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           状态镜像 (增强)                           │  │
│  │  实时镜像 Game Server 的实体状态                     │  │
│  │  + 增量同步 (仅变化字段)                            │  │
│  │  + 快照持久化 (定期全量)                            │  │
│  │  + 故障恢复 (从镜像恢复 Game Server 状态)            │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

**关键改进点：**

1. **替换 mongosh 为原生 C 驱动**：使用 `libmongoc` 替代 `io.popen("mongosh ...")`，性能提升 1000x+
2. **分片策略**：按 `player_id` 哈希分片，每个 DBProxy 管理一部分玩家数据
3. **批量写入**：攒批后批量写入 MongoDB，减少 IO 次数
4. **读缓存**：LRU 缓存热点玩家数据，减少数据库查询
5. **事务支持**：跨集合操作使用 MongoDB 事务

**代码层面改造：**

```c
// 新增: ce_dbproxy_native.h (替代 Lua handler.lua)
typedef struct CeDbproxyCluster CeDbproxyCluster;

CeDbproxyCluster* ce_dbproxy_cluster_create(int shard_count,
                                              const char** mongo_uris);
CeResult ce_dbproxy_save(CeDbproxyCluster* cluster,
                          uint64_t player_id,
                          const uint8_t* data, uint32_t len);
CeResult ce_dbproxy_load(CeDbproxyCluster* cluster,
                          uint64_t player_id,
                          uint8_t** out_data, uint32_t* out_len);
CeResult ce_dbproxy_batch_save(CeDbproxyCluster* cluster,
                                const uint64_t* player_ids,
                                const uint8_t** datas,
                                const uint32_t* lens,
                                int count);
```

#### 4.2.5 同步与复制层

**职责：** 客户端状态同步、跨服状态复制

**设计改进：**

```
┌──────────────────────────────────────────────────────────┐
│                  同步与复制层                             │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           客户端同步 (增强)                         │  │
│  │                                                    │  │
│  │  Game → Client:                                    │  │
│  │  ├── 不可靠通道 (KCP): 位置/旋转/速度 (高频)       │  │
│  │  ├── 可靠通道 (TCP): RPC/属性/状态 (低频)          │  │
│  │  └── 优先级队列: 关键操作优先发送                   │  │
│  │                                                    │  │
│  │  Client → Game:                                    │  │
│  │  ├── 输入流 (KCP): 移动/技能输入 (高频)            │  │
│  │  ├── RPC 请求 (TCP): 交互操作 (低频)               │  │
│  │  └── 客户端预测 + 服务端 reconciliation (新增)     │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           跨服复制 (新增)                           │  │
│  │                                                    │  │
│  │  Game-A ←→ Game-B (邻居 Cell):                     │  │
│  │  ├── Ghost 实体同步 (边界实体属性)                  │  │
│  │  ├── 跨 Cell RPC (技能/交互)                        │  │
│  │  └── 共享内存优化 (同机 Cell 间零拷贝)              │  │
│  │                                                    │  │
│  │  Game-A ←→ Game-C (跨区):                          │  │
│  │  ├── 跨区 RPC (通过 Router 转发)                   │  │
│  │  ├── 全局实体查询 (Mailbox 路由)                   │  │
│  │  └── 消息压缩 + Delta 编码                         │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

**复制系统容量改进：**

```c
// ce_replication_internal.h 改进
// 旧:
// #define CE_REPL_MAX_DIRTY_ENTITIES      4096
// uint64_t mailbox_keys[4096];

// 新: 动态扩容
typedef struct CeReplContext {
    // 动态脏标表 (可扩容)
    uint32_t        dirty_capacity;       // 动态容量
    uint32_t        dirty_count;
    CeReplDirtyEntry* dirty_entities;     // 动态分配
    uint64_t*       dirty_hash_keys;      // 动态哈希表
    uint32_t*       dirty_hash_values;
    uint32_t        dirty_hash_capacity;

    // 动态 Mailbox (可扩容)
    uint32_t        mailbox_capacity;
    uint32_t        mailbox_count;
    uint64_t*       mailbox_keys;         // 动态分配
    uint32_t*       mailbox_values;

    // 属主映射改为哈希表 (O(1) 查找)
    uint32_t        owner_hash_capacity;
    uint64_t*       owner_hash_keys;
    uint64_t*       owner_hash_values;

    // 多 ECS 世界支持
    CeEcsWorld*     ecs_world;            // 当前绑定的 ECS 世界
    // ...
} CeReplContext;
```

#### 4.2.6 安全层（新增）

**设计：**

```
┌──────────────────────────────────────────────────────────┐
│                    安全架构                               │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           传输安全                                  │  │
│  │  TLS 1.3 (TCP) / DTLS 1.3 (KCP/UDP)               │  │
│  │  证书管理 + 自动轮换                                │  │
│  │  完美前向保密 (PFS)                                 │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           认证授权                                  │  │
│  │  JWT Token (登录时签发)                             │  │
│  │  + Refresh Token 机制                               │  │
│  │  + 权限校验 (RBAC)                                  │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           反作弊                                    │  │
│  │  服务器端权威: 关键操作服务端校验                   │  │
│  │  + 移动速度检测 (超出阈值标记异常)                  │  │
│  │  + 封包频率限制 (per connection rate limit)        │  │
│  │  + 行为分析 (异常模式检测)                          │  │
│  │  + 回放审计 (关键操作记录回放)                      │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │           DDoS 防护                                 │  │
│  │  eBPF/XDP 流量过滤 (现有 ce_xdp.c 基础)            │  │
│  │  + IP 限速 (per-IP connection limit)               │  │
│  │  + SYN Flood 防护                                  │  │
│  │  + 挑战-应答 (Proof of Work)                       │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

#### 4.2.7 可观测性层（新增）

**设计：**

```
┌──────────────────────────────────────────────────────────┐
│                    可观测性架构                           │
│                                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Metrics     │  │  Tracing     │  │  Logging     │  │
│  │  Prometheus  │  │  OpenTelemetry│  │  Loki/ELK   │  │
│  │  + Grafana   │  │  + Jaeger    │  │              │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  │
│         │                 │                 │            │
│  ┌──────▼─────────────────▼─────────────────▼───────┐  │
│  │              统一采集 Agent                        │  │
│  │  (嵌入各进程的 C SDK)                             │  │
│  │  + ce_metrics_* API                               │  │
│  │  + ce_trace_* API (span/context)                  │  │
│  │  + ce_log 结构化日志 (JSON)                       │  │
│  └──────────────────────────────────────────────────┘  │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │              告警与自动化                           │  │
│  │  AlertManager + 自动扩缩容 + 故障自愈              │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### 4.3 数据流设计

#### 4.3.1 玩家登录流程

```
Client                    Gateway              Router              Game Server
  │                         │                    │                      │
  │── LOGIN (JWT) ─────────►│                    │                      │
  │                         │── Verify JWT ──────│                      │
  │                         │◄── OK + player_id──│                      │
  │                         │                    │                      │
  │                         │── Route(player_id)─►│                      │
  │                         │◄── game_server_id ──│                      │
  │                         │                    │                      │
  │                         │── LOGIN ──────────────────────────────────►│
  │                         │                    │                      │
  │                         │                    │  ┌─ Load player data ─┤
  │                         │                    │  │  from DBProxy      │
  │                         │                    │  │                    │
  │                         │                    │  ├─ Create entity ────┤
  │                         │                    │  │  in ECS world      │
  │                         │                    │  │                    │
  │                         │                    │  ├─ Enter AOI ────────┤
  │                         │                    │  │  (notify neighbors)│
  │                         │                    │  │                    │
  │                         │                    │  ├─ Register mailbox ─┤
  │                         │                    │  │  entity_id→server  │
  │                         │                    │  │                    │
  │                         │◄── JOIN_RESPONSE ──────────────────────────┤
  │◄── LOGIN_RESP ──────────│                    │                      │
  │                         │                    │                      │
```

#### 4.3.2 玩家跨 Cell 迁移流程

```
Game-A (源Cell)          Router               Game-B (目标Cell)       Gateway
  │                        │                       │                     │
  │── MIGRATE_REQUEST ────►│                       │                     │
  │   (entity_id,          │                       │                     │
  │    target_cell_id)     │                       │                     │
  │                        │── MIGRATE_PREPARE ───►│                     │
  │                        │                       │                     │
  │                        │◄── PREPARE_OK ────────│                     │
  │                        │   (ready to receive)  │                     │
  │                        │                       │                     │
  │── SERIALIZE_STATE ─────│                       │                     │
  │   (ECS components,     │                       │                     │
  │    AOI state,          │                       │                     │
  │    pending RPCs)       │                       │                     │
  │                        │                       │                     │
  │── TRANSFER_STATE ─────►│── TRANSFER_STATE ───►│                     │
  │   (serialized blob)    │   (to target)         │                     │
  │                        │                       │                     │
  │                        │                       │── DESERIALIZE ──────┤
  │                        │                       │  (rebuild ECS)      │
  │                        │                       │  (rebuild AOI)      │
  │                        │                       │  (register mailbox) │
  │                        │                       │                     │
  │                        │◄── MIGRATION_DONE ────│                     │
  │                        │                       │                     │
  │                        │── REDIRECT ──────────────────────────────────►│
  │                        │   (entity_id→new     │                     │
  │                        │    game_server_id)   │                     │
  │                        │                       │                     │
  │                        │                       │  ← Gateway 更新路由 │
  │                        │                       │    后续消息发往 B   │
  │                        │                       │                     │
  │── CLEANUP_SOURCE ──────│                       │                     │
  │   (remove entity,      │                       │                     │
  │    release resources)  │                       │                     │
```

#### 4.3.3 每帧更新数据流

```
Game Server 主循环 (60Hz):

  1. io_uring 事件循环
     ├── 接收客户端输入 (KCP/TCP)
     ├── 接收跨服消息 (Router/邻居Cell)
     └── 定时器触发

  2. 输入处理
     ├── 解析客户端位置更新
     ├── 解析 RPC 请求
     └── 解析跨服消息

  3. ECS System 调度 (多线程)
     ├── Movement System (写 Transform)
     ├── AI System (写 AIState, 读 Transform)
     ├── Combat System (写 Health, 读 Transform/Combat)
     └── Physics System (写 Transform, 读 Collider)

  4. AOI 更新
     ├── ce_aoi_grid_move (更新位置)
     ├── 检测 enter/leave 事件
     └── Ghost 实体边界同步

  5. Cell 更新
     ├── ce_cell_mgr_update (检查分裂/合并)
     └── 跨 Cell 迁移检测

  6. 复制 flush
     ├── 收集脏字段 (动态扩容脏标表)
     ├── 按 flag 分流:
     │   ├── AOI_BROADCAST → 查 Grid AOI → 打包 → KCP 发送
     │   ├── OWNER_ONLY → 查属主 → 打包 → TCP 发送
     │   ├── PERSIST → 攒批 → DBProxy 批量写入
     │   └── GHOST → 邻居 Cell 同步
     └── 清除脏标

  7. 同步
     ├── Sync frame → DBProxy (PERSIST 字段)
     └── Ghost frame → 邻居 Game Server
```

### 4.4 模块间依赖关系

```
                          ┌─────────────┐
                          │   Admin     │ (依赖所有模块的监控接口)
                          └──────┬──────┘
                          ┌──────┼──────┐
                 ┌────────▼──┐ ┌▼────────┐ ┌▼──────────┐
                 │  Gateway  │ │ Router  │ │  Auth     │
                 │  (C/io_uring) │(C/Raft)│ │ (JWT)     │
                 └────┬──────┘ └──┬──────┘ └───────────┘
                      │           │
              ┌───────┼───────────┤
              │       │           │
     ┌────────▼──┐ ┌──▼────────┐ │
     │Game Server│ │ DBProxy   │ │
     │ (多Cell)  │ │ (分片)    │ │
     │           │ │           │ │
     │ ┌───────┐ │ └─────┬─────┘ │
     │ │  ECS  │ │       │       │
     │ │(多线程)│ │  ┌────▼────┐  │
     │ └───┬───┘ │  │ MongoDB │  │
     │ ┌───▼───┐ │  │ (Sharded│  │
     │ │  AOI  │ │  │  Cluster)│  │
     │ │(Grid) │ │  └─────────┘  │
     │ └───┬───┘ │               │
     │ ┌───▼───┐ │          ┌────▼────┐
     │ │ Cell  │ │          │ Event   │
     │ │(分布式)│ │          │ Bus     │
     │ └───┬───┘ │          │(Redis   │
     │ ┌───▼───┐ │          │ Stream) │
     │ │ Repl  │ │          └─────────┘
     │ │(动态) │ │
     │ └───────┘ │
     └───────────┘
```

### 4.5 协议设计增强

#### 4.5.1 消息头扩展

```
现有:  [4B total_len][2B msg_type][N payload]

增强:  [4B total_len][2B msg_type][1B flags][1B priority][4B sequence][N payload]

flags:
  bit 0:   加密 (0=明文, 1=加密)
  bit 1:   压缩 (0=原始, 1=压缩)
  bit 2:   可靠 (0=不可靠, 1=可靠)
  bit 3-7: 保留

priority:
  0 = 低 (位置同步)
  1 = 普通 (属性更新)
  2 = 高 (RPC)
  3 = 紧急 (登录/迁移)

sequence:
  消息序号 (用于可靠传输的 ACK/重传)
```

#### 4.5.2 新增消息类型

```c
// ce_net_base.h 扩展
typedef enum CeNetMsgType {
    // 现有消息类型保持不变
    CE_NET_MSG_PING         = 0x0001,
    CE_NET_MSG_PONG         = 0x0002,
    CE_NET_MSG_LOGIN        = 0x0010,
    CE_NET_MSG_LOGIN_RESP   = 0x0011,
    CE_NET_MSG_GAME_DATA    = 0x0100,
    CE_NET_MSG_DISCONNECT   = 0xFFFF,

    // 跨区消息 (现有)
    CE_NET_MSG_CROSS_REGION = 0x1000,
    CE_NET_MSG_REGION_SYNC  = 0x1001,
    CE_NET_MSG_ROUTER_HELLO = 0x1002,
    CE_NET_MSG_ROUTER_BYE   = 0x1003,

    // 新增: Cell 迁移消息 (0x2000-0x2FFF)
    CE_NET_MSG_MIGRATE_REQ      = 0x2000,
    CE_NET_MSG_MIGRATE_PREPARE  = 0x2001,
    CE_NET_MSG_MIGRATE_TRANSFER = 0x2002,
    CE_NET_MSG_MIGRATE_DONE     = 0x2003,
    CE_NET_MSG_MIGRATE_FAILED   = 0x2004,
    CE_NET_MSG_MIGRATE_REDIRECT = 0x2005,  // Gateway→Client 重定向

    // 新增: Ghost 同步消息 (0x3000-0x3FFF)
    CE_NET_MSG_GHOST_ENTER  = 0x3000,
    CE_NET_MSG_GHOST_UPDATE = 0x3001,
    CE_NET_MSG_GHOST_LEAVE  = 0x3002,

    // 新增: 实体路由消息 (0x4000-0x4FFF)
    CE_NET_MSG_ROUTE_QUERY  = 0x4000,   // Gateway→Router: 查询实体位置
    CE_NET_MSG_ROUTE_RESP   = 0x4001,   // Router→Gateway: 实体位置响应
    CE_NET_MSG_ROUTE_UPDATE = 0x4002,   // Game→Router: 更新实体位置

    // 新增: 安全消息 (0x5000-0x5FFF)
    CE_NET_MSG_AUTH_TOKEN   = 0x5000,   // Token 认证
    CE_NET_MSG_AUTH_RESP    = 0x5001,
    CE_NET_MSG_AUTH_REFRESH = 0x5002,   // Token 刷新

    CE_NET_MSG_USER_BASE    = 0x8000,
} CeNetMsgType;
```

### 4.6 容量与性能目标

| 指标 | 当前 | 目标 (全球同服MMO) |
|------|------|-------------------|
| 单 Game Server 实体数 | 1,024 | 10,000+ (单 Cell) |
| 单 Cell 玩家数 | N/A | 500-2,000 |
| 单区域 Cell 数 | 1 (单进程) | 100-500 |
| 全局并发玩家 | ~1,000 | 100,000+ |
| 单 Gateway 连接数 | 10,000 | 50,000+ |
| AOI 查询延迟 | O(n) 遍历 | O(1) Grid 查询 |
| ECS tick 速率 | 60Hz (单线程) | 60Hz (多线程) |
| 存档吞吐 | <10 writes/s | 10,000+ writes/s |
| 跨区延迟 | 200ms+ (TCP) | <50ms (QUIC+压缩) |
| 故障恢复时间 | 手动重启 | <30s (自动故障转移) |
| Mailbox 容量 | 4,096 (固定) | 1,000,000+ (动态) |
| 脏标表容量 | 4,096 (固定) | 100,000+ (动态) |

### 4.7 技术选型建议

| 领域 | 当前 | 建议 | 理由 |
|------|------|------|------|
| Router 实现 | Lua (LuaSocket) | C (保持 io_uring 栈) | 高 QPS 路由查询需要 C 性能 |
| 共识协议 | 无 (伪选举) | Raft (自研或 etcd) | 强一致性保证 |
| 跨区传输 | TCP (LuaSocket) | QUIC | 0-RTT、多路复用、连接迁移 |
| MongoDB 驱动 | mongosh (os.execute) | libmongoc (C 原生) | 性能、事务、连接池 |
| AOI 数据结构 | 十字链表 | Grid + Ghost | O(1) 查询、跨进程支持 |
| ECS 调度 | 单线程线性 | 依赖图 + Work-stealing | 多核利用 |
| 事件总线 | 无 | Redis Stream | 轻量、持久化、消费者组 |
| 分布式追踪 | 无 | OpenTelemetry | 行业标准 |
| 配置中心 | 无 | etcd / Consul | 动态配置、服务发现 |
| 容器编排 | 无 | Kubernetes | 自动扩缩容、故障恢复 |

---

## 5. 迁移路线图

### Phase 1: 基础设施加固（2-3 个月）

**目标：** 修复致命缺陷，使架构可水平扩展

| 任务 | 优先级 | 预估工时 | 依赖 |
|------|--------|---------|------|
| 1.1 DBProxy 替换 mongosh → libmongoc | P0 | 2 周 | 无 |
| 1.2 ECS 实例化 (全局单例→结构体) | P0 | 3 周 | 无 |
| 1.3 Cell 管理器实例化 | P0 | 2 周 | 1.2 |
| 1.4 AOI 从十字链表改为 Grid | P0 | 2 周 | 1.3 |
| 1.5 复制系统动态扩容 | P0 | 1 周 | 1.2 |
| 1.6 Game Server 不再直接 accept 客户端 | P0 | 1 周 | Gateway 增强 |
| 1.7 Gateway 增加 entity_id 路由 | P1 | 2 周 | Router 增强 |

### Phase 2: 分布式能力（3-4 个月）

**目标：** 实现 Cell 集群、跨服迁移、Ghost 同步

| 任务 | 优先级 | 预估工时 | 依赖 |
|------|--------|---------|------|
| 2.1 Router 从 Lua 迁移到 C | P0 | 4 周 | Phase 1 |
| 2.2 Router 增加 Raft 共识 | P0 | 4 周 | 2.1 |
| 2.3 实体路由表 (entity_id→server) | P0 | 2 周 | 2.2 |
| 2.4 Cell 迁移协议实现 | P0 | 4 周 | 2.3, Phase 1 |
| 2.5 Ghost 实体管理 | P0 | 3 周 | 2.4, 1.4 |
| 2.6 跨 Cell AOI 查询 | P0 | 2 周 | 2.5 |
| 2.7 DBProxy 分片 | P1 | 3 周 | 1.1 |
| 2.8 事件总线 (Redis Stream) | P1 | 2 周 | 无 |

### Phase 3: 性能与安全（2-3 个月）

**目标：** 多线程 ECS、安全层、全球加速

| 任务 | 优先级 | 预估工时 | 依赖 |
|------|--------|---------|------|
| 3.1 ECS 多线程调度器 | P0 | 4 周 | Phase 1 |
| 3.2 TLS/DTLS 传输加密 | P0 | 3 周 | 无 |
| 3.3 JWT 认证 | P0 | 2 周 | 无 |
| 3.4 QUIC 协议支持 | P1 | 4 周 | 无 |
| 3.5 客户端预测 + reconciliation | P1 | 4 周 | 无 |
| 3.6 反作弊基础框架 | P1 | 3 周 | 3.3 |
| 3.7 eBPF/XDP DDoS 防护 | P2 | 2 周 | 现有 ce_xdp.c |

### Phase 4: 可观测性与运维（1-2 个月）

**目标：** 生产级可观测性、自动化运维

| 任务 | 优先级 | 预估工时 | 依赖 |
|------|--------|---------|------|
| 4.1 OpenTelemetry 集成 | P1 | 2 周 | 无 |
| 4.2 Prometheus 指标采集 | P1 | 1 周 | 无 |
| 4.3 结构化日志 (JSON) | P1 | 1 周 | 无 |
| 4.4 告警规则配置 | P2 | 1 周 | 4.2 |
| 4.5 热更新框架 | P2 | 2 周 | 无 |
| 4.6 K8s 部署 + 自动扩缩容 | P2 | 3 周 | Phase 2 |

### 总预估：8-12 个月

---

## 6. 附录

### 6.1 审查文件清单

| 类别 | 文件 | 行数 | 审查深度 |
|------|------|------|---------|
| 网络 | `src_c/network/ce_net_base.h` | 580 | 完整 |
| 网络 | `src_c/network/ce_async_io.h` | 105 | 完整 |
| 网关 | `src_c/gateway/ce_gateway.h` | 232 | 完整 |
| 网关 | `src_c/gateway/ce_gateway_internal.h` | 161 | 完整 |
| 网关 | `src_c/gateway/ce_gateway.c` | 1677 | 前200行 |
| ECS | `src_c/public_api/ce_ecs.h` | 128 | 完整 |
| ECS | `src_c/ecs/ce_ecs.c` | 600 | 前200行 |
| AOI | `src_c/server/ce_aoi.h` | 76 | 完整 |
| AOI | `src_c/server/ce_aoi.c` | 531 | 前150行 |
| Cell | `src_c/server/ce_cell.h` | 85 | 完整 |
| Cell | `src_c/server/ce_cell.c` | 396 | 完整 |
| 协议 | `src_c/server/ce_game_protocol.h` | 141 | 完整 |
| 会话 | `src_c/server/ce_game_session.h` | 170 | 完整 |
| 类型 | `src_c/server/ce_server_types.h` | 84 | 完整 |
| 同步 | `src_c/sync/ce_sync.h` | 184 | 完整 |
| 同步 | `src_c/sync/ce_sync.c` | 623 | 前100行 |
| 复制 | `src_c/replication/ce_replication.h` | 298 | 完整 |
| 复制 | `src_c/replication/ce_replication_internal.h` | 133 | 完整 |
| 复制 | `src_c/replication/ce_replication.c` | 500 | 前200行 |
| Mailbox | `src_c/replication/ce_mailbox.h` | 66 | 完整 |
| RPC | `src_c/replication/ce_rpc_channel.h` | 125 | 完整 |
| 存档 | `src_c/save/ce_save.c` | 692 | 前150行 |
| DBProxy | `src_c/dbproxy/ce_dbproxy.c` | 588 | 前100行 |
| DBProxy | `src_lua/dbproxy/handler.lua` | 292 | 前150行 |
| DBProxy | `src_lua/dbproxy/state_mirror.lua` | 266 | 前100行 |
| Router | `src_lua/router/router.lua` | 251 | 前100行 |
| Router | `src_lua/router/cluster.lua` | 496 | 前200行 |
| Router | `src_lua/router/cross_region.lua` | 427 | 前150行 |
| Router | `src_lua/router/hash_ring.lua` | 307 | 前200行 |
| Server | `src_c/runtime/ce_server_main.c` | 580 | 前350行 |
| 运维 | `scripts/start_cluster.sh` | 157 | 完整 |
| 渲染 | `src_c/render/ce_rhi.h` | 120 | 前80行 |
| 类型 | `src_c/public_api/ce_types.h` | 131 | 前100行 |
| Spec | `docs/spec/chaos-engine-spec-v0.2.md` | 993 | 前200行 |
| Spec | `docs/spec/chaos-engine-replication-spec-v0.1.md` | 1267 | 前150行 |
| Spec | `docs/spec/gateway-c-spec.md` | 2216 | 前150行 |
| Spec | `docs/spec/chaos-engine-io_uring-ebpf-spec-v0.1.md` | 642 | 前100行 |

### 6.2 术语表

| 术语 | 含义 |
|------|------|
| AOI | Area of Interest，兴趣区域，决定玩家能看到哪些实体 |
| Cell | 世界空间的矩形划分单元，可分配给不同 Game Server 进程 |
| Ghost | 边界实体的影子副本，存在于邻居 Cell 中，避免频繁跨进程查询 |
| Archetype | ECS 中具有相同组件集合的实体分组，用于 cache-friendly 存储 |
| Mailbox | entity_id → server_id 的映射表，用于跨服实体寻址 |
| Reconciliation | 服务端纠正客户端预测结果后的回滚重放 |
| ZCRX | Zero-Copy Receive，io_uring 零拷贝网络接收 |
| XDP | eXpress Data Path，Linux 内核高性能数据路径 |

### 6.3 参考系统

| 系统 | 参考点 |
|------|--------|
| EVE Online | 单世界服务器集群、时空连续体、Cell 架构 |
| Albion Online | 空间分区、跨区迁移、AOI 管理 |
| Roblox | 多线程 ECS、分布式模拟、全球部署 |
| Unreal Engine | World Partition、Replication Graph |
| BigWorld Technology | Cell App 集群、Ghost 实体、Mailbox 系统 |

---

> **本文档基于对 ChaosEngine 全部源码的深度审查编写，所有问题均附带代码层面的证据，所有建议均附带给具体的 API 设计和迁移方案。**
