# Design: Router 集群 — 服务发现 + 消息路由 + Game 进程按功能模块拆分

## Context

ChaosEngine v0.3 已具备以下能力：

- **Gateway 网络接入层**（`src_lua/gateway/`）：TCP/KCP/WebSocket 多协议接入，连接管理，消息路由（静态配置）。
- **DBProxy 数据持久化**（`src_lua/dbproxy/`）：Game 内存状态实时备份，主备架构，MongoDB 存储。
- **ce_sync 状态同步**（`src_c/sync/`）：Game → DBProxy 单向状态推送，心跳检测，故障切换。
- **ce_save 存档管理**（`src_c/save/`）：定时存档 + 手动存档，存档队列。
- **ce_dbproxy 客户端**（`src_c/dbproxy/`）：二进制协议编解码，连接池，主备切换。
- **Admin IPC**（`src_c/admin_ipc/`）：JSON-RPC 2.0 over Unix Domain Socket，运行时查询。
- **Lua Admin 进程**：Lapis Web 管理后台。

**当前缺失**：

1. **无服务发现**：Gateway 通过静态配置转发消息，无法动态感知后端服务变化。
2. **无服务间路由**：不同 Game 进程之间无法通信，所有逻辑耦合在单一进程中。
3. **无 Game 进程按功能模块拆分基础设施**：缺乏统一的服务注册、消息路由、数据隔离机制。
4. **无跨区域通信**：多区域部署时无法跨区域转发消息。**全球同服多区部署是核心架构需求**，每个大区（亚洲区/欧洲区/美洲区）需独立部署完整集群，大区之间通过 Router 全球网格交互。
5. **网络逻辑重复**：Gateway 和 Router 各自实现 TCP 连接管理、协议编解码、心跳检测，代码重复。

本设计为 v0.4 引入 **Router 集群** + **ce_net_base 共享网络库** + **Game 进程按功能模块拆分** 三大子系统，使 ChaosEngine 从单区域单进程进化为全球同服的分布式架构。

## Goals / Non-Goals

**Goals:**

1. **共享网络库 ce_net_base**：从 Gateway 抽离 TCP 连接管理、二进制协议编解码、心跳检测、连接池为独立 C 库，Gateway 和 Router 共享。
2. **Router 服务注册与发现**：Game 进程启动时向 Router 注册，不同 Game 进程间通过 Router 发现彼此。Gateway 直连 Game，不经过 Router。
3. **一致性哈希消息路由**：按 player_id 一致性哈希将请求路由到目标 Game 进程，支持进程扩缩容时的自动再均衡。
4. **Router 集群**：区域内多 Router 实例组成集群，广播同步路由表，避免单点故障。
5. **跨大区路由（核心能力）**：全球分多个大区（亚洲区/欧洲区/美洲区），每个大区独立部署完整集群。大区之间通过 Router 间 TCP 长连接组成全球 Router 网格，支持跨区消息路由（跨服 PVP、全球聊天、跨区交易）。跨区消息格式包含 source_region + target_region。
6. **Game 进程按功能模块拆分**：定义功能模块类型枚举，以好友功能为示例拆分为独立 Game 进程实例（加载 `friend.lua` 业务脚本），通过 Router 通信。
7. **数据隔离**：每个 Game 进程连接独立 DBProxy 实例，使用独立数据库命名空间。

**Non-Goals:**

- ❌ 不做服务网格（Service Mesh）的 sidecar 模式 —— 当前通过 Router 集中路由。
- ❌ 不做基于 Raft/Paxos 的 Router 集群强一致性 —— 当前使用广播同步（最终一致）。
- ❌ 不做 Game 进程动态扩缩容的自动触发（Auto-Scaling）—— 当前手动扩缩容。
- ❌ 不做跨区域数据同步（如跨区域 DBProxy 数据复制）—— 延后到 v0.5。
- ❌ 不做消息优先级队列和流量控制 —— 延后到后续版本。
- ❌ 不做 Game 进程间的事务协调（Saga/TCC）—— 当前各 Game 进程独立事务。

## Architecture

