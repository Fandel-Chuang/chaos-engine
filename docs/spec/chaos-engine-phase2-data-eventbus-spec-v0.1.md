# ChaosEngine Phase 2 — DBProxy 分片与事件总线设计规格说明书

- **文档版本**：v0.1（草案）
- **覆盖范围**：演进计划 `docs/spec/chaos-engine-evolution-plan-v1.md` Phase 2 的 **2.7 DBProxy 分片**、**2.8 事件总线（Redis Stream）**
- **作者**：zhongfangdao
- **状态**：待评审
- **上游依据**：`chaos-engine-evolution-plan-v1.md` 第 288–380 行（Phase 2，2.1–2.8 共 8 项）
- **不覆盖**：2.1–2.3（Router 纯 C + Raft + 实体路由表）、2.4–2.6（Cell 迁移 / Ghost / 跨 Cell AOI）。本文仅定义与它们之间的**接口契约**，不重复设计其内部实现。

---

## 0. 全局设计约束（贯穿本文所有章节，不可违反）

以下约束来自项目既定工程纪律，本文所有设计必须满足；任何实现 PR 若违反其中任意一条，评审直接拒绝。

| 编号 | 约束 | 在本文中的落点 |
|---|---|---|
| C-1 | **性能关键组件必须纯 C 实现，坚决拒绝 Lua** | 分片路由层（`ce_shard_router`）、事件总线（`ce_eventbus`）全部为纯 C99；`src_lua/router/hash_ring.lua` 仅作为**算法思路参考**，不作为运行时依赖，也不通过 Lua 绑定调用 |
| C-2 | **异步 I/O 必须用 io_uring（liburing 2.14），不允许用 epoll**；跨平台靠已有 `src_c/network/ce_async_io.h` 抽象层（Windows 对应 IOCP） | hiredis 必须以**非阻塞 + 外部事件循环**模式接入 `ce_async_io`，见 §4.3；禁止在 eventbus 中出现 `epoll_create` / `epoll_wait` |
| C-3 | **多协议倾向共用同一端口号** | DBProxy 分片实例的分片管理协议（`DB_SHARD_*` 消息族）复用既有 DBProxy 数据端口 `CE_DBPROXY_DEFAULT_PORT = 9700`，通过 `msg_type` 分流，不新开管理端口，见 §2.4 |
| C-4 | **公共代码只写一份，禁止客户端/服务端各抄一份**；`src_c/server/ce_game_protocol.h` 是协议常量与结构体的唯一权威来源 | 新增的分片消息类型、事件总线事件类型常量一律进 `ce_game_protocol.h`，Game 侧与 DBProxy 侧共用同一份声明，见 §2.4 / §3.5 |
| C-5 | **测试与验证必须集成进现有 GitHub Actions CI 流水线**（`.github/workflows/ci.yml`，已有 5 个中文 job：`Lua 语法检查` / `Gateway 集成测试` / `编译 & 测试 (Debug)` / `Release 编译` / `内存检查 (Valgrind)`），尽量复用已有构建产物，**不新增独立 job** | 验收章节 §6 明确每一项接到哪个既有 job |
| C-6 | **日志宏签名是 `CE_LOG_ERROR(cat, fmt, ...)`**，第一个参数是分类 tag 字符串 | 本文示例统一使用 tag `"DBPROXY"` / `"SHARD"` / `"EVENTBUS"` |
| C-7 | **提交格式 `[type](scope): description`**，type ∈ {feat, fix, refactor, docs, perf, test}，scope ∈ {core, ecs, render, network, server, plugin, editor, script, engine} | 见 §7 落地路线图中的提交示例 |
| C-8 | **压测脚本用 Lua**（共享引擎网络层 C 绑定，Headless 模式，多协程并发 + QPS/P50/P99 统计），不要用 Python | 见 §6.4 压测方案 |
| C-9 | **关闭服务用 `scripts/stop_cluster_server.sh`**，不要直接 `kill` / `pkill` | 见 §6.4、§8 回退操作 |

---

## 1. 现状分析（Phase 1 交付物精读）

### 1.1 `src_c/dbproxy/` 目录构成

Phase 1 结束时该目录共 5 个文件：

| 文件 | 职责 | 关键内容 |
|---|---|---|
| `ce_dbproxy.h` | **Game 侧客户端**接口 | 不透明句柄 `CeDbproxyContext`；消息枚举 `CeDbproxyMsgType`（`DB_SAVE_PLAYER=0x01`、`DB_LOAD_PLAYER=0x02`、`DB_SAVE_WORLD=0x03`、`DB_LOAD_WORLD=0x04`、`DB_HEARTBEAT=0x05`、`DB_ERROR=0xFF`）；常量 `CE_DBPROXY_DEFAULT_PORT=9700`、`CE_DBPROXY_MAX_MSG_SIZE=256KiB`、`CE_DBPROXY_DEFAULT_TIMEOUT_MS=10000`、`CE_DBPROXY_DEFAULT_HEARTBEAT_MS=5000`、重连退避 `1s → 30s`；配置结构 `CeDbproxyConfig{primary_host, primary_port, backup_host, backup_port, heartbeat_ms, timeout_ms}` |
| `ce_dbproxy.c` | 客户端实现 | 二进制协议 `[4B total_len][2B msg_type][N payload]`（大端，`total_len = 6 + payload_len`）；非阻塞连接 + `ce_dbproxy_send/recv`；`ce_dbproxy_set_master()` 主备地址切换（延迟到下次重连生效） |
| `ce_dbproxy_native.h` | **DBProxy 服务侧**原生 MongoDB 驱动接口 | 不透明 `CeDbproxyNativeCtx`；统计 `CeDbproxyNativeStats{total_saves, total_loads, total_batch_saves, total_errors}`；`init/shutdown/save/load/batch_save/get_stats` 六个函数 |
| `ce_dbproxy_native.c` | libmongoc 实现 | 见 §1.3 |
| `CMakeLists.txt` | 构建 | `pkg_check_modules(MONGOC QUIET mongoc)`；命中则 `HAVE_MONGOC=1` 并链接，未命中则编译出空实现 |

### 1.2 当前"单 DBProxy 主备"架构

- **拓扑**：N 个 Game 进程 → 1 个 DBProxy 进程（primary）→ MongoDB；另有一个 backup DBProxy 地址写在客户端配置里。
- **故障切换是客户端侧的**：`CeDbproxyConfig` 同时持有 `primary_host/port` 与 `backup_host/port`，客户端在连接失败后按 `1s→30s` 指数退避重连；`ce_dbproxy_set_master()` 允许外部（运维或 Router）改写主备地址，但**不立即重连**，下次重连时才生效。
- **没有共识**：主备切换依赖外部判定，DBProxy 之间无心跳、无选举、无数据分区概念。backup 在正常时期完全空闲，是纯冷备。
- **所有玩家共用一条链路**：`DB_SAVE_PLAYER` / `DB_LOAD_PLAYER` 无论 `player_id` 为何，都走同一个 TCP 连接、同一个 DBProxy 进程、同一个 `mongoc_client_pool_t`。

### 1.3 libmongoc 连接池与批量写入实现方式

`ce_dbproxy_native.c` 在 `HAVE_MONGOC` 下的实现要点：

1. **上下文**：`struct CeDbproxyNativeCtx { mongoc_client_pool_t* pool; mongoc_uri_t* uri; char db_name[64]; char coll_name[64]; CeDbproxyNativeStats stats; }`，固定 `db = "chaos_engine"`，`coll = "players"`。
2. **连接池**：`ce_dbproxy_native_init()` 调 `mongoc_init()`（幂等）→ `mongoc_uri_new_with_error()` → `mongoc_client_pool_new()` → `mongoc_client_pool_max_size(pool, pool_size)` + `min_size(pool, 1)`。建议 `pool_size` 4–16。`client_pool` 自身线程安全，因此多线程可并发 `pop/push`。
3. **单条写**：`pool_pop` → `mongoc_client_get_collection` → `BCON_NEW("_id", BCON_INT64(player_id))` 作 query，`{$set:{data:<binary>}}` 作 update，`mongoc_collection_update(coll, MONGOC_UPDATE_UPSERT, ...)` → `pool_push`。即 **`_id` 就是 `player_id`，upsert 语义**。
4. **单条读**：`find_with_opts` 带 `projection {data:1}`，取游标第一条，`bson_iter_binary()` 拷出二进制，`malloc` 后交由调用方 `free`。未找到时打 `CE_LOG_WARN("DBPROXY", ...)` 并计入 `total_errors`。
5. **批量写**：`mongoc_collection_create_bulk_operation_with_opts()` → 循环 `mongoc_bulk_operation_update(bulk, query, update, /*upsert=*/true)` → 一次 `mongoc_bulk_operation_execute()`。统计上 `total_batch_saves++` 且 `total_saves += count`。
6. **统计与日志**：全程 `CE_LOG_*("DBPROXY", ...)`；`shutdown` 时打印累计 saves/loads/errors；**不调用 `mongoc_cleanup()`**（避免影响其他模块）。
7. **无 mongoc 时**：`#else` 分支给出全部符号的空实现（返回 `CE_ERR` + 一条 `CE_LOG_WARN`），保证链接不断——这是本项目对第三方依赖的**统一降级范式**，§4 的 hiredis 接入必须照抄这一范式。

### 1.4 无分片带来的三类瓶颈

**（a）单点写入吞吐上限**
所有 `SAVE` 请求汇聚到单进程的单个 `client_pool`。瓶颈叠加三层：单条 TCP 连接的 `recv` 串行解包；`pool_size ≤ 16` 决定并发在途 MongoDB 操作数；`bulk_operation` 虽把 N 次往返压成 1 次，但整批仍串行落在同一个 mongod 主节点。经验上单 DBProxy 的稳定写入天花板在 **1–2 万 upsert/s** 量级，且随批内文档变大迅速下滑。全球同服目标下这不够。

**（b）内存上限**
DBProxy 是有状态代理：待落盘的脏玩家队列、`CeDbproxyResponse` 中 **每连接 256 KiB 的内部响应缓冲**（`payload[CE_DBPROXY_MAX_MSG_SIZE]`）、bulk 构造期间同时活着的 `bson_t` query/update 对。单进程地址空间与单机内存把"同时在线且有脏数据的玩家数"锁死在一个固定量级，加机器也无法分摊——因为只有一个进程在干活。

**（c）故障域过大**
DBProxy primary 崩溃 = **全服所有玩家**存档链路中断。backup 是冷备：无预热连接池、无脏数据、切换后需重建 MongoDB 连接并等待客户端退避重连（最坏 30s）。故障域 = 100% 玩家，恢复时间 = 退避 + 冷启动。这与"全球同服"的可用性目标直接冲突。

**结论**：必须做 2.7 分片——把故障域从"全服"降到"1/N 玩家"，把写入吞吐从"单进程"变成"N 进程线性扩展"，把内存压力按 `player_id` 空间切开。

---
## 2. 2.7 DBProxy 分片

### 2.1 目标与总体拓扑

**目标**：按 `player_id` 哈希分片，每个 DBProxy 实例只管理一部分玩家；Game 侧透明——上层继续调 `ce_dbproxy_save_player(player_id, ...)`，由**分片路由层**决定落到哪个实例。

```
                    ┌──────────────── etcd（已有 ce_etcd_registry）────────────────┐
                    │  /chaos/services/dbproxy_shard/<host>:<port>  (含 metadata)  │
                    │  /chaos/shardmap/dbproxy/epoch  +  slot→shard 映射表          │
                    └───────────┬──────────────────────────────────────────────────┘
                                │ Watch + 心跳 + 本地缓存 TTL
   ┌──────────┐   ce_shard_router（纯 C，新增）
   │ Game #1  │──▶ hash(player_id) → slot → shard_id → CeDbproxyContext*
   ├──────────┤                │                 │                 │
   │ Game #2  │──▶ ───────────┐│                 │                 │
   └──────────┘               ▼▼                 ▼                 ▼
                        DBProxy shard0      DBProxy shard1     DBProxy shard2
                        (libmongoc pool)    (libmongoc pool)   (libmongoc pool)
                              │                   │                  │
                              ▼                   ▼                  ▼
                        MongoDB RS-A        MongoDB RS-B       MongoDB RS-C
```

要点：
- **分片路由层跑在 Game 进程内（客户端侧分片）**，不引入额外网络跳数。DBProxy 实例本身也持有同一份分片元数据，用于**拒绝错投请求**（防止元数据不一致导致的双写）。
- 每个分片一个独立 `CeDbproxyNativeCtx`（独立 `mongoc_client_pool_t`），可指向同一 MongoDB 集群的不同库/集合，也可指向不同副本集。MVP 阶段允许所有分片共用一个 MongoDB URI，仅按 `coll_name = players_s<N>` 区分——这样先验证路由层，再验证存储扩展。
- 主备保留：每个分片自己有 primary/backup（沿用 `CeDbproxyConfig` 的双地址字段），即"**分片 × 主备**"二维冗余。

### 2.2 分片算法选型对比

| 维度 | 方案 A：取模 `player_id % N` | 方案 B：一致性哈希环（含虚拟节点） | 方案 C：**虚拟槽位（固定槽数 + 槽→分片映射表）** |
|---|---|---|---|
| 实现复杂度 | 极低（一行） | 中（有序环 + 二分查找 + vnode 生成，参考 `src_lua/router/hash_ring.lua`：每物理节点 150 vnode、DJB2/MD5 哈希、二分查找） | 中低（一次哈希 + 一次数组下标，映射表是普通数组） |
| 查找复杂度 | O(1) | O(log V)，V = 物理节点数 × 150 | **O(1)**，`slot = hash & (SLOT_COUNT-1)` 后直接查表 |
| 扩容 N→N+1 的迁移量 | **灾难性**：几乎全量重哈希，≈ (N)/(N+1) 的键要搬（3→4 时约 75% 的玩家要搬库） | ≈ 1/(N+1) 的键要搬（理论最优） | ≈ 1/(N+1) 的键要搬（等同 B），且**搬哪些槽完全由运维显式指定** |
| 数据分布均匀性 | 依赖 `player_id` 生成器是否均匀；若 ID 含机器号/时间戳位，会严重倾斜 | 靠 vnode 数量摊平，仍有 ±5%~10% 抖动 | **最好**：16384 槽平均分配，可手工微调把热槽单独挪走 |
| 迁移过程的可观测性 | 无法表达"部分迁移中"的中间状态 | 中间状态难以精确枚举（环上区间不是离散可数集合） | **天然可表达**：槽是离散有限集合，可给每个槽标 `MIGRATING/IMPORTING` 状态，逐槽灰度 |
| 元数据体积 | 0（只需 N） | 环表 = V 个 `(hash, shard_id)` 条目，V 可达数千 | 16384 × `uint16_t` = **32 KiB**，可整块放进 etcd 一个 key，也可整块驻留内存 |
| 热点玩家处理 | 不可能 | 只能靠加 vnode 间接影响 | **可以**：把热槽单独指向专用分片 |
| 缩容 | 同扩容，灾难 | 平滑 | 平滑，且可精确控制槽回收顺序 |

**推荐：方案 C —— 虚拟槽位，`CE_SHARD_SLOT_COUNT = 16384`（2^14）。**

理由：

1. **扩缩容迁移代价与一致性哈希持平，但状态机可枚举**。这是决定性因素。DBProxy 搬的是**玩家存档二进制数据**，不是缓存——搬错一个玩家就是掉档。一致性哈希在扩容瞬间"某个 key 的归属变了"这件事是隐式发生的（环上插入新 vnode 后查找结果自动改变），很难在 C 代码里精确表达"这个 key 正在搬迁中，读走旧分片、写要双写"。虚拟槽位把归属显式化成一张 `uint16_t slot_to_shard[16384]` 表 + 每槽一个状态位，迁移期间逐槽推进，任何时刻都能回答"槽 7391 现在是 STABLE 还是 MIGRATING、源是谁、目标是谁"。这对回滚和对账是刚性需求。
2. **O(1) 查找，无二分**。DBProxy 路由在玩家上线/下线/定时存盘的热路径上，且约束 C-1 要求纯 C 追求性能。`hash & 0x3FFF` 加一次数组访问（32 KiB 表，L2 常驻）比 O(log V) 二分 + 指针跳转快一个量级，且分支可预测。
3. **元数据紧凑，适配已有 etcd 客户端**。32 KiB 一个 value，一次 `ce_etcd_*` 读取就能拿全，配合 `epoch` 单调递增号做版本校验；不需要为环表设计增量同步协议。
4. **保留一致性哈希的思路精华**：槽的初始分配 **仍用一致性哈希环算法离线生成**（直接复用 `src_lua/router/hash_ring.lua` 的 DJB2 + 150 vnode 思路，写成一个 C 侧的离线工具函数），保证初始分布均匀且新增分片时"应该从哪些槽里各拿一点"有一致性哈希的最优性作依据。**运行时不跑环，只查表**——这是 Redis Cluster 已验证的工程折中。

**哈希函数**：`player_id` 是 `uint64_t`。采用固定的 64→32 位混淆（如 SplitMix64 finalizer 风格的乘移混合），再取低 14 位。**必须自实现且固定不变**（写死在头文件的 `static inline`，不依赖任何第三方哈希库），因为哈希函数一变就等于全量迁移。禁止使用 `player_id % 16384`（低位可能是自增位，会周期性倾斜）。

### 2.3 分片路由层结构体与函数签名（新增 `src_c/dbproxy/ce_dbproxy_shard.h/.c`）

> 以下为**接口设计**，不是实现。实现阶段严格遵守：纯 C99、`ce_` 前缀、不透明句柄、`CE_LOG_*(tag, fmt, ...)`。

```c
/* ---- 常量（唯一权威定义处，Game 与 DBProxy 共用） ---- */
#define CE_SHARD_SLOT_COUNT      16384    /* 2^14，固定，永不变更 */
#define CE_SHARD_SLOT_MASK       (CE_SHARD_SLOT_COUNT - 1)
#define CE_SHARD_MAX_SHARDS      256      /* uint8_t 不够，用 uint16_t 存 shard_id */
#define CE_SHARD_INVALID         0xFFFF
#define CE_SHARD_MAP_CACHE_TTL_MS 3000    /* 与 ce_etcd_registry 缓存 TTL 对齐 */

/* ---- 槽状态：迁移可枚举的关键 ---- */
typedef enum CeShardSlotState {
    CE_SLOT_STABLE    = 0,  /* 稳定：读写都走 owner */
    CE_SLOT_MIGRATING = 1,  /* 源侧：读走 owner，写双写 owner+target */
    CE_SLOT_IMPORTING = 2,  /* 目标侧：接受写入，读回源 */
    CE_SLOT_FROZEN    = 3,  /* 切换瞬间短暂冻结（毫秒级），请求排队重试 */
} CeShardSlotState;

/* ---- 单个分片实例描述 ---- */
typedef struct CeShardNode {
    uint16_t shard_id;
    char     primary_host[64];
    int      primary_port;
    char     backup_host[64];
    int      backup_port;
    int      weight;              /* 供离线槽分配用，来自 etcd metadata */
    CeBool   healthy;             /* 由 etcd Watch/心跳维护 */
} CeShardNode;

/* ---- 分片映射表（epoch + 槽表 + 迁移信息） ---- */
typedef struct CeShardMap {
    uint64_t          epoch;                            /* 单调递增版本号 */
    uint16_t          slot_owner[CE_SHARD_SLOT_COUNT];  /* 槽 → shard_id，32 KiB */
    uint8_t           slot_state[CE_SHARD_SLOT_COUNT];  /* CeShardSlotState，16 KiB */
    uint16_t          slot_target[CE_SHARD_SLOT_COUNT]; /* 迁移目标，非迁移时 INVALID */
    CeShardNode       nodes[CE_SHARD_MAX_SHARDS];
    int               node_count;
} CeShardMap;

/* ---- 不透明路由器句柄 ---- */
typedef struct CeShardRouter CeShardRouter;

/* ---- 哈希（固定实现，禁止修改） ---- */
static inline uint16_t ce_shard_slot_of(uint64_t player_id);

/* ---- 生命周期 ---- */
CeShardRouter* ce_shard_router_create(CeEtcdClient* etcd,
                                      const char*   service_name,   /* "dbproxy_shard" */
                                      const CeDbproxyConfig* tmpl); /* 超时/心跳模板 */
void           ce_shard_router_destroy(CeShardRouter* r);

/* ---- 元数据同步 ---- */
CeResult ce_shard_router_refresh(CeShardRouter* r);        /* 主动拉取 etcd，比对 epoch */
CeResult ce_shard_router_start_watch(CeShardRouter* r);    /* 注册 etcd Watch 回调 */
void     ce_shard_router_stop_watch(CeShardRouter* r);
uint64_t ce_shard_router_epoch(const CeShardRouter* r);

/* ---- 路由查询（热路径，O(1)，无锁读） ---- */
uint16_t ce_shard_router_lookup(const CeShardRouter* r, uint64_t player_id);
CeDbproxyContext* ce_shard_router_conn(CeShardRouter* r, uint64_t player_id);
CeShardSlotState  ce_shard_router_slot_state(const CeShardRouter* r, uint64_t player_id);

/* ---- 分片感知的数据面 API（Game 侧唯一入口，替代直接用 ce_dbproxy_send） ---- */
CeResult ce_shard_save_player(CeShardRouter* r, uint64_t player_id,
                              const uint8_t* data, uint32_t len);
CeResult ce_shard_load_player(CeShardRouter* r, uint64_t player_id,
                              uint8_t** out_data, uint32_t* out_len);

/* 批量：内部按 shard_id 分桶，每桶各发一次 batch，复用 native batch_save */
CeResult ce_shard_batch_save(CeShardRouter* r,
                             const uint64_t* ids, const uint8_t** datas,
                             const uint32_t* lens, int count);

/* ---- 统计 ---- */
typedef struct CeShardRouterStats {
    uint64_t lookups;
    uint64_t routed[CE_SHARD_MAX_SHARDS]; /* 每分片请求数，用于验证均匀性 */
    uint64_t epoch_reloads;               /* 元数据热更新次数 */
    uint64_t misroute_rejects;            /* 被分片拒绝的错投数 */
    uint64_t dual_writes;                 /* 迁移期双写次数 */
    uint64_t retries;                     /* FROZEN 导致的重试数 */
} CeShardRouterStats;
CeResult ce_shard_router_get_stats(const CeShardRouter* r, CeShardRouterStats* out);
```