### 整体架构图（全球多区域拓扑）

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│                        大区: asia-east (亚洲区)                                        │
│                                                                                      │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐                                         │
│  │  Client  │   │  Client  │   │  Client  │   ... (数千客户端)                        │
│  └────┬─────┘   └────┬─────┘   └────┬─────┘                                         │
│       │              │              │                                                 │
│       └──────────────┼──────────────┘                                                 │
│                      │ TCP / KCP / WebSocket                                          │
│                      ▼                                                               │
│  ┌──────────────────────────────────────────────────────────────────────────────┐    │
│  │                         Gateway (Lua)                                         │    │
│  │                         PID: 4001                                             │    │
│  │                                                                              │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                        │    │
│  │  │ 连接管理       │  │ 协议适配      │  │ 消息转发      │                        │    │
│  │  │ (ce_net_base) │  │ TCP/KCP/WS   │  │ (直连 Game)   │                        │    │
│  │  └──────────────┘  └──────────────┘  └──────┬───────┘                        │    │
│  └─────────────────────────────────────────────┼─────────────────────────────────┘    │
│                                                │                                      │
│                         直连 Game（不经过 Router）│                                      │
│                                                ▼                                      │
│  ┌──────────────────────────────────────────────────────────────────────────────┐    │
│  │                       Game 进程层（同一个 chaos_game 二进制，加载不同 Lua 脚本）      │    │
│  │                                                                               │    │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌─────────┐  │    │
│  │  │ Game/1001  │  │FRIEND/2001 │  │ PVP/3001   │  │ PVE/4001   │  │TRADE/   │  │    │
│  │  │ 核心逻辑    │  │ 好友系统    │  │ 玩家对战    │  │ PVE内容    │  │5001     │  │    │
│  │  │ ECS/AOI/   │  │(friend.lua)│  │(pvp.lua)   │  │(pve.lua)   │  │(trade.  │  │    │
│  │  │ Cell       │  │            │  │            │  │            │  │ lua)    │  │    │
│  │  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └────┬────┘  │    │
│  │        │               │               │               │              │       │    │
│  │        │         Game↔Game 通信经 Router 中转            │              │       │    │
│  │        │               │               │               │              │       │    │
│  └────────┼───────────────┼───────────────┼───────────────┼──────────────┼───────┘    │
│           │               │               │               │              │            │
│           │    ┌──────────┴───────────────┴───────────────┴──────────────┘            │
│           │    │                                                                      │
│           │    ▼                                                                      │
│           │  ┌──────────────────────────────────────────────────────────────────┐     │
│           │  │               Router 集群 (区域内)                                 │     │
│           │  │                                                                  │     │
│           │  │  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────┐  │     │
│           │  │  │  Router R1 (Lua) │◄──►  Router R2 (Lua) │◄──► Router R3    │  │     │
│           │  │  │  PID: 5001       │   │  PID: 5002       │   │ PID: 5003    │  │     │
│           │  │  │                  │   │                  │   │              │  │     │
│           │  │  │  服务注册表       │   │  服务注册表       │   │  服务注册表   │  │     │
│           │  │  │  一致性哈希环     │   │  一致性哈希环     │   │  一致性哈希环 │  │     │
│           │  │  │  健康检查         │   │  健康检查         │   │  健康检查     │  │     │
│           │  │  │  区域路由表       │   │  区域路由表       │   │  区域路由表   │  │     │
│           │  │  └──────────────────┘   └──────────────────┘   └──────────────┘  │     │
│           │  │                                                                  │     │
│           │  │        集群内广播同步路由表                                        │     │
│           │  └──────────────────────────────────────────────────────────────────┘     │
│           │                                                                           │
│           │    各 Game 进程连接独立 DBProxy                                                  │
│           ▼               ▼               ▼               ▼              ▼            │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌─────────┐         │
│  │DBProxy(主) │  │DBProxy(主) │  │DBProxy(主) │  │DBProxy(主) │  │DBProxy  │         │
│  │:9003       │  │:9013       │  │:9023       │  │:9033       │  │:9043    │         │
│  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └────┬────┘         │
│        │               │               │               │              │              │
│        ▼               ▼               ▼               ▼              ▼              │
│  ┌──────────────────────────────────────────────────────────────────────────────┐    │
│  │                         MongoDB 集群                                          │    │
│  │  chaos_game001_game / chaos_game001_friend / chaos_game001_pvp / ...         │    │
│  └──────────────────────────────────────────────────────────────────────────────┘    │
│                                                                                      │
│                    ┌─────────────────────────────────────────┐                       │
│                    │  跨区连接: asia-east ↔ eu-west          │                       │
│                    │  跨区连接: asia-east ↔ us-west          │                       │
│                    └──────────────┬──────────────────────────┘                       │
└───────────────────────────────────┼──────────────────────────────────────────────────┘
                                    │
        ┌───────────────────────────┼───────────────────────────┐
        │                           │                           │
        ▼                           ▼                           ▼