**并发模型**：`CeShardMap` 采用 **RCU 风格双缓冲**——写侧（etcd Watch 线程）构造全新的 `CeShardMap` 后原子换指针（`_Atomic(CeShardMap*)`），读侧热路径纯读、零锁、零原子写。旧表延迟一个宽限期后释放。理由：读多写极少（epoch 变更是分钟/小时级事件），读路径必须零开销。

**日志示例（严格遵守 C-6）**：

```c
CE_LOG_INFO("SHARD", "shardmap reloaded: epoch=%llu nodes=%d migrating_slots=%d",
            (unsigned long long)m->epoch, m->node_count, migrating);
CE_LOG_ERROR("SHARD", "lookup failed: player=%llu slot=%u owner=INVALID epoch=%llu",
             (unsigned long long)player_id, slot, (unsigned long long)m->epoch);
```

### 2.4 分片元数据来源：复用 `src_c/rpc/ce_etcd_registry`

**不新建注册中心。** 已有 `ce_etcd_registry.h` 提供：`ce_etcd_create/destroy`、`ce_etcd_health_check`、`ce_etcd_register/deregister`、`ce_etcd_heartbeat` + `heartbeat_start/stop`（后台心跳线程）、`ce_etcd_discover`（带 `CE_ETCD_LB_ROUND_ROBIN/RANDOM/LEAST_CONN`）、`ce_etcd_list`、`ce_etcd_refresh_cache`（本地缓存 TTL）、`ce_etcd_watch/watch_stop`（`CE_ETCD_EVENT_PUT/DELETE` 回调）、实例结构 `CeEtcdInstance{name,host,port,weight,metadata[256],registered_ms}`。

**两类元数据分开存放：**

1. **实例存活信息 → 走标准服务注册**
   每个 DBProxy 分片实例启动时 `ce_etcd_register(cli, "dbproxy_shard", host, port, weight, metadata)`，其中 `metadata` 内嵌 `shard_id=<N>;role=primary|backup;coll=players_s<N>`（256 字节足够）。存活由已有的 `ce_etcd_heartbeat_start()` 维护，实例挂掉后由 etcd lease 过期 → `CE_ETCD_EVENT_DELETE` → 路由层把该 `CeShardNode.healthy = CE_FALSE` 并切到 backup。**这一层完全零新增代码**，只是新的 service name。

2. **槽表 → 单独一个 etcd key，靠 Watch 推送**
   Key：`/chaos/shardmap/dbproxy`，Value：`epoch(8B) + slot_owner[16384](32KiB) + slot_state[16384](16KiB) + slot_target[16384](32KiB)`，紧凑二进制，小端固定布局（**不用 JSON**，避免 80 KiB 文本解析进热路径；也不引入新序列化依赖）。
   - 路由层启动时 `ce_shard_router_refresh()` 全量拉一次。
   - 之后 `ce_etcd_watch(cli, "shardmap/dbproxy", cb, r)` 监听变更，回调里构造新表 + 原子换指针。
   - **兜底**：即使 Watch 断连，仍按 `CE_SHARD_MAP_CACHE_TTL_MS = 3000` 定期 refresh（与 etcd 客户端已有缓存 TTL 语义对齐），避免长时间用陈旧表。
   - **epoch 单调校验**：拉到的 epoch ≤ 当前则丢弃（防止 etcd 多实例读到旧副本导致回退）。

**错投防护（元数据不一致时的最后一道闸）**：Game 侧发请求时在包头携带 `epoch` 与 `slot`。DBProxy 实例校验"该 slot 是否归我"：
- 归我且 STABLE → 正常处理；
- 不归我 → 回 `DB_ERROR` 且 `err_code = CE_ERR_SHARD_MOVED`，payload 带正确的 `shard_id` + 最新 `epoch`（**MOVED 重定向**，Game 收到后立即 refresh 并重投，类似 Redis Cluster 的 MOVED）；
- 归我但 MIGRATING → 回 `CE_ERR_SHARD_ASK`，要求写请求同时投目标分片。

**协议消息（遵守 C-3 共用端口 + C-4 单一权威来源）**：不新开端口，全部复用 `CE_DBPROXY_DEFAULT_PORT = 9700`，在 `CeDbproxyMsgType` 空闲段扩展，且**这些常量定义在 `src_c/server/ce_game_protocol.h`**（唯一权威来源），`ce_dbproxy.h` 与分片头都从那里取，不复制：

| 消息 | 值 | 方向 | 用途 |
|---|---|---|---|
| `DB_SHARD_MOVED` | 0x06 | Proxy→Game | 错投重定向响应 |
| `DB_SHARD_ASK` | 0x07 | Proxy→Game | 迁移中，要求双投 |
| `DB_SHARD_MIGRATE_BEGIN` | 0x10 | Admin→Proxy | 开始导出指定槽 |
| `DB_SHARD_MIGRATE_CHUNK` | 0x11 | Proxy→Proxy | 槽数据分块传输（≤ 256 KiB/块，受 `CE_DBPROXY_MAX_MSG_SIZE` 约束） |
| `DB_SHARD_MIGRATE_END` | 0x12 | Proxy→Admin | 槽导出完成 + 校验和 |
| `DB_SHARD_MAP_QUERY` | 0x13 | Game→Proxy | 查询实例自认的 epoch/槽归属（调试与对账） |

分片间迁移传输的 `MIGRATE_CHUNK` 走 **`ce_async_io`（io_uring）** 异步收发，禁止阻塞 `send`（约束 C-2）。

### 2.5 跨分片查询与事务处理策略

**总原则：把跨分片需求挡在设计阶段，而不是在运行时用分布式事务硬扛。**

**（a）单玩家操作 —— 100% 单分片。**
`player_id` 是分片键，`_id == player_id`（现状即如此）。玩家存档读写、背包、任务进度全部落单分片，天然原子（MongoDB 单文档写是原子的）。**这覆盖了 95%+ 的 DBProxy 流量**，是分片能成立的根本前提。

**（b）跨分片只读查询（排行榜、GM 查询、全服统计）—— scatter/gather，且不进热路径。**
```c
/* 并发向所有健康分片发同一查询，各自返回 top-K，路由层归并 */
CeResult ce_shard_scatter_query(CeShardRouter* r,
                                uint16_t query_type,
                                const uint8_t* req, uint32_t req_len,
                                CeShardQueryResult* out_results, /* 每分片一个 */
                                int* out_n, int timeout_ms);
```
- 用 `ce_async_io` 并发下发，总耗时 ≈ max(各分片耗时) 而非求和。
- **一致性等级明确降级为"最终一致 + 快照不对齐"**：各分片返回的是各自时刻的数据。排行榜/统计类业务接受此语义；写在 API 注释里。
- **禁止把 scatter/gather 放进玩家登录、存盘等在线热路径**。排行榜由离线/定时任务算好后写入独立的"全局数据分片"（见 c）。

**（c）全局数据（世界数据、公告、活动配置）—— 固定 shard 0，不参与哈希。**
现状已有 `DB_SAVE_WORLD/DB_LOAD_WORLD`。这类数据量小、写频低、需要全服一致视图，直接**钉死在 shard 0**（`slot = 0` 的特权槽，永不迁移）。避免为它设计跨分片一致性。

**（d）真正跨玩家的写事务（交易、邮件带附件、公会资金）—— 不做 2PC，改用 Saga + 事件总线。**
理由：MongoDB 4.0+ 虽支持跨分片事务，但一旦我们的分片是**跨副本集/跨库**的（这是分片的目标），分布式事务的延迟与失败面不可接受，且会把两个分片的可用性耦合成乘积。

方案：**本地事务 + 幂等事件 + 补偿**：
1. 玩家 A（shard1）扣道具 —— 单分片本地原子写，同时在**同一个分片内**写一条 `outbox` 记录（同文档 `$push`，仍是单文档原子，即 transactional outbox 模式）。
2. `ce_eventbus` 投递 `CE_EVT_CROSS_SHARD_MAIL` 事件（§3）。
3. 玩家 B（shard2）消费事件，幂等地加道具（幂等键 = `event_id`）。
4. 失败/超时 → 消费侧不 ACK，Redis Stream `PEL` 保留 → 重投；超过 `max_deliver` 进死信流，触发**补偿事件**回滚 A 侧。

**这就是 2.7 与 2.8 必须一起设计的原因**：分片让跨玩家写变成跨分片，而事件总线是承接它的唯一可行机制。

### 2.6 分片再平衡（Rebalance）流程

触发场景：扩容（加分片）、缩容（减分片）、热点分片拆分。全过程**逐槽推进、可中断、可回滚**。

```
阶段 0  规划（离线）
  ├─ 用一致性哈希环算法（参考 hash_ring.lua 思路的 C 离线工具）计算目标槽分布
  ├─ 产出迁移计划：[(slot, src_shard, dst_shard), ...]，按 slot 升序
  └─ 人工评审 + 落盘到 /chaos/shardmap/dbproxy/plan（不生效，仅记录）

阶段 1  逐槽迁移（在线，一次只处理 一批 ≤ 64 槽）
  ├─ 1.1 写 etcd：slot_state[slot]=MIGRATING, slot_target[slot]=dst, epoch++
  ├─ 1.2 Watch 推送到所有 Game 的路由层（<1s）
  │       ▸ 此后该槽的 写请求 双写 src+dst；读请求 仍走 src
  ├─ 1.3 src 侧全量导出该槽玩家：ce_async_io 分块 DB_SHARD_MIGRATE_CHUNK → dst
  │       ▸ 每块携带 CRC32；dst 侧 upsert 落库（复用 native batch_save）
  ├─ 1.4 增量追赶：导出期间的双写已保证 dst 不落后；对账比较 (count, checksum)
  ├─ 1.5 对账通过 → 写 etcd：slot_state=FROZEN, epoch++（冻结窗口目标 < 200ms）
  │       ▸ Game 侧对该槽请求进入本地重试队列（退避 5ms/10ms/20ms…）
  ├─ 1.6 最终对账 → 写 etcd：slot_owner[slot]=dst, slot_state=STABLE,
  │       slot_target=INVALID, epoch++
  └─ 1.7 Game 侧解冻，重试队列排空；src 侧延迟 24h 后清理旧数据（保留回滚能力）

阶段 2  收尾
  ├─ 全部槽 STABLE → 校验 slot_owner 分布与计划一致
  ├─ 缩容场景：被腾空的分片 ce_etcd_deregister 下线
  └─ 归档迁移报告（每槽耗时、双写量、冻结时长、重试数）
```