┌───────────────────────┐  ┌───────────────────────┐  ┌───────────────────────┐
│  大区: eu-west (欧洲区) │  │  大区: us-west (美洲区) │  │  大区: sa-east (南美区) │
│                       │  │                       │  │  (未来扩展)            │
│  ┌─────────────────┐  │  │  ┌─────────────────┐  │  │                       │
│  │  Router 集群     │  │  │  │  Router 集群     │  │  │  只需与现有大区 Router  │
│  │  (同 asia-east   │  │  │  │  (同 asia-east   │  │  │  建立 TCP 连接即可     │
│  │   结构)          │  │  │  │   结构)          │  │  │  加入全球网格          │
│  └────────┬────────┘  │  │  └────────┬────────┘  │  │                       │
│           │           │  │           │           │  │                       │
│  ┌────────┴────────┐  │  │  ┌────────┴────────┐  │  └───────────────────────┘
│  │ Gateway + Game 进程  │  │  │  │ Gateway + Game 进程  │  │
│  │ DBProxy + MongoDB│  │  │  │ DBProxy + MongoDB│  │
│  └─────────────────┘  │  │  └─────────────────┘  │
│                       │  │                       │
│  跨区连接:             │  │  跨区连接:             │
│  eu-west ↔ us-west   │  │  us-west ↔ eu-west    │
└───────────────────────┘  └───────────────────────┘

全球 Router 网格: 3 个大区，全互联拓扑，共 3 条跨区 TCP 长连接
跨区消息格式: {source_region, target_region, player_id, body}
Gateway → Game 直连（客户端消息路径），Game ↔ Game 经 Router（Game 进程间通信）
```

### 模块分层

```
┌──────────────────────────────────────────────────────────────────┐
│                        应用层                                      │
│  ce_gateway_main.c  (Gateway 进程入口)                             │
│  ce_router_main.c   (Router 进程入口)                              │
│  ce_game_main.c     (Game 进程入口，加载不同 Lua 业务脚本)              │
├──────────────────────────────────────────────────────────────────┤
│                    Lua 业务层                                      │
│  src_lua/gateway/   (连接管理、协议适配、消息转发直连 Game)          │
│  src_lua/router/    (服务注册、一致性哈希、集群同步、健康检查)        │
│  src_lua/services/  (功能模块 Lua 业务脚本: friend.lua, pvp.lua, trade.lua 等) │
├──────────────────────────────────────────────────────────────────┤
│                 ce_net_base (共享网络库，C)                         │
│  TCP 连接管理 / 二进制协议编解码 / 心跳检测 / 连接池                 │
│  Gateway 和 Router 共享此库                                        │
├──────────────────────────────────────────────────────────────────┤
│                  现有模块 (不变)                                    │
│  engine_core (ECS/AOI/Cell/Network/Log)                            │
│  engine_sync (Game↔DBProxy 状态同步)                                │
│  engine_save (存档管理器)                                           │
│  engine_dbproxy (DBProxy 客户端)                                    │
│  engine_admin_ipc (Admin JSON-RPC IPC)                             │
└──────────────────────────────────────────────────────────────────┘
```

### 进程拓扑

```
进程                    机器        端口              职责
──────────────────────────────────────────────────────────────────
chaos_gateway           A         9000 (TCP)       客户端接入 + 协议适配
                                  9001 (KCP)       
                                  9002 (WebSocket) 
chaos_router (R1)       B         9100             服务注册/路由查询
                                  9101             集群内部通信
chaos_router (R2)       C         9100             服务注册/路由查询
                                  9101             集群内部通信
chaos_game (GAME)       D         7777             核心游戏逻辑
Lua DBProxy (GAME)      D         9003             数据库代理
chaos_game (FRIEND)     E         9100 (注册端口)   好友 Game 进程（加载 friend.lua）
Lua DBProxy (FRIEND)    E         9013             数据库代理
chaos_game (PVP)        F         9100 (注册端口)   PVP Game 进程（加载 pvp.lua）
Lua DBProxy (PVP)       F         9023             数据库代理
chaos_game (PVE)        G         9100 (注册端口)   PVE Game 进程（加载 pve.lua）
Lua DBProxy (PVE)       G         9033             数据库代理
chaos_game (TRADE)      H         9100 (注册端口)   交易 Game 进程（加载 trade.lua）
Lua DBProxy (TRADE)     H         9043             数据库代理
MongoDB                 I        27017             数据持久化
```

## Decisions

### Decision 1: ce_net_base 共享网络库 vs 各自实现

**选择**：抽离 ce_net_base 共享 C 库，Gateway 和 Router 均链接此库。

**理由**：
- **消除代码重复**：Gateway 和 Router 都需要 TCP 连接管理、二进制协议编解码、心跳检测、连接池。抽离为共享库后，两者共享同一套实现，减少 ~2000 行重复代码。
- **统一协议**：Gateway 和 Router 使用相同的二进制协议格式，客户端 → Gateway → Game 和 Game → Router → Game 进程的消息无需协议转换。
- **统一维护**：网络层的 bug 修复和性能优化只需在一处进行，Gateway 和 Router 同时受益。
- **Gateway 迁移友好**：Gateway 原本内联的网络逻辑替换为 ce_net_base 调用，Gateway 代码量减少 ~40%，聚焦于协议适配和消息转发（直连 Game）。Gateway 不引入路由功能。

**替代方案**：
- ❌ 各自实现：代码重复，协议可能不一致，维护负担翻倍。
- ❌ 全部放在 Lua 层：性能关键路径（协议编解码、连接池）在 C 层更高效。

**ce_net_base 接口设计**：
```c
// 连接管理
typedef struct CeNetBaseConn CeNetBaseConn;
CeNetBaseConn* ce_net_base_connect(const char* host, int port);
void ce_net_base_disconnect(CeNetBaseConn* conn);
int  ce_net_base_reconnect(CeNetBaseConn* conn, int max_retries);

// 消息收发
int ce_net_base_send(CeNetBaseConn* conn, uint16_t msg_type, const uint8_t* payload, uint32_t len);
int ce_net_base_recv(CeNetBaseConn* conn, uint16_t* msg_type, uint8_t* payload, uint32_t* len);

// 心跳
int ce_net_base_heartbeat_start(CeNetBaseConn* conn, int interval_ms, int timeout_ms);
int ce_net_base_heartbeat_stop(CeNetBaseConn* conn);
bool ce_net_base_is_alive(CeNetBaseConn* conn);

// 连接池
typedef struct CeNetBasePool CeNetBasePool;
CeNetBasePool* ce_net_base_pool_create(int min_conn, int max_conn);
CeNetBaseConn* ce_net_base_pool_acquire(CeNetBasePool* pool);
void ce_net_base_pool_release(CeNetBasePool* pool, CeNetBaseConn* conn);

// 协议编解码
int ce_net_base_pack(uint16_t msg_type, const uint8_t* payload, uint32_t len, uint8_t* buf, uint32_t* buf_len);
int ce_net_base_unpack(const uint8_t* buf, uint32_t buf_len, uint16_t* msg_type, uint8_t* payload, uint32_t* payload_len);
```

### Decision 2: 一致性哈希 vs 范围分片 vs 随机路由

**选择**：一致性哈希（虚拟节点数 150），路由键为 player_id。

**理由**：
- **会话亲和性**：同一 player_id 始终路由到同一进程，避免跨进程状态查询，简化 Game 进程内部逻辑。
- **扩缩容友好**：进程增减时仅影响相邻虚拟节点覆盖的 player_id 范围，数据迁移量最小（约 1/N，N 为进程数）。
- **负载均衡**：150 个虚拟节点使哈希环分布均匀，各进程负载接近均衡。
- **O(log N) 查询**：虚拟节点在有序数据结构（如跳表或红黑树）中二分查找，查询效率高。

**替代方案**：
- ❌ 范围分片（player_id % N）：扩缩容时几乎所有 player_id 都需要重新分配，数据迁移量接近 100%。
- ❌ 随机路由：无会话亲和性，每次请求可能路由到不同进程，Game 进程需要无状态设计或分布式缓存。
- ❌ 最少连接数路由：需要 Router 维护每个进程的连接数，增加复杂度；且无法保证会话亲和性。

**一致性哈希实现细节**：
```
哈希环结构:
- 虚拟节点数: 150 per 物理进程
- 哈希函数: MD5(player_id) 取前 4 字节作为 uint32
- 虚拟节点命名: "{process_id}:{vnode_index}" (如 "2001:0", "2001:1", ... "2001:149")
- 虚拟节点哈希: MD5("{process_id}:{vnode_index}") 取前 4 字节
- 查找: 二分查找 >= hash(player_id) 的最小虚拟节点，环形回绕