**关键设计点**：
- **只有 FROZEN 那一小段窗口影响可用性**，且一次只冻结 ≤ 64 槽 = 全量的 0.39%。因此"再平衡期间可用性"的验收指标可以设得很高（见 §6）。
- **旧数据延迟 24h 清理**，任何阶段都能把 `slot_owner` 改回 src 完成回滚。
- **双写失败处理**：src 成功、dst 失败 → 不返回成功给 Game，记 `CE_LOG_ERROR("SHARD", ...)`，该槽迁移标记为 `FAILED` 并回滚到 STABLE(src)。宁可迁移失败，不可数据分叉。
- **迁移速率限流**：`MIGRATE_CHUNK` 带令牌桶（默认 20 MB/s），防止把 MongoDB 或网卡打满影响在线玩家。

---
## 3. 2.8 事件总线（Redis Stream + hiredis）

### 3.1 目标与决策前提

**目标**：新建 `src_c/eventbus/ce_eventbus.c/h`，提供跨服务异步事件通知，消除"所有操作同步执行"的耦合。

**已定决策（不再论证）**：底层 = **Redis Stream**，客户端 = **hiredis C 客户端**。

选择 Redis Stream 而非 Pub/Sub 的必要性（仅作记录）：Pub/Sub 是 fire-and-forget，订阅者掉线即丢消息，无法满足"跨服邮件不能丢"；Stream 提供**持久化日志 + 消费者组（`XGROUP`）+ 未确认列表（PEL）+ `XAUTOCLAIM` 故障接管**，是唯一能给出至少一次语义的原生结构。

**架构定位**：
```
Game / Router / DBProxy / Cell  ──publish──▶  Redis Stream (ce:evt:<topic>)
                                                     │
                                    ┌────────────────┼────────────────┐
                              XREADGROUP        XREADGROUP       XREADGROUP
                              consumer_g1       consumer_g2      consumer_g3
                              (mail_svc)        (notice_svc)     (stat_svc)
```
每个逻辑订阅方 = 一个**消费者组**（组内多实例负载均摊、组间广播）。这一点很关键：`ce_eventbus` 天然同时支持"广播"（多个组）与"竞争消费"（同组多消费者）。

### 3.2 发布 / 订阅 API 设计（`src_c/eventbus/ce_eventbus.h`）

```c
/* ---- 常量 ---- */
#define CE_EVT_TOPIC_MAX        64      /* topic 名最大长度 */
#define CE_EVT_PAYLOAD_MAX      (64 * 1024)   /* 单事件 payload 上限 64 KiB */
#define CE_EVT_BATCH_MAX        128     /* 一次 XREADGROUP 最多取多少条 */
#define CE_EVT_MAX_DELIVER      5       /* 超过此投递次数进死信 */
#define CE_EVT_STREAM_MAXLEN    1000000 /* XADD MAXLEN ~ 近似裁剪，保留百万条 */
#define CE_EVT_DEFAULT_PORT     6379

/* ---- 事件 ID：Redis Stream 原生 ID（ms-seq），同时作为幂等键 ---- */
typedef struct CeEventId {
    uint64_t ms;    /* 毫秒时间戳部分 */
    uint64_t seq;   /* 同毫秒内序号 */
} CeEventId;

/* ---- 事件结构 ---- */
typedef struct CeEvent {
    CeEventId       id;                       /* Redis 分配，publish 后回填 */
    uint16_t        type;                     /* CeEventType，见 §3.5 */
    uint32_t        payload_len;
    const uint8_t*  payload;                  /* 二进制，不做文本转义 */
    char            topic[CE_EVT_TOPIC_MAX];
    uint64_t        source_node;              /* 发布方 node_id，便于追踪 */
    uint32_t        deliver_count;            /* 第几次投递（≥2 表示重投） */
    uint64_t        trace_id;                 /* 贯穿 RPC + 事件的链路 ID */
} CeEvent;

/* ---- 不透明句柄 ---- */
typedef struct CeEventBus CeEventBus;

/* ---- 配置 ---- */
typedef struct CeEventBusConfig {
    const char* redis_host;
    int         redis_port;
    const char* redis_password;      /* 可为 NULL */
    const char* consumer_group;      /* 本服务的消费者组名，如 "mail_svc" */
    const char* consumer_name;       /* 组内唯一名，建议 "<host>:<pid>" */
    int         connect_timeout_ms;
    int         block_ms;            /* XREADGROUP BLOCK 时长，建议 100 */
    int         claim_idle_ms;       /* XAUTOCLAIM 接管阈值，建议 30000 */
    CeBool      enable_dead_letter;
} CeEventBusConfig;

/* ---- 生命周期 ---- */
CeEventBus* ce_eventbus_create(const CeEventBusConfig* cfg);
void        ce_eventbus_destroy(CeEventBus* bus);
CeBool      ce_eventbus_is_connected(const CeEventBus* bus);

/* ---- 发布 ---- */
/* 同步发布（仅用于非热路径 / 启动期）：内部 XADD，阻塞等回复 */
CeResult ce_eventbus_publish(CeEventBus* bus, const char* topic,
                             uint16_t type,
                             const uint8_t* payload, uint32_t len,
                             CeEventId* out_id);

/* 异步发布（热路径唯一允许的形式）：入本地环形队列，由 io_uring 驱动的
 * ce_eventbus_poll() 批量 flush（Redis pipeline），完成后回调 */
typedef void (*CeEventPublishCb)(CeResult r, const CeEventId* id, void* ud);
CeResult ce_eventbus_publish_async(CeEventBus* bus, const char* topic,
                                   uint16_t type,
                                   const uint8_t* payload, uint32_t len,
                                   CeEventPublishCb cb, void* ud);

/* ---- 订阅 ---- */
typedef enum CeEventAck {
    CE_EVT_ACK   = 0,   /* 处理成功 → XACK */
    CE_EVT_RETRY = 1,   /* 暂时失败 → 不 ACK，留在 PEL 待重投 */
    CE_EVT_DROP  = 2,   /* 永久失败 → XACK + 转死信流 */
} CeEventAck;

typedef CeEventAck (*CeEventHandler)(const CeEvent* evt, void* ud);

/* 订阅某 topic 的某类事件；同一 topic 可注册多个 type 的 handler */
CeResult ce_eventbus_subscribe(CeEventBus* bus, const char* topic,
                               uint16_t type,          /* CE_EVT_TYPE_ANY 表示全收 */
                               CeEventHandler handler, void* ud);
CeResult ce_eventbus_unsubscribe(CeEventBus* bus, const char* topic, uint16_t type);

/* ---- 事件循环驱动（与 ce_async_io 集成，见 §4.3） ---- */
/* 非阻塞：处理已就绪的 Redis 回复、派发 handler、flush 发布队列、
 * 周期性 XAUTOCLAIM。返回本次处理的事件数，<0 为错误。
 * 由宿主进程的主循环每 tick 调用一次。 */
int ce_eventbus_poll(CeEventBus* bus, int max_events);

/* 把 eventbus 的 socket fd 挂到宿主的 ce_async_io 上下文 */
CeResult ce_eventbus_attach_async(CeEventBus* bus, CeAsyncContext* async);

/* ---- 重放 ---- */
/* 从指定 ID 之后重放历史事件（不经消费者组，独立游标，只读） */
CeResult ce_eventbus_replay(CeEventBus* bus, const char* topic,
                            const CeEventId* from, const CeEventId* to,
                            CeEventHandler handler, void* ud,
                            int max_events);

/* ---- 死信 ---- */
CeResult ce_eventbus_dead_letter_list(CeEventBus* bus, const char* topic,
                                      CeEvent* out, int* out_n, int cap);
CeResult ce_eventbus_dead_letter_requeue(CeEventBus* bus, const char* topic,
                                         const CeEventId* id);

/* ---- 统计 ---- */
typedef struct CeEventBusStats {
    uint64_t published;
    uint64_t publish_failed;
    uint64_t consumed;
    uint64_t acked;
    uint64_t retried;
    uint64_t dropped;
    uint64_t duplicates_filtered;   /* 幂等去重命中数 */
    uint64_t claimed;               /* XAUTOCLAIM 接管数 */
    uint64_t pel_size;              /* 当前未确认数（周期采样） */
    uint64_t latency_p99_us;        /* 投递延迟 P99（发布→handler 入口） */
} CeEventBusStats;
CeResult ce_eventbus_get_stats(const CeEventBus* bus, CeEventBusStats* out);
```

**日志示例（遵守 C-6）**：
```c
CE_LOG_ERROR("EVENTBUS", "XADD failed: topic=%s type=%u err=%s",
             topic, (unsigned)type, err_str);
CE_LOG_WARN("EVENTBUS", "event to dead-letter: id=%llu-%llu type=%u deliver=%u",
            (unsigned long long)evt->id.ms, (unsigned long long)evt->id.seq,
            (unsigned)evt->type, (unsigned)evt->deliver_count);
```

### 3.3 消费者组与 ACK 机制

**Redis 命令映射**（全部通过 hiredis 发出，禁止 shell 调 `redis-cli`）：

| 动作 | Redis 命令 | 说明 |
|---|---|---|
| 建组 | `XGROUP CREATE ce:evt:<topic> <group> $ MKSTREAM` | `MKSTREAM` 保证流不存在也能建；已存在时忽略 `BUSYGROUP` 错误 |
| 拉取新消息 | `XREADGROUP GROUP <group> <consumer> COUNT 128 BLOCK 100 STREAMS ce:evt:<topic> >` | `>` = 只取本组未投递过的 |
| 拉取自己的未确认 | `XREADGROUP ... STREAMS ce:evt:<topic> 0` | 进程重启后先补处理自己 PEL 中的遗留 |
| 确认 | `XACK ce:evt:<topic> <group> <id...>` | 批量 ACK，一次最多 128 个 ID |
| 接管僵死消费者 | `XAUTOCLAIM ce:evt:<topic> <group> <me> 30000 0 COUNT 64` | 每 10s 执行一次，把 idle > 30s 的 PEL 条目转给自己 |
| 裁剪 | `XADD ce:evt:<topic> MAXLEN ~ 1000000 * ...` | 发布时顺带近似裁剪，避免无界增长 |
| 死信 | `XADD ce:evt:<topic>:dlq * ...` + `XACK` 原流 | 死信是独立流，可人工 `requeue` |

**ACK 时机原则：处理完成后才 ACK（at-least-once），绝不先 ACK 后处理。**
handler 返回 `CE_EVT_ACK` → 加入待 ACK 批次；返回 `CE_EVT_RETRY` → 不 ACK，条目留在 PEL，下次 `XREADGROUP ... 0` 或 `XAUTOCLAIM` 会重投；返回 `CE_EVT_DROP` 或 `deliver_count > CE_EVT_MAX_DELIVER` → 写死信流后 ACK 原流（防止毒消息永久堵塞）。

**消费者身份**：`consumer_name = "<host>:<pid>"`。进程崩溃重启后 pid 变化 → 旧消费者的 PEL 由 `XAUTOCLAIM` 被同组其他实例（或自己）接管，30s 内恢复。这是**不需要额外故障检测机制**的关键——Redis Stream 自带。

### 3.4 消息持久化、重放与幂等

**持久化**：依赖 Redis 自身。要求生产环境 Redis 开启 **AOF `appendfsync everysec`**（性能与持久性折中）；关键流（跨服邮件、交易）额外要求 Redis 主从 + Sentinel。Stream 通过 `MAXLEN ~ 1000000` 做近似裁剪，按典型 1 KiB/事件估算单流内存约 1 GB 上限，多流总量需在容量规划里核算。