进程注册时:
  for i in 0..149:
    vnode_hash = hash(process_id + ":" + i)
    ring[vnode_hash] = {process_id, address}

进程注销时:
  for i in 0..149:
    delete ring[hash(process_id + ":" + i)]

路由查询:
  h = hash(player_id)
  vnode = ring.lower_bound(h)  // 第一个 >= h 的虚拟节点
  if vnode == ring.end():
    vnode = ring.begin()        // 环形回绕
  return vnode.process_id, vnode.address
```

### Decision 3: Router 集群广播同步 vs 集中式存储

**选择**：Router 集群内广播同步路由表（Gossip-like），每个 Router 维护完整路由表副本。

**理由**：
- **无单点故障**：任何 Router 实例均可独立响应路由查询，不依赖中心化存储。
- **低延迟**：路由查询在本地内存完成，无需跨网络访问外部存储。
- **部署简单**：无需额外部署 etcd/Consul/ZooKeeper 等外部协调服务。
- **最终一致可接受**：路由表变更（服务注册/注销）不是高频操作（每秒数次），广播延迟（<100ms）可接受。

**替代方案**：
- ❌ 集中式存储（如 Redis）：引入外部依赖，增加部署复杂度和故障点。
- ❌ Raft 强一致性：实现复杂度高，路由表变更频率低，强一致性收益不大。
- ❌ 单 Router 实例：单点故障，不可接受。

**广播同步协议**：
```
消息类型:
  REGISTER_NOTIFY   — 服务注册广播
  DEREGISTER_NOTIFY — 服务注销广播
  HEALTH_CHANGE     — 健康状态变更广播
  SYNC_REQUEST      — 全量同步请求（新 Router 加入时）
  SYNC_RESPONSE     — 全量同步响应
  REGION_UPDATE     — 区域路由表变更广播

广播流程:
  1. Router R1 收到 REGISTER 请求
  2. R1 更新本地路由表，分配序列号 seq=N
  3. R1 向所有已知集群成员发送 REGISTER_NOTIFY {seq: N, ...}
  4. 收到广播的 Router 更新本地路由表
  5. 若某 Router 未响应（超时 2s），R1 重试最多 3 次

序列号机制:
  - 每个 Router 维护自己的递增序列号
  - 广播消息携带 {router_id, seq}
  - 收到广播时，若 seq > local_seq[router_id]，应用更新
  - 若 seq <= local_seq[router_id]，忽略（已处理）
```

### Decision 4: 跨大区 Router 全球网格 vs 跨区域消息队列

**选择**：Router 间直接 TCP 长连接，全互联拓扑，组成全球 Router 网格。

**理由**：
- **低延迟**：直连 TCP 比经过消息队列（Kafka/RabbitMQ）少一跳，延迟更低。
- **部署简单**：无需额外的消息队列集群，减少运维负担。
- **全互联可控**：大区数量通常较少（3-5 个），全互联连接数可控（N*(N-1)/2 条连接）。
- **故障隔离**：单个跨区连接断开不影响其他大区通信。
- **可扩展**：新增大区时，只需与现有大区 Router 建立 TCP 连接即可加入全球网格。

**替代方案**：
- ❌ 消息队列（Kafka/RabbitMQ）：引入重量级外部依赖，增加延迟（至少 +10ms），运维复杂。
- ❌ 中心化跨区域 Hub：单点故障，所有跨区域流量经过 Hub 成为瓶颈。

**跨区消息格式**：
```
跨区消息头:
┌──────────────┬──────────────┬──────────────┬──────────────┬──────────────────────┐
│  length (4B) │  type  (2B)  │ src_region(2B)│ dst_region(2B)│  payload (变长)       │
└──────────────┴──────────────┴──────────────┴──────────────┴──────────────────────┘