**重放**：`ce_eventbus_replay()` 用 `XRANGE ce:evt:<topic> <from> <to> COUNT n`，**不经过消费者组、不产生 PEL、不 ACK**——它是一个纯只读的历史扫描通道。用途：
- 事故恢复：某消费者逻辑有 bug，修复后从事故起始 ID 重放；
- 新服务冷启动补齐历史（如新增统计服务）；
- 线上问题排查（按 `trace_id` 过滤）。

**幂等处理（至少一次语义的必然要求）**：
1. **幂等键统一为 `CeEventId`**（Redis 原生 `ms-seq`，全局唯一、单调）。业务侧禁止自造幂等键。
2. **两级去重**：
   - **L1 本地布隆/环形缓存**：消费者进程内维护最近 N（默认 100 万）个已处理 `event_id` 的固定大小哈希环（纯 C，开放寻址，无动态分配）。命中即视为重复，直接 ACK 并 `duplicates_filtered++`。开销 O(1)，覆盖绝大多数重投（重投通常发生在 30s 内）。
   - **L2 持久化幂等表**：对**必须严格一次生效**的事件（跨服邮件发放、道具转移），消费者在自己的分片 MongoDB 里维护 `idempotency` 集合，`_id = event_id 字符串`，业务写入与幂等记录**在同一文档或同一单分片操作内完成**（transactional outbox 的对偶：inbox 模式）。已存在则跳过。
3. **业务侧强制要求 handler 幂等**：无法幂等的操作（如"+1 计数"）必须改写成幂等形式（如"设置为 X"或"按 event_id 去重后累加"）。这条写进 `ce_eventbus_subscribe()` 的文档注释。

### 3.5 典型事件类型清单

常量定义在 **`src_c/server/ce_game_protocol.h`**（约束 C-4，唯一权威来源），Game / DBProxy / Router / Cell 全部 include 同一份，**禁止任何一侧另抄一套**。

```c
typedef enum CeEventType {
    CE_EVT_TYPE_ANY               = 0x0000,  /* 仅用于 subscribe 通配 */

    /* ---- 玩家生命周期（topic: "player"）---- */
    CE_EVT_PLAYER_ONLINE          = 0x0101,  /* 玩家上线：player_id, node_id, ts */
    CE_EVT_PLAYER_OFFLINE         = 0x0102,  /* 玩家下线：player_id, 在线时长, 原因 */
    CE_EVT_PLAYER_KICK            = 0x0103,  /* 踢下线指令（GM / 顶号）*/
    CE_EVT_PLAYER_LEVELUP         = 0x0104,  /* 升级（供统计/成就消费）*/

    /* ---- 跨服社交（topic: "social"）---- */
    CE_EVT_CROSS_SHARD_MAIL       = 0x0201,  /* 跨服邮件（可带附件，Saga 关键路径）*/
    CE_EVT_MAIL_ATTACH_CLAIMED    = 0x0202,  /* 附件领取回执（补偿判定依据）*/
    CE_EVT_FRIEND_REQUEST         = 0x0203,
    CE_EVT_GUILD_MEMBER_CHANGE    = 0x0204,
    CE_EVT_CROSS_SHARD_TRADE      = 0x0205,  /* 跨分片交易（Saga）*/
    CE_EVT_SAGA_COMPENSATE        = 0x0206,  /* 补偿事件：回滚已扣除的资源 */

    /* ---- 全局广播（topic: "global"）---- */
    CE_EVT_GLOBAL_NOTICE          = 0x0301,  /* 全局公告：文本 + 展示策略 */
    CE_EVT_SERVER_MAINTENANCE     = 0x0302,  /* 维护通知/倒计时 */
    CE_EVT_WORLD_BOSS_SPAWN       = 0x0303,
    CE_EVT_ACTIVITY_STATE_CHANGE  = 0x0304,  /* 活动开启/结束 */

    /* ---- 集群拓扑（topic: "cluster"）---- */
    CE_EVT_CELL_MIGRATION_DONE    = 0x0401,  /* Cell 迁移完成（2.4 契约，见 §3.7）*/
    CE_EVT_CELL_SPLIT_DONE        = 0x0402,
    CE_EVT_GHOST_SYNC_HINT        = 0x0403,  /* Ghost 批量刷新提示（2.5 契约）*/
    CE_EVT_SHARD_MAP_CHANGED      = 0x0404,  /* 分片映射 epoch 变更（辅助通道）*/
    CE_EVT_SHARD_REBALANCE_DONE   = 0x0405,  /* 某槽再平衡完成 */
    CE_EVT_NODE_JOIN              = 0x0406,
    CE_EVT_NODE_LEAVE             = 0x0407,

    /* ---- 运维 / 审计（topic: "ops"）---- */
    CE_EVT_GM_COMMAND             = 0x0501,
    CE_EVT_ANTI_CHEAT_ALERT       = 0x0502,
    CE_EVT_AUDIT_LOG              = 0x0503,  /* 落冷存储，永不 DROP */
} CeEventType;
```

**Topic 划分原则**：按**消费者集合**而非按业务模块划分。同一 topic 内的事件被同一批消费者组关注，避免消费者被迫过滤大量无关事件。5 个 topic 已足够，不要膨胀（每个 topic 都是一个独立 Stream + 一组 PEL，运维成本线性增长）。

**payload 编码**：二进制紧凑布局，定义在 `ce_game_protocol.h` 中的 `struct CeEvt*Payload`（如 `CeEvtPlayerOnlinePayload{uint64 player_id; uint64 node_id; int64 ts_ms;}`），前置 `uint16 version` 便于演进。**不用 JSON**（解析开销 + 无 schema 约束）。

### 3.6 与 `src_c/rpc/` 同步 RPC 的分工边界

这是本节最重要的规范，用于防止"两套通信机制混用导致架构腐化"。

**判定规则（依次判断，命中即定）**：

| # | 判定条件 | 选择 | 例子 |
|---|---|---|---|
| 1 | 调用方**需要返回值才能继续**（请求-响应语义） | **RPC** | 加载玩家存档、查询实体所在 server、校验道具是否存在 |
| 2 | 调用方**必须知道对端是否成功**且失败要立即回滚 | **RPC** | 扣除玩家货币（同分片内）、Cell 迁移的握手阶段 |
| 3 | 延迟敏感，要求 **P99 < 10ms** | **RPC** | AOI 查询、Ghost 属性同步、跨 Cell 邻居查询 |
| 4 | **一对多广播**，发布方不关心谁在听、听了没有 | **事件总线** | 玩家上线、全局公告、活动开启 |
| 5 | 消费方可能**离线/未启动**，消息必须留存 | **事件总线** | 跨服邮件（目标玩家不在线）、审计日志 |
| 6 | 需要**削峰**（瞬时洪峰，允许延迟处理） | **事件总线** | 开服瞬间的大量上线统计、赛季结算 |
| 7 | 跨分片/跨服的**最终一致写**（Saga 步骤） | **事件总线** | 跨分片交易、跨服道具转移 |
| 8 | 需要**事后重放**做恢复或补算 | **事件总线** | 统计补算、掉档追溯 |

**反模式（明令禁止）**：
- ❌ 用事件总线做请求-响应（发一个事件再订阅一个 `*_reply` 事件）→ 延迟不可控、无超时语义、状态机复杂度爆炸。要响应就用 RPC。
- ❌ 用 RPC 做一对多广播（循环给 N 个节点发 RPC）→ N 次网络往返 + 部分失败难处理。
- ❌ 在 RPC handler 里同步 `ce_eventbus_publish()`（阻塞 XADD）→ 把 Redis 延迟串进 RPC 延迟。热路径**必须**用 `ce_eventbus_publish_async()`。
- ❌ 事件 payload 里放大对象（>64 KiB）→ 放引用（存 MongoDB 后传 `_id`），Stream 只传指针。

**组合模式（推荐）**：**RPC 定成败，事件传扩散**。例：玩家上线 → Gateway 用 RPC 向 Game 请求 `enter_world`（要成败）→ Game 成功后 `publish_async(CE_EVT_PLAYER_ONLINE)`（好友通知、统计、公会提示各自消费）。

### 3.7 与 Phase 2 其他子项的接口契约（不重复设计其内部）

本文**只定义契约，不设计 2.1–2.6 的内部实现**：

1. **与 2.4 Cell 迁移**：Cell 迁移的**控制流走 RPC**（`ce_cell_migration` 内部的握手、序列化传输、确认，由 2.4 spec 定义）。迁移**完成后**，由 2.4 侧调用 `ce_eventbus_publish_async(bus, "cluster", CE_EVT_CELL_MIGRATION_DONE, ...)`，payload 为 `CeEvtCellMigrationDonePayload{uint16 version; uint64 cell_id; uint64 src_node; uint64 dst_node; uint32 entity_count; int64 ts_ms;}`。事件总线**不参与迁移决策，不承载实体数据**。
2. **与 2.3 实体路由表**：Router 的 `entity_id → server_id` 权威存储是 Raft 日志（2.2/2.3 负责）。事件总线仅作为**旁路通知**（`CE_EVT_NODE_JOIN/LEAVE`）供非关键订阅方（监控、统计）使用。**任何组件不得把事件总线当作路由表的一致性来源**——Raft 是唯一权威。
3. **与 2.5 Ghost**：Ghost 的属性同步是延迟敏感的高频路径，**必须走 RPC/直连**（规则 3）。事件总线只承载低频的 `CE_EVT_GHOST_SYNC_HINT`（如"邻居 Cell 拓扑变了，请全量刷一次"）。
4. **与 2.7 分片（本文内部）**：分片映射的权威来源是 **etcd**（§2.4）。`CE_EVT_SHARD_MAP_CHANGED` 只是加速通知的辅助通道，路由层**必须**以 etcd Watch + TTL refresh 为准，不得仅依赖事件。

---
## 4. 第三方依赖接入方案（hiredis）

### 4.1 项目既有依赖接入范式（三种，照抄不发明）

读 `CMakeLists.txt`（顶层）与 `src_c/CMakeLists.txt` 后归纳：

| 依赖 | 探测方式 | 启用宏 | 未命中行为 | 参考位置 |
|---|---|---|---|---|
| **libmongoc** | `find_package(PkgConfig QUIET)` + `pkg_check_modules(MONGOC QUIET mongoc)` | `HAVE_MONGOC=1`（`target_compile_definitions ... PRIVATE`） | 打 `message(STATUS "... NOT found, native driver compiled out")`，源码 `#else` 分支给空实现 | `src_c/dbproxy/CMakeLists.txt` |
| **liburing** | `pkg_check_modules(LIBURING liburing)`（在 `if(LINUX)` 内） | `CHAOS_HAS_IO_URING` | `message(STATUS "io_uring: disabled (liburing not found)")`，回退 `ce_async_posix.c` | `src_c/CMakeLists.txt` L202–210 |
| **libbpf** | `pkg_check_modules(LIBBPF libbpf)` | `CHAOS_HAS_EBPF` | `message(STATUS "eBPF: disabled")` | `src_c/CMakeLists.txt` L146–199 |
| **libcurl** | `pkg_check_modules(CURL QUIET libcurl)`，失败再 `check_include_file(curl/curl.h ...)` + `find_library(CURL_LIB NAMES curl)` 双重回退 | `HAVE_CURL=1` | **从源文件列表里 `list(FILTER ... EXCLUDE REGEX ".*ce_etcd.*\\.c$")` 剔除** | `src_c/rpc/CMakeLists.txt` |

**hiredis 采纳 libmongoc 范式（pkg-config + `HAVE_*` 宏 + `#else` 空实现）**，理由：eventbus 会被多个 target 链接，剔除源文件会导致符号缺失（`ce_etcd` 那种剔除法之所以可行，是因为调用点也有条件编译；eventbus 的调用点分散在 server/game 里，用空实现更安全，且与 `ce_dbproxy_native.c` 完全一致，维护者不需要学第二种模式）。

### 4.2 新增 `src_c/eventbus/CMakeLists.txt`（设计稿，实现阶段落地）

```cmake
# ============================================================
# ChaosEngine 事件总线模块（Redis Stream）
# 纯 C99，产出静态库 libengine_eventbus.a
# ============================================================

# ---- hiredis 检测（条件编译，范式对齐 dbproxy/libmongoc） ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HIREDIS QUIET hiredis)
endif()
# 双重回退（范式对齐 rpc/libcurl）
if(NOT HIREDIS_FOUND)
    include(CheckIncludeFile)
    check_include_file(hiredis/hiredis.h HAVE_HIREDIS_H)
    if(HAVE_HIREDIS_H)
        find_library(HIREDIS_LIB NAMES hiredis)
        if(HIREDIS_LIB)
            set(HIREDIS_FOUND TRUE)
            set(HIREDIS_LIBRARIES "${HIREDIS_LIB}")
        endif()
    endif()
endif()

file(GLOB EVENTBUS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(engine_eventbus STATIC ${EVENTBUS_SOURCES})

target_include_directories(engine_eventbus PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)

if(HIREDIS_FOUND)
    message(STATUS "EventBus: hiredis found, enabling Redis Stream event bus")
    target_compile_definitions(engine_eventbus PRIVATE HAVE_HIREDIS=1)
    target_include_directories(engine_eventbus PRIVATE ${HIREDIS_INCLUDE_DIRS})
    target_link_directories(engine_eventbus PRIVATE ${HIREDIS_LIBRARY_DIRS})
    target_link_libraries(engine_eventbus PRIVATE ${HIREDIS_LIBRARIES})
else()
    message(STATUS "EventBus: hiredis NOT found, event bus compiled out")
endif()

target_link_libraries(engine_eventbus PRIVATE engine_core pthread)

target_compile_options(engine_eventbus PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
)
set_target_properties(engine_eventbus PROPERTIES LINKER_LANGUAGE C)
```

在 `src_c/CMakeLists.txt` 末尾（`add_subdirectory(codec)` 之后）追加：

```cmake
# ============================================================
# 事件总线模块 (Redis Stream 发布/订阅)
# ============================================================
add_subdirectory(eventbus)
```

并把 `engine_eventbus` 加入 `chaos_server` 的链接列表（现为 `engine_core engine_server engine_admin_ipc engine_sync engine_dbproxy engine_save`）。

**分片路由层不需要新 CMake 改动**：`ce_dbproxy_shard.c/h` 落在 `src_c/dbproxy/` 下，被现有 `file(GLOB DBPROXY_SOURCES "*.c")` 自动收进 `engine_dbproxy`。但需要给 `engine_dbproxy` 增加 `target_link_libraries(... engine_rpc)`（因为要用 `ce_etcd_registry`）——注意 `engine_rpc` 的 etcd 部分依赖 libcurl 且已有条件编译，因此 `ce_dbproxy_shard.c` 中所有 etcd 调用需包在 `#ifdef HAVE_CURL` 内，未命中时退化为**读本地静态配置文件的固定槽表**（单分片模式，等价于现状）。

### 4.3 hiredis 网络层与 io_uring 的协作（约束 C-2 的关键设计）

**问题**：hiredis 默认同步 API（`redisConnect` / `redisCommand`）内部用阻塞 `read`/`write`；其异步 API（`redisAsyncContext`）自带 `libevent` / `libev` / `libuv` adapter，但**没有 io_uring adapter**，且我们禁止 epoll。

**方案：使用 hiredis 的"非阻塞 + 手动 buffer 驱动"底层 API，把网络 I/O 完全交给 `ce_async_io`。** hiredis 提供了正好合适的三个原语：

| hiredis 原语 | 用途 |
|---|---|
| `redisReaderCreate()` / `redisReaderFree()` | 独立的 **RESP 协议解析器**，与 socket 完全解耦 |
| `redisReaderFeed(reader, buf, len)` | 把我们从 io_uring 收到的字节喂给解析器 |
| `redisReaderGetReply(reader, &reply)` | 取出完整的 `redisReply*`（不足则返回 `NULL` 且不报错） |
| `redisFormatCommand(&cmd, fmt, ...)` / `redisFormatSdsCommandArgv()` | 把命令**格式化成字节串**，不发送 |

**数据流**：

```
发布侧：
  ce_eventbus_publish_async()
     └─ redisFormatCommandArgv() → 得到 XADD 的 RESP 字节串
        └─ 追加到本模块的 out_buf（环形缓冲，多命令天然形成 pipeline）
  ce_eventbus_poll()
     └─ ce_async_send(async, redis_fd, out_buf, len, tag=EVENTBUS_SEND)   ← io_uring
        └─ ce_async_submit()

接收侧：
  宿主主循环 ce_async_wait() → ce_async_get_event()
     └─ 事件 user_data 标记为 EVENTBUS_RECV
        └─ redisReaderFeed(reader, recv_buf, n)
           └─ while (redisReaderGetReply(reader,&r)==REDIS_OK && r)
                 ├─ 匹配在途命令队列（FIFO，RESP 保证顺序）
                 ├─ XADD 回复 → 回填 CeEventId → publish 回调
                 └─ XREADGROUP 回复 → 解析为 CeEvent[] → 派发 handler
        └─ 重新投递 ce_async_recv() 续读
```

**要点**：
1. **hiredis 只当协议编解码库用，一行 socket 代码都不走它的**。这样 io_uring 独占所有网络 I/O，符合 C-2，且天然跨平台——Windows 下 `ce_async_io` 走 IOCP，hiredis 的 reader 是纯内存操作，无平台差异。
2. **连接建立**：自己 `socket()` + `O_NONBLOCK` + `connect()`，用 `ce_async_io` 等可写；**不用 `redisConnectNonBlock`**（它会持有 fd 并管理自己的 buffer，容易双重管理）。若确需复用，只取其 `redisContext->fd` 后立刻把 `c->reader` 拿走自管。
3. **`BLOCK` 参数的处理**：`XREADGROUP ... BLOCK 100` 是**服务端阻塞**，客户端 socket 仍是非阻塞的——Redis 会在 100ms 内无消息时返回 nil。这与 io_uring 完美兼容（不需要客户端阻塞等待）。设 `block_ms = 100` 是延迟与空转 CPU 的折中。
4. **两条连接**：发布用一条、消费用一条（因为消费连接长期挂在 `BLOCK` 上，会阻塞后续 pipeline 命令）。`XAUTOCLAIM` 走发布连接。
5. **`redisReply` 的内存**：`freeReplyObject()` 必须在 handler 返回后调用；handler 收到的 `CeEvent.payload` 是**指向 reply 内部内存的借用指针**，生命周期仅限 handler 内——写进头文件注释，避免使用方悬垂引用。
6. **降级路径**：`HAVE_HIREDIS` 未定义时，`ce_eventbus_*` 全部返回 `CE_ERR` 并打一次 `CE_LOG_WARN("EVENTBUS", "hiredis not available, event bus compiled out")`（照抄 `ce_dbproxy_native.c` 的 `#else` 分支写法）。业务侧必须容忍事件总线不可用（degraded 模式：跨服邮件排队到本地 MongoDB，Redis 恢复后补投）。

### 4.4 CI 环境安装（对齐 `.github/workflows/ci.yml` 现有写法）

现有 4 个 job 的"安装依赖"步骤格式统一为：
```yaml
      - name: 安装依赖
        run: |
          sudo apt-get update -qq
          sudo apt-get install -y -qq \
            cmake build-essential \
            liblua5.4-dev liburing-dev libbpf-dev \
            clang llvm luarocks xvfb xauth \
            libvulkan-dev libx11-dev \
            flatbuffers-compiler libcurl4-openssl-dev
```

**改动方式：在已有的三处依赖清单里各加两个包，不新增 step、不新增 job（约束 C-5）。**

```diff
             flatbuffers-compiler libcurl4-openssl-dev \
+            libhiredis-dev libmongoc-dev
```

- `libhiredis-dev` — ubuntu-latest 官方仓库有（hiredis 1.x），满足 §4.3 所需的 `redisReader*` / `redisFormatCommandArgv` API（这些自 hiredis 0.13 起稳定）。
- `libmongoc-dev` — **顺带补上**：当前 CI 未安装它，意味着 `HAVE_MONGOC` 在 CI 里始终为假，`ce_dbproxy_native.c` 走的是空实现分支，**Phase 1 的 libmongoc 代码其实没有被 CI 编译验证过**。做 2.7 之前必须补上，否则分片代码的编译错误无法在 CI 暴露。

需要改的 job：`编译 & 测试 (Debug)`、`Release 编译`、`内存检查 (Valgrind)`。`Gateway 集成测试` job 只编 `chaos_gateway`，可不改（但为保持依赖清单一致，建议同步加，代价是几秒安装时间）。`Lua 语法检查` job 不涉及编译，不改。

**Redis 服务（集成测试用）**：GitHub Actions 的 `services:` 容器方式最省事，只加在 `编译 & 测试 (Debug)` 这一个 job 上：
```yaml
    services:
      redis:
        image: redis:7-alpine
        ports: ['6379:6379']
        options: >-
          --health-cmd "redis-cli ping"
          --health-interval 5s --health-timeout 3s --health-retries 5
```
这样 `ctest` 里的 eventbus 集成测试可连 `127.0.0.1:6379`。**若不希望引入 service 容器**，退化方案：eventbus 测试检测 `getenv("CE_TEST_REDIS")`，未设置则 `ctest` 跳过（`SKIP_RETURN_CODE`），只保留纯内存的单元测试（RESP 编解码、幂等环、槽哈希分布）。MVP 采纳 **service 容器方案**，因为跨服邮件的至少一次语义必须有真 Redis 才能验。

MongoDB 同理需要 service 容器（`mongo:7`）才能测分片写入吞吐；MVP 阶段 CI 只跑**分片路由层的纯内存测试**（不连 MongoDB），真实吞吐验证放在本地/预发压测（§6.4），避免把 CI 时长（现 20 分钟上限）撑爆。

---

## 5. MVP 范围

**MVP 目标：用最小改动证明"分片可行 + 事件可靠"，不追求功能完备。**

### 5.1 纳入 MVP