大区标识:
  0x0001  asia-east   (亚洲区)
  0x0002  eu-west     (欧洲区)
  0x0003  us-west     (美洲区)
  0x0004  us-east     (美东区)
  0x0005  sa-east     (南美区)
```

### Decision 5: Game 进程通过 Router 中转 vs Game 进程直连

**选择**：所有 Game 进程间（Game↔Game）消息通过 Router 中转，Game 进程之间不建立直接连接。Gateway 直连 Game，不经过 Router。

**理由**：
- **集中路由管控**：Router 统一管理 Game↔Game 消息路由，便于监控、限流、审计。
- **简化 Game 进程**：Game 进程只需维护与 Router 的一条连接，调用 `ce_rpc_call()` 即可通信，无需管理与其他 Game 进程的连接。
- **服务发现透明**：Game 进程只需指定目标 service_type 和路由键，Router 负责查找目标地址。
- **故障隔离**：目标 Game 进程不可用时，Router 返回错误，不会影响源 Game 进程的连接状态。
- **客户端路径低延迟**：Gateway→Game 直连，客户端消息不经过 Router，减少一跳延迟。

**替代方案**：
- ❌ Game 进程直连：每个 Game 进程需要维护到所有其他 Game 进程的连接（O(N²) 连接数），服务发现逻辑分散在各 Game 进程中。
- ❌ 事件总线（Pub/Sub）：异步模式不适合请求-响应场景（如查询玩家在线状态）。

**消息路由流程**：
```
Game 进程 A → Router → Game 进程 B:

1. Game 进程 A 调用 ce_rpc_call() 构造消息:
   {
     src_service: "FRIEND",
     src_process_id: 2001,
     dst_service: "GAME",
     dst_key: {player_id: 99},      // 一致性哈希路由键
     msg_id: "req-001",             // 用于请求-响应匹配
     body: {action: "query_status"}
   }

2. Router 收到消息:
   a. 查询服务注册表: service_type = "GAME", status = ONLINE
   b. 一致性哈希: hash(player_id=99) → process_id=1001
   c. 查找 process_id=1001 的地址: 192.168.1.10:7777
   d. 转发消息到 192.168.1.10:7777

3. Game 进程 B (GAME/1001) 处理请求，构造响应:
   {
     dst_service: "FRIEND",
     dst_process_id: 2001,
     msg_id: "req-001",
     body: {status: "online", position: {x: 100, y: 200}}
   }

4. Router 收到响应:
   a. 直接路由: dst_process_id=2001 → 192.168.1.10:9100
   b. 转发响应到 FRIEND/2001
```

### Decision 6: 数据隔离 — 独立 DBProxy vs 共享 DBProxy

**选择**：每个 Game 进程连接独立的 DBProxy 实例（或共享 DBProxy 但使用独立数据库）。

**理由**：
- **数据边界清晰**：每个 Game 进程拥有独立的数据库命名空间（`chaos_{game_id}_{service_type}`），数据模型独立演进。
- **故障隔离**：一个 Game 进程的数据库压力不影响其他 Game 进程。
- **独立扩展**：每个 Game 进程的 DBProxy 可独立扩容（增加连接数、读写分离）。
- **安全边界**：Game 进程 A 无法直接访问 Game 进程 B 的数据库，必须通过 Router 发送消息。

**替代方案**：
- ❌ 共享数据库：Game 进程间数据耦合，一方的 schema 变更影响其他方；无法独立扩容。
- ❌ 每个 Game 进程直连 MongoDB：失去 DBProxy 的连接池复用、主备切换、协议抽象等优势。

## Data Flow

### 1. 客户端请求 → Game 进程响应（完整链路）

```
时间线 →