**2.7 分片**
1. `ce_shard_slot_of()` 固定哈希 + `CeShardMap` 结构 + `slot_owner` 表（`slot_state` 字段保留但 MVP 只用 `STABLE`）。
2. `ce_shard_router_create/destroy/lookup/conn`、`ce_shard_save_player/load_player/batch_save`（按 shard 分桶）。
3. 元数据：etcd 读取 + `ce_etcd_watch` 热更新 + epoch 单调校验 + TTL 兜底 refresh。
4. 错投防护：`DB_SHARD_MOVED` 重定向（服务侧校验 + 客户端自动 refresh 重投）。
5. 全局数据钉死 shard 0。
6. **分片数固定 4，不支持在线再平衡**（离线停机改槽表）。

**2.8 事件总线**
1. `ce_eventbus_create/destroy/publish/publish_async/subscribe/poll/attach_async/get_stats`。
2. 消费者组 + `XACK` + `XREADGROUP ... 0`（重启补 PEL）+ `XAUTOCLAIM`（30s 接管）。
3. L1 本地幂等环（100 万条）。
4. 事件类型：仅 `CE_EVT_PLAYER_ONLINE/OFFLINE`、`CE_EVT_GLOBAL_NOTICE`、`CE_EVT_CELL_MIGRATION_DONE`、`CE_EVT_CROSS_SHARD_MAIL` **五个**（覆盖广播、持久、跨分片三种模式各一）。
5. hiredis + `redisReader` + `ce_async_io` 集成（§4.3）。
6. 死信流 + `ce_eventbus_replay()`。

### 5.2 明确排除（Phase 2 后续 / Phase 3）

- 在线逐槽再平衡（双写 / FROZEN / 对账全流程）—— 设计已在 §2.6 定稿，MVP 不实现。
- 跨分片 Saga 编排器（`CE_EVT_SAGA_COMPENSATE` 的自动补偿状态机）—— MVP 仅预留事件类型，补偿走人工工单。
- L2 持久化幂等表（inbox 模式）—— MVP 只做 L1；跨服邮件先接受"极端场景下可能重复发放"，由客服兜底。
- scatter/gather 跨分片查询 —— MVP 不实现，排行榜继续用现有全局数据方案。
- Redis Cluster / Sentinel 多节点 —— MVP 单 Redis 实例 + AOF。
- 分片级主备自动切换 —— MVP 沿用现有客户端退避重连 + `ce_dbproxy_set_master()`。

---

## 6. 可量化验收标准

> 每一项都标注**接到哪个既有 CI job**（约束 C-5，复用已有构建产物，不新增 job）。

### 6.1 功能正确性 → job `编译 & 测试 (Debug)`（`ctest` 内新增测试用例，复用同一次编译）

| 编号 | 验收项 | 判定标准 |
|---|---|---|
| F-1 | 槽哈希分布均匀性 | `test_shard_hash`：1000 万个连续/随机 `player_id`，16384 槽的每槽计数偏离均值 **< ±3%**；4 分片的每分片占比 **25% ± 1%** |
| F-2 | 哈希稳定性 | `test_shard_hash`：内置 100 组 `(player_id, expected_slot)` 黄金向量，全部命中（防止有人改哈希函数导致全量迁移） |
| F-3 | 路由查表正确 | `test_shard_router`：构造 mock `CeShardMap`，任意 `player_id` 的 `lookup()` 结果与手算一致；`epoch` 回退的更新被拒绝 |
| F-4 | MOVED 重定向 | `test_shard_moved`：故意用旧 epoch 投错分片，客户端在 **1 次重定向内**成功落到正确分片 |
| F-5 | 批量分桶 | `test_shard_batch`：1000 个混合 id 的 `batch_save`，各分片收到的 id 集合并集 == 输入集合，且无交集 |
| F-6 | RESP 编解码 | `test_eventbus_resp`：`redisReaderFeed` 分片喂入（逐字节 / 半包 / 多包粘连）均能正确还原 `XREADGROUP` 回复 |
| F-7 | 幂等去重 | `test_eventbus_idem`：同一 `event_id` 重复投递 100 次，handler 实际执行 **恰好 1 次**，`duplicates_filtered == 99` |
| F-8 | ACK / 重投 | `test_eventbus_ack`（需 Redis service）：handler 返回 `RETRY` 3 次后返回 `ACK`，`XPENDING` 最终为 0，事件被处理 4 次但业务生效 1 次 |
| F-9 | 消费者故障接管 | `test_eventbus_claim`（需 Redis service）：consumer A 取走 10 条不 ACK 后退出，consumer B 在 `claim_idle_ms` 后通过 `XAUTOCLAIM` 全部接管并 ACK |
| F-10 | 死信 | `test_eventbus_dlq`：handler 恒返回 `RETRY`，投递 `CE_EVT_MAX_DELIVER + 1` 次后事件出现在 `:dlq` 流且原流 PEL 为空 |
| F-11 | 重放 | `test_eventbus_replay`：写 1000 条后从第 500 条重放，收到恰好 501 条，且不影响消费者组 PEL |
| F-12 | 降级不崩 | `test_eventbus_disabled`：`HAVE_HIREDIS` 未定义的构建下所有 API 返回 `CE_ERR`，进程不崩溃、无未定义符号 |

### 6.2 内存安全 → job `内存检查 (Valgrind)`

把 `test_shard_router`、`test_shard_hash`、`test_eventbus_resp`、`test_eventbus_idem` 加入该 job 现有的测试列表（现为 `test_math test_memory test_ecs test_aoi test_cell test_network test_net_base`）。

**判定标准**：`valgrind --leak-check=full --error-exitcode=1 --suppressions=../scripts/valgrind.supp` **零泄漏、零非法访问**。
注意：`test_eventbus_resp/idem` 选为纯内存测试正是为了能进 Valgrind（连真 Redis 的测试在 Valgrind 下太慢，不纳入）。

### 6.3 编译健壮性 → job `Release 编译`

`-O2` 下 `engine_dbproxy` 与 `engine_eventbus` **零 warning**（现有编译选项已含 `-Wall -Wextra`）；`Release` 与 `Debug` 均能产出 `bin/chaos_server`。

### 6.4 性能指标 → 本地 / 预发压测（Lua 脚本，约束 C-8），报告归档到 `docs/bench/`

压测脚本形态（**Lua，不用 Python**）：`scripts/bench_shard_write.lua`、`scripts/bench_eventbus_latency.lua`，通过 `chaos_headless` 加载，共享引擎网络层 C 绑定，多协程并发发压，脚本内统计 QPS / P50 / P99。测试结束统一用 `scripts/stop_cluster_server.sh` 关停集群（约束 C-9，禁止 `kill`/`pkill`）。

| 编号 | 指标 | 基线 | 目标 | 测量方法 |
|---|---|---|---|---|
| P-1 | **分片写入吞吐提升倍数** | 单 DBProxy 稳定写入 QPS（记为 `Q1`） | 4 分片达到 **≥ 3.4 × Q1**（即 ≥ 85% 线性扩展率） | `bench_shard_write.lua`：256 协程，随机 `player_id`（覆盖全 16384 槽），1 KiB payload，持续 5 分钟，取稳态 QPS；分别跑 1/2/4 分片配置 |
| P-2 | 分片路由层自身开销 | — | `ce_shard_router_lookup()` **P99 < 200ns**；引入路由层后单分片 QPS 相比 Phase 1 直连下降 **< 3%** | 微基准 + 1 分片对照组 |
| P-3 | 批量写分桶效率 | — | 1000 条混合 id 的 `batch_save` 总耗时 **≤ 1.3 ×** 单分片 1000 条 `batch_save` 耗时（并发发送，非串行） | `bench_shard_write.lua` 批量模式 |
| P-4 | **事件投递延迟 P99** | — | 发布方 `publish_async` 调用 → 订阅方 handler 入口，**P99 < 50ms**，P50 < 10ms（单机 Redis，同机房，1 万 events/s，1 KiB payload） | `bench_eventbus_latency.lua`：payload 内嵌发布时刻 `ts_us`，handler 侧算差值，脚本统计 P50/P99 |
| P-5 | 事件发布吞吐 | — | 单进程 `publish_async` **≥ 5 万 events/s**（pipeline 生效），且发布线程 CPU 占用 < 1 核 | 同上 |
| P-6 | 事件不丢 | — | 发布 100 万条，消费方累计 ACK 的**去重后**事件数 == 100 万，`dropped == 0` | 长跑测试，中途 `stop_cluster_server.sh` 重启一个消费者实例，验证 `XAUTOCLAIM` 补齐 |
| P-7 | **分片再平衡期间可用性**（设计目标，MVP 不实现在线再平衡，指标供后续验收） | — | 迁移全程写请求成功率 **≥ 99.9%**（含客户端自动重试）；单批（≤ 64 槽）`FROZEN` 窗口 **< 200ms**；受影响玩家请求的额外延迟 P99 **< 500ms**；**零掉档**（迁移前后每槽 `(count, checksum)` 完全一致） | 4→5 分片扩容演练，边迁移边跑 `bench_shard_write.lua` |
| P-8 | 故障域收敛 | 单 DBProxy 挂 = 100% 玩家受影响 | 4 分片下单分片挂 = **25% ± 1%** 玩家受影响，其余分片 QPS 不下降 | 故障注入：用 `stop_cluster_server.sh` 停一个分片实例，观察其余分片指标 |

---
## 7. 落地路线图（建议 5 周，2.7 三周 + 2.8 两周，可并行）

| 周 | 内容 | 交付提交（格式遵守 C-7：`[type](scope): description`） |
|---|---|---|
| W1 | hiredis / libmongoc 接入 CMake + CI 依赖清单 + 空实现降级路径 | `[feat](engine): 接入 hiredis 与 libmongoc 到 CMake 构建与 CI 依赖` |
| W1 | 槽哈希 + `CeShardMap` 结构 + 黄金向量测试 | `[feat](server): 新增 DBProxy 虚拟槽位哈希与分片映射表结构` |
| W2 | 分片路由层（etcd 读取 + Watch 热更新 + RCU 双缓冲 + lookup） | `[feat](server): 实现 DBProxy 分片路由层与 etcd 元数据同步` |
| W2 | 协议常量入 `ce_game_protocol.h` + MOVED 重定向 | `[feat](network): 新增 DB_SHARD_* 分片协议消息与 MOVED 重定向` |
| W3 | 分片数据面 API（save/load/batch 分桶）+ 全局数据钉 shard 0 | `[feat](server): DBProxy 分片数据面 API 与批量分桶` |
| W3 | 分片测试进 ctest / Valgrind + Lua 压测脚本 | `[test](server): 新增分片路由测试与 Lua 分片写入压测脚本` |
| W4 | `ce_eventbus` 骨架 + RESP 编解码 + io_uring 集成 | `[feat](network): 新增基于 Redis Stream 的事件总线（hiredis + io_uring）` |
| W4 | 消费者组 / ACK / XAUTOCLAIM / 死信 / 重放 | `[feat](network): 事件总线消费者组、ACK 与死信重放机制` |
| W5 | 幂等环 + 事件类型清单 + 接入玩家上下线与全局公告 | `[feat](server): 接入玩家上下线与全局公告事件到事件总线` |
| W5 | 事件总线测试进 ctest / Valgrind + 延迟压测 + 文档定稿 | `[test](network): 事件总线集成测试与投递延迟压测` / `[docs](engine): 补充 Phase 2 分片与事件总线实现说明` |

**依赖关系**：2.7 依赖 Phase 1（1.1 libmongoc 原生驱动，已完成）与 `ce_etcd_registry`（已完成）；2.8 无前置依赖，可与 2.7 完全并行。跨分片 Saga 需要两者都就位，排在 MVP 之后。

---

## 8. 风险分析与回退方案

### 8.1 2.7 分片风险

| # | 风险 | 概率/影响 | 缓解措施 | 回退方案 |
|---|---|---|---|---|
| R-1 | **哈希函数被后续修改，导致全量玩家换分片 → 大面积掉档** | 低 / 灾难 | ① 哈希写成头文件 `static inline` 并加 `/* 永不修改 */` 注释；② F-2 黄金向量测试锁死（改了必然 CI 红）；③ Code Review 检查清单列入此项 | 无需回退——CI 拦截。若已上线才发现，只能按旧哈希做一次全量再平衡（走 §2.6 流程） |
| R-2 | **etcd 槽表与实例实际状态不一致 → 双写或写丢** | 中 / 高 | ① epoch 单调校验；② 服务侧 MOVED 校验作最后闸门（Game 侧错了也写不进去）；③ TTL 兜底 refresh，Watch 断连不致命 | 把槽表改回"全部指向 shard 0"，等价退回单 DBProxy |
| R-3 | **etcd 不可用（或 CI/私有环境无 libcurl）→ 路由层拿不到元数据** | 中 / 中 | ① 本地磁盘缓存上一次成功的槽表（带 epoch），启动时可离线加载；② `#ifdef HAVE_CURL` 未命中时退化为读静态配置文件 | 静态配置文件 `conf/shardmap.bin` 指定固定槽表，完全脱离 etcd 运行 |
| R-4 | **分片间负载倾斜（少数大 R 玩家 payload 特别大）** | 中 / 中 | ① 按 `routed[]` 统计监控每分片实际字节吞吐（不只看请求数）；② 槽表可手工把热槽单独挪走（方案 C 的核心优势） | 手工改槽表，把热槽指向专用分片 |
| R-5 | **再平衡期间双写失败导致数据分叉** | 低 / 灾难 | ① 双写任一失败即不回成功、该槽迁移标记 FAILED 并回滚；② 源数据延迟 24h 清理；③ 每槽 `(count, checksum)` 强制对账 | 改 `slot_owner` 回源分片，`epoch++`；源数据未删，秒级回退 |
| R-6 | **引入路由层后单分片性能反而下降** | 低 / 中 | P-2 指标卡死 < 3%；RCU 无锁读，热路径零分配 | `CE_SHARD_DISABLE=1` 环境变量/编译宏 → `lookup()` 恒返回 0，退化为单 DBProxy 直连 |
| R-7 | **CI 从未编译过 libmongoc 分支，分片代码带编译错误上线** | 高（现状即如此） / 高 | §4.4 明确在三个 job 的依赖清单里补 `libmongoc-dev`，作为 W1 第一件事 | 无 |
| R-8 | 全局数据（world）钉死 shard 0 成为新热点 | 低 / 低 | world 数据写频极低；必要时加进程内读缓存 | 无需回退 |

### 8.2 2.8 事件总线风险

| # | 风险 | 概率/影响 | 缓解措施 | 回退方案 |
|---|---|---|---|---|
| R-9 | **Redis 单点故障 → 事件全断** | 中 / 高 | ① MVP 后接 Sentinel 主从；② 业务侧必须容忍 degraded：跨服邮件先落本地 MongoDB outbox，Redis 恢复后由后台任务补投；③ 广播类事件（公告）丢了可接受 | 关闭事件总线（配置开关），跨服邮件退回"目标玩家上线时主动拉取 outbox"的轮询模式 |
| R-10 | **hiredis 无 io_uring adapter，集成方式复杂/出错** | 中 / 中 | ① 只用 `redisReader*` + `redisFormatCommandArgv`，socket 完全自管（§4.3），这是 hiredis 官方支持的解耦用法；② F-6 用逐字节/半包/粘连三种喂入方式压 RESP 解析器 | 若集成受阻，MVP 临时用**同步 `redisCommand` + 独立发布线程**（不进主循环，不违反主循环的 io_uring 约束），后续再改异步。此为**唯一被允许的临时妥协**，需在代码里留 `TODO` 与 issue 编号 |
| R-11 | **至少一次语义导致重复发放道具** | 中 / 高 | ① L1 幂等环覆盖 30s 内重投（绝大多数）；② 对道具类事件在 Phase 2 后续补 L2 持久幂等表；③ 所有 handler 强制 review 幂等性 | 高价值道具类事件暂不走事件总线，改走同分片 RPC + 本地事务（缩小功能范围而非降低正确性） |
| R-12 | **Stream 无界增长打爆 Redis 内存** | 中 / 高 | ① `XADD MAXLEN ~ 1000000` 发布时顺带裁剪；② `pel_size` 纳入监控告警（PEL 堆积说明消费者卡住）；③ payload 上限 64 KiB 硬约束 | 手工 `XTRIM` + 临时下调 `MAXLEN`；极端情况删流重建（丢历史，保可用） |
| R-13 | **毒消息（handler 恒失败）堵塞整组消费** | 中 / 中 | `CE_EVT_MAX_DELIVER = 5` 后强制进死信并 ACK 原流；死信可人工 `requeue` | 直接 `XACK` 该消息跳过，人工处理 |
| R-14 | **有人用事件总线做请求-响应，架构腐化** | 中 / 中 | §3.6 分工表写进 review checklist；`ce_eventbus.h` 头部注释直接列出四条反模式 | Review 拦截 |
| R-15 | **`freeReplyObject` 时机错误导致 handler 内悬垂指针** | 中 / 高 | 头文件明确声明 `CeEvent.payload` 生命周期仅限 handler 内；F-7/F-12 进 Valgrind job | 无 |
| R-16 | CI 引入 Redis service 容器导致流水线变慢/不稳 | 中 / 低 | 只加在 `编译 & 测试 (Debug)` 一个 job；带 healthcheck；不可用时测试按 `CE_TEST_REDIS` 未设置而跳过 | 去掉 service 容器，只保留纯内存单元测试（F-6/F-7/F-10/F-11/F-12 仍可跑） |

### 8.3 整体回退决策树

```
分片出问题？
 ├─ 路由层可疑 → 设 CE_SHARD_DISABLE=1，退回单 DBProxy 直连（R-6）
 ├─ 元数据可疑 → 槽表全指向 shard 0（R-2），或加载 conf/shardmap.bin（R-3）
 └─ 迁移中出问题 → slot_owner 改回源分片 + epoch++，源数据 24h 未删（R-5）

事件总线出问题？
 ├─ Redis 挂 → 配置开关关闭事件总线，业务走 outbox 轮询（R-9）
 ├─ 重复发放 → 高价值事件退回同分片 RPC + 本地事务（R-11）
 └─ 消费卡死 → 死信流 + 人工 XACK 跳过（R-13）

任何回退操作涉及重启服务，统一用 scripts/stop_cluster_server.sh（约束 C-9）
```

**回退演练要求**：上线前必须在预发环境实际演练 R-2（槽表回退）、R-6（`CE_SHARD_DISABLE`）、R-9（关闭事件总线）三条路径，并记录演练耗时。未演练过的回退方案视为不存在。

---

## 9. 附录

### 9.1 术语表

| 术语 | 含义 |
|---|---|
| 槽（slot） | 虚拟分片单元，固定 16384 个，`slot = ce_shard_slot_of(player_id)` |
| 分片（shard） | 一个 DBProxy 实例（含 primary/backup）+ 其后端 MongoDB，管理若干槽 |
| epoch | 槽表版本号，单调递增；用于拒绝陈旧元数据 |
| MOVED / ASK | 服务侧对错投请求的重定向响应（借用 Redis Cluster 概念） |
| PEL | Pending Entries List，Redis Stream 消费者组的未确认消息列表 |
| Saga | 用一系列本地事务 + 补偿事务替代分布式事务的模式 |
| outbox / inbox | 发布方/消费方与业务写在同一原子单元内记录事件/幂等键的模式 |
| 死信（DLQ） | 超过最大投递次数的事件被转入的独立 Stream |

### 9.2 新增文件清单（实现阶段）

```
src_c/dbproxy/ce_dbproxy_shard.h        分片路由层接口（新增）
src_c/dbproxy/ce_dbproxy_shard.c        分片路由层实现（新增）
src_c/eventbus/ce_eventbus.h            事件总线接口（新增）
src_c/eventbus/ce_eventbus.c            事件总线实现（新增）
src_c/eventbus/CMakeLists.txt           事件总线构建（新增）
src_c/server/ce_game_protocol.h         追加 DB_SHARD_* 与 CeEventType（修改，唯一权威来源）
src_c/dbproxy/ce_dbproxy_native.h/.c    追加多集合支持 coll_name 可配（修改）
src_c/CMakeLists.txt                    add_subdirectory(eventbus) + 链接（修改）
.github/workflows/ci.yml                依赖清单补 libhiredis-dev / libmongoc-dev + Redis service（修改）
scripts/bench_shard_write.lua           分片写入压测（新增，Lua）
scripts/bench_eventbus_latency.lua      事件延迟压测（新增，Lua）
docs/bench/phase2-shard-eventbus.md     压测报告（新增）
```

**明确不新增**：任何 Lua 运行时逻辑（压测脚本除外）、任何独立 CI job、任何新监听端口、任何第二份协议常量定义。

### 9.3 评审检查清单（Review Checklist）

- [ ] 分片路由与事件总线是否全部为纯 C99，无 Lua 运行时依赖？（C-1）
- [ ] 是否出现 `epoll_*` 调用？hiredis 是否只用作 RESP 编解码？（C-2）
- [ ] 是否新增了监听端口？`DB_SHARD_*` 是否复用 9700？（C-3）
- [ ] 协议常量与事件类型是否只在 `ce_game_protocol.h` 定义一份？（C-4）
- [ ] 测试是否全部接入既有 5 个 job，未新增 job？（C-5）
- [ ] 所有 `CE_LOG_*` 首参是否为 tag 字符串（`"SHARD"` / `"EVENTBUS"` / `"DBPROXY"`）？（C-6）
- [ ] 提交信息是否符合 `[type](scope): description`？（C-7）
- [ ] 压测脚本是否为 Lua、Headless、多协程、含 P50/P99？（C-8）
- [ ] 文档/脚本中关停服务是否统一用 `scripts/stop_cluster_server.sh`？（C-9）
- [ ] 哈希函数黄金向量测试是否存在且通过？（R-1）
- [ ] 所有事件 handler 是否已论证幂等性？（R-11）
- [ ] `CeEvent.payload` 生命周期注释是否明确？（R-15）
- [ ] 三条回退路径是否已在预发演练并记录？（§8.3）

---

**文档结束。** 本 spec 为 v0.1 草案，评审通过后进入实现阶段；实现过程中发现的设计偏差须回写本文档并升版本号。