客户端          Gateway           Game(核心)          Router            Game进程(FRIEND)    DBProxy(FRIEND)
──────          ───────           ─────────           ──────            ──────────────      ───────────────
   │               │                  │                  │                    │                   │
   │ ──好友请求──→  │                  │                  │                    │                   │
   │ {msg_type:    │                  │                  │                    │                   │
   │  0x0300,      │                  │                  │                    │                   │
   │  player_id:42,│                  │                  │                    │                   │
   │  action:      │                  │                  │                    │                   │
   │  "add_friend",│                  │                  │                    │                   │
   │  target_id:99}│                  │                  │                    │                   │
   │               │                  │                  │                    │                   │
   │               │ Gateway 直连 Game │                  │                    │                   │
   │               │ ──转发消息────→  │                  │                    │                   │
   │               │                  │                  │                    │                   │
   │               │                  │ Game 需要调用     │                    │                   │
   │               │                  │ 好友进程处理请求   │                    │                   │
   │               │                  │                  │                    │                   │
   │               │                  │ ─ce_rpc_call()─→ │                    │                   │
   │               │                  │ {player_id:42,   │                    │                   │
   │               │                  │  dst:FRIEND,     │                    │                   │
   │               │                  │  body:{...}}     │                    │                   │
   │               │                  │                  │                    │                   │
   │               │                  │                  │ 一致性哈希:         │                   │
   │               │                  │                  │ hash(42) → 2001   │                   │
   │               │                  │                  │                    │                   │
   │               │                  │                  │ ──路由到2001──→   │                   │
   │               │                  │                  │                    │                   │
   │               │                  │                  │                    │ 处理好友请求       │
   │               │                  │                  │                    │ 验证 target_id    │
   │               │                  │                  │                    │                   │
   │               │                  │                  │                    │ ──保存好友关系──→ │
   │               │                  │                  │                    │                   │ 写入
   │               │                  │                  │                    │                   │ chaos_.._friend
   │               │                  │                  │                    │ ←──保存成功────   │
   │               │                  │                  │                    │                   │
   │               │                  │                  │                    │ 构造响应          │
   │               │                  │                  │ ←──响应────────   │                   │
   │               │                  │                  │                    │                   │
   │               │                  │ ←──响应────────  │                    │                   │
   │               │                  │                  │                    │                   │
   │               │ ←──响应────────  │                  │                    │                   │
   │               │                  │                  │                    │                   │
   │ ←──响应────   │                  │                  │                    │                   │
   │ {status:"ok"} │                  │                  │                    │                   │
```

### 2. 跨大区消息转发（全球 Router 网格）

```
时间线 →

asia-east 大区                        us-west 大区
────────────                          ──────────

玩家A (player_id=42)                  玩家B (player_id=99)
   │                                     │
   │ ──PVP挑战──→                        │
   │                                     │
Gateway (asia-east)                      │
   │                                     │
   │ 直连 Game                            │
   ▼                                     │
Game (asia-east)                         │
   │                                     │
   │ Game→Router（跨区消息）              │
   ▼                                     │
Router R1 (asia-east)                    │
   │                                     │
   │ 识别目标大区: us-west                │
   │                                     │
   │ ──跨区TCP转发──────────────────→    │
   │ {source_region: asia-east,          │
   │  target_region: us-west,            │
   │  player_id: 99,                     │
   │  body: {action: "pvp_challenge"}}   │
   │                                     ▼
   │                              Router R1 (us-west)
   │                                     │
   │                                     │ 一致性哈希: hash(99) → PVP/3002
   │                                     │
   │                                     ▼
   │                              PVP 服务 (us-west)
   │                                     │
   │                                     │ 通知玩家B（经 Game → Gateway）
   │                                     │
   │                              ←──响应────────
   │                                     │
   │ ←──跨区TCP转发──────────────────    │
   │ {source_region: us-west,            │
   │  target_region: asia-east}          │
   │                                     │
   ▼                                     │
Router R1 (asia-east)                    │
   │                                     │
   │ ──响应──→ Game (asia-east)          │
   ▼                                     │
Game (asia-east)                         │
   │                                     │
   │ ──响应──→ Gateway                   │
   ▼                                     │
Gateway (asia-east)                      │
   │                                     │
   │ ──PVP响应──→ 玩家A                  │
```

### 3. 服务注册与路由表同步

```
时间线 →

Game进程(FRIEND/2001)   Router R1          Router R2          Router R3
─────────────────      ────────           ────────           ────────
   │                      │                  │                  │
   │ ──REGISTER────→     │                  │                  │
   │ {FRIEND, 2001,      │                  │                  │
   │  192.168.1.10:9100} │                  │                  │
   │                      │                  │                  │
   │                      │ 更新本地路由表    │                  │
   │                      │ 更新一致性哈希环  │                  │
   │                      │                  │                  │
   │                      │ ──REGISTER_NOTIFY──→               │
   │                      │ {seq: 42, ...}   │                  │
   │                      │                  │                  │
   │                      │ ───────────────────REGISTER_NOTIFY─→│
   │                      │                  │ {seq: 42, ...}   │
   │                      │                  │                  │
   │                      │                  │ 更新本地路由表    │
   │                      │                  │                  │
   │                      │                  │                  │ 更新本地路由表
   │                      │                  │                  │
   │ ←──REGISTER_ACK────  │                  │                  │
   │                      │                  │                  │
   │ [开始心跳]            │                  │                  │
   │ ──PONG────────────→  │                  │                  │
   │ ←──PING────────────  │                  │                  │
```

## Risks / Trade-offs

### Risk 1: Router 成为单点瓶颈

- **风险**：所有服务间消息经过 Router 中转，Router 可能成为性能瓶颈。
- **缓解**：
  - Router 集群多实例部署，Game 进程可连接不同 Router 实例分散负载。
  - Router 内部使用 Lua 协程处理并发，单实例可支撑数万 QPS。
  - 路由查询在内存中完成（O(log N) 哈希环查找），延迟极低（<100μs）。
  - 未来可引入消息直连优化：高频通信的 Game 进程对可在 Router 协调下建立直连。
- **影响**：中。单 Router 实例可支撑当前规模，多实例集群可水平扩展。

### Risk 2: 路由表广播延迟导致短暂不一致

- **风险**：服务注册/注销后，集群内其他 Router 在广播到达前路由表不一致，可能将消息路由到已下线进程。
- **缓解**：
  - 广播延迟通常在 100ms 以内，不一致窗口极短。
  - 发送方 Router 在转发消息时若连接失败，立即更新本地路由表并重试路由。
  - Game 进程进入 DRAINING 状态后再注销，确保进行中请求完成。
- **影响**：低。短暂不一致仅影响新请求，且发送方 Router 有重试机制。

### Risk 3: 跨区域网络延迟

- **风险**：跨区域消息（如 us-west ↔ ap-southeast）延迟可能达到 150-300ms，影响实时性要求高的场景（如跨服 PVP）。
- **缓解**：
  - 跨区域消息仅用于必须跨区的场景（如跨服 PVP、全球聊天），同区域消息不受影响。
  - 对延迟敏感的场景（如实时战斗），建议匹配同区域玩家。
  - 未来可引入区域就近路由（将玩家分配到最近区域）。
- **影响**：中。跨区域延迟是物理限制，需在游戏设计层面规避。

### Risk 4: 一致性哈希的数据迁移

- **风险**：进程扩缩容时，player_id 范围重新分配，已在原进程内存中的玩家数据需要迁移。
- **缓解**：
  - 数据迁移由 Game 进程自身处理（从 DBProxy 加载），Router 不参与。
  - 缩容时使用 DRAINING 状态，给原进程时间完成数据持久化。
  - 玩家重连时自动从 DBProxy 加载数据到新进程。
- **影响**：中。需要 Game 进程实现数据加载逻辑，但架构上已支持（DBProxy 持久化）。

### Trade-off: 集中路由 vs 直连

- **选择**：Game↔Game 通过 Router 集中路由，Gateway→Game 直连。
- **代价**：Game↔Game 消息增加一跳网络延迟（~0.1-1ms 内网），Router 成为 Game 进程间通信的关键路径。
- **收益**：统一服务发现、集中管控、简化 Game 进程、连接数从 O(N²) 降为 O(N)。客户端路径（Gateway→Game）保持低延迟。
- **缓解**：Router 集群多实例 + 内存路由表 + 协程并发。

### Trade-off: 广播同步 vs 强一致性

- **选择**：广播同步（最终一致）。
- **代价**：短暂的路由表不一致窗口（<100ms）。
- **收益**：无外部依赖、部署简单、低延迟本地查询。
- **缓解**：发送方 Router 的重试机制 + DRAINING 状态。
