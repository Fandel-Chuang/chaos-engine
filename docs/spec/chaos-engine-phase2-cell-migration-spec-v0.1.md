# ChaosEngine Phase 2 设计规格：Cell 分布式迁移与 Ghost 同步

- **文档编号**：chaos-engine-phase2-cell-migration-spec-v0.1
- **覆盖范围**：演进计划 Phase 2 中的 **2.4 Cell 迁移协议** / **2.5 Ghost 实体管理** / **2.6 跨 Cell AOI 查询**
- **状态**：Draft（待评审）
- **作者**：zhongfangdao
- **上游依据**：`docs/spec/chaos-engine-evolution-plan-v1.md` 第 288-380 行（Phase 2 章节，2.4/2.5/2.6 三项）
- **关联文档**：`docs/spec/chaos-engine-spec-v0.2.md`、`docs/spec/chaos-engine-replication-spec-v0.1.md`、`docs/spec/chaos-engine-architecture-review-v1.md`
- **依赖**：Phase 1 全部完成（CeEcsWorld 实例化、CeCellManager 实例化、Grid AOI、GameServer 不直连客户端、微服务 RPC/注册发现、DBProxy libmongoc）；Phase 2 的 2.1-2.3（Router 纯 C + Raft + 实体路由表）由另一份 spec 定义，本文只定义**接口契约与消息格式**，不重复设计 Router 内部实现。

---

## 0. 全局工程约束（本 spec 的所有设计必须遵守）

| 约束 | 内容 |
|---|---|
| **纯 C 实现** | Cell 迁移、Ghost 同步、跨 Cell AOI 属于战斗核心与热路径，**必须纯 C99 实现**，坚决拒绝 Lua。Lua 只允许作为胶水层（如运维触发一次手动 split 的命令行入口）和压测机器人。 |
| **异步 I/O** | 迁移传输、Ghost 增量包发送**必须走 io_uring（liburing 2.14）**，禁止 epoll。跨平台通过已有的 `ce_async_io` 抽象层落地，不在本模块内直接调用 `io_uring_*`。 |
| **公共代码单份** | 所有新增消息类型、消息结构体、常量**只能加在 `src_c/server/ce_game_protocol.h`**（协议唯一权威来源），客户端直接 `#include`，禁止客户端/服务端各写一份。 |
| **日志宏** | 签名为 `CE_LOG_ERROR(cat, fmt, ...)`，第一个参数是分类 tag 字符串。本模块使用的 tag：`"CELL"`、`"MIGRATE"`、`"GHOST"`、`"AOI"`。 |
| **CI 集成** | 验证必须接入已有 `.github/workflows/ci.yml` 的 5 个 job（Lua 语法检查 / Gateway 集成测试 / 编译&测试(Debug) / Release 编译 / 内存检查(Valgrind)），**复用已有构建产物，不新增独立 job**。 |
| **压测脚本** | 用 **Lua** 机器人（Headless、共享引擎网络层 C 绑定、多协程并发连接、统计 QPS/P50/P99），不用 Python。 |
| **停服** | 一律用 `scripts/stop_cluster_server.sh`，禁止 `kill` / `pkill`。 |
| **提交格式** | `[type](scope): description`，type ∈ {feat,fix,refactor,docs,perf,test}，scope ∈ {core,ecs,render,network,server,plugin,editor,script,engine}。 |

---

## 1. 现状分析与 bug 复盘

### 1.1 现有数据结构

`src_c/server/ce_server_types.h`（第 45-78 行）定义：

```c
typedef enum CeCellState {
    CE_CELL_ACTIVE = 0, CE_CELL_SPLITTING, CE_CELL_MERGING, CE_CELL_MIGRATING,
} CeCellState;

typedef struct CeCellBounds { float min_x, min_y, max_x, max_y; } CeCellBounds;

typedef struct CeCell {
    CeCellId     id;
    CeCellBounds bounds;
    CeCellState  state;
    int          entity_count;      /* 负载指标 */
    int          max_entities;      /* 分裂阈值 */
    int          min_entities;      /* 合并阈值 */
    int          process_id;        /* 进程分配，-1 = 未分配 */
    CeCellId     neighbors[8];
    int          neighbor_count;
} CeCell;
```

**关键观察 A：`CeCell` 结构体里根本没有实体列表。** 它只有一个 `int entity_count` 计数器。实体的空间归属完全由全局 AOI（`ce_aoi_grid.c` 的 `CeAoiGrid`，内部 `CeGridEntity` + `entity_id → node` 哈希表）持有。也就是说 **Cell 层与实体集合之间没有任何反向索引**，这是后续所有 bug 的结构性根因。

**关键观察 B：Cell 索引寻址方式是"行列公式"而非"空间树"。** `mgr_pos_to_cell_index()`（`ce_cell_manager.c` 第 70-86 行）按 `row * cells_per_row + col` 计算索引，`mgr_build_neighbors()`（第 88-108 行）也按 `cell_index / cells_per_row` 反推行列。这意味着**规则网格假设被硬编码进了寻址与邻居构建**，而 split 会产生四分之一尺寸的子 Cell，破坏这个假设（详见 1.3）。

### 1.2 严重 bug 定位：`ce_cell_mgr_split()` 实体数据丢失

**文件**：`src_c/server/ce_cell_manager.c`，函数 `ce_cell_mgr_split()`，第 294-354 行。

关键代码路径（原文摘录，行号精确）：

```c
294  CeResult ce_cell_mgr_split(CeCellManager* mgr, CeCellId cell_id) {
...
306      /* 四等分边界 */
307      float mid_x = (cell->bounds.min_x + cell->bounds.max_x) * 0.5f;
308      float mid_y = (cell->bounds.min_y + cell->bounds.max_y) * 0.5f;
...
330      /* 原 Cell 变为第一个子 Cell */
331      cell->bounds = sub_bounds[0];
332      cell->entity_count = 0;          /* ← BUG 核心：实体计数被直接清零 */
333      cell->state = CE_CELL_ACTIVE;
334
335      /* 创建 3 个新 Cell */
336      for (int i = 1; i < 4; i++) {
337          CeCell* sub = &mgr->cells[mgr->cell_count++];
...
344          sub->entity_count  = 0;      /* ← 3 个新子 Cell 也是 0 */
345      }
...
352      CE_LOG_INFO("CELL", "Cell %u split into 4 sub-cells", cell_id);
353      return CE_OK;                    /* ← 返回成功，无任何实体重分配 */
354  }
```

**Bug 描述**：函数在第 307-308 行算出四等分中线、第 311-316 行构造 4 个子 Cell 边界之后，在第 331 行把原 Cell 边界缩小为左下象限（面积变为 1/4），并在**第 332 行直接把 `entity_count` 置 0**；第 336-345 行创建的 3 个新子 Cell 的 `entity_count` 同样是 0。全过程**完全没有**：

1. 枚举原 Cell 内的实体（结构上也做不到——见 1.1 关键观察 A，`CeCell` 没有实体列表）；
2. 对每个实体按新的四象限边界重新判定归属 Cell；
3. 把实体迁移（或至少重挂）到对应子 Cell；
4. 更新 AOI 索引、复制系统脏标、Router 路由表。

结果：**原 Cell 内 N 个实体在 Cell 管理层视角下凭空消失**，四个子 Cell 加起来 `entity_count == 0`，而 AOI 里那 N 个 `CeGridEntity` 节点仍然存在。Cell 层的世界模型与 AOI 层的世界模型自此永久分叉。

### 1.3 影响面复盘（逐子系统）

#### (1) 负载均衡与分裂/合并决策彻底失效（直接影响）

`ce_cell_mgr_update()`（第 259-292 行）用 `cell->entity_count > cell->max_entities` 判定分裂、用 `cell->entity_count + neighbor->entity_count < cell->min_entities` 判定合并。split 后四个子 Cell 计数全为 0：

- **永远不会再触发分裂**：即使实际有 5000 个实体挤在一个子 Cell 里，计数是 0（只会被后续 `enter_entity` 慢慢累加，而已在场的实体永远不回来）。
- **立刻满足合并条件**：`0 + 0 < min_entities` 恒真，`ce_cell_mgr_update()` 下一帧就会把刚分裂出来的子 Cell 判定为"低负载待合并"。这会形成 **split → 立即 merge → 负载又超 → split** 的抖动死循环（抑制策略见第 7 章）。

#### (2) AOI 索引：残留但语义错位（严重）

`ce_cell_mgr_enter_entity()`（第 212-225 行）做两件事：`cell->entity_count++` 然后 `ce_aoi_enter()`。`ce_cell_mgr_leave_entity()`（第 227-235 行）只调 `ce_aoi_leave()`，注释里坦白"实体计数由 AOI 管理，Cell 计数为近似值"。

split 时：AOI（`CeAoiGrid`）内部有自己独立的 `cell_w/cell_h` 网格与 `world_to_cell()` 映射，它**完全不知道 Cell 管理器发生了 split**。因此：

- AOI 里的 N 个实体节点**没有丢失**，`ce_aoi_grid_query()` 仍能查到它们——所以从客户端表现上看"实体还在，还能互相看见"，**bug 被掩盖，不会立刻崩溃**，这正是它危险的地方；
- 但 AOI Grid 的 cell 粒度与 Cell 管理器 split 后的子 Cell 边界**不再对齐**。一次 AOI 查询返回的实体集合可能横跨 2-4 个逻辑子 Cell（未来这些子 Cell 分属不同 GameServer 进程），而 `ce_cell_mgr_query_nearby()` 无从区分；
- 一旦子 Cell 被 `ce_cell_mgr_assign_process()` 分配到**不同进程**，本地 AOI 里那些"其实已经不属于本进程"的实体节点就变成**幽灵数据**：本进程会继续把它们广播给客户端，但不再对它们做任何逻辑推进（移动、战斗），客户端看到一堆静止不动的僵尸实体。

#### (3) 复制系统脏标：悬空 owner 与静默停更（严重）

`src_c/replication/`（`ce_replication.c/h`）以 `entity_id` 为键维护脏标与 owner 映射：`ce_repl_mark_dirty(ctx, entity_id, component_id)`、`ce_repl_set_owner(ctx, entity_id, client_id)`；`ce_aoi_set_replication_context()`（`ce_aoi.h` 第 70 行）让 AOI 的 ENTER 事件自动把非 `CE_FLAG_SERVER_ONLY` 字段标脏。

split 不通知复制系统，导致：

- **脏标残留**：`CeReplContext` 里仍持有那 N 个实体的脏字段位图与 owner 记录。若这些实体后续被判定属于其他进程，本进程的 `ce_repl_tick()` 会继续为它们生成并下发同步帧 → **同一实体被两个进程同时广播**，客户端出现位置抖动 / 回拉。
- **`CE_FLAG_PERSIST` 字段双写 DBProxy**：两个进程都认为自己是权威，同一 `player_id` 的存档字段互相覆盖，**造成真实的玩家数据损坏**（不只是显示问题）。
- **反向静默停更**：若实体在迁出侧被清理而迁入侧从未收到它（正是当前 split 的情形——没有任何迁入动作），该实体的脏标再也不会被生成，客户端表现为**属性永久冻结**且无任何 ERROR 日志。

#### (4) Router 路由表：悬空引用（依赖 2.3，当前尚未落地即已注定）

2.3 定义的全局 `entity_id → game_server_id` 路由表由 Router 持有并经 Raft 日志持久化。split 后子 Cell 可能被分配到新进程，但：

- split 路径里**完全没有向 Router 发送任何通知**（`ce_cell_mgr_split()` 全文无 RPC 调用），路由表中这 N 个实体的 `game_server_id` 仍指向 split 前的旧进程；
- 若旧进程随后被缩容下线，路由表里就留下**指向已死进程的悬空引用**，Gateway 按表路由客户端消息 → 全部投递失败 / 超时；
- `ce_cell_mgr_assign_process()`（第 396-404 行）只改本地 `cell->process_id`，**不做任何跨进程或跨服务广播**，Router 与其他 GameServer 对 Cell→进程 的映射认知不一致。

#### (5) `ce_cell_mgr_merge()` 的对称缺陷（中等）

`ce_cell_mgr_merge()`（第 356-392 行）问题不同但同源：

- 第 371-374 行用 min/max 取并集扩边界——**对非相邻或非对齐的两个 Cell 会产生覆盖第三方区域的错误矩形**（没有任何相邻性校验）；
- 第 376 行 `ca->entity_count += cb->entity_count` 只是把两个**本身已不可信**的计数相加；
- 第 380-381 行 `memset(cb, 0, ...)` + `cb->id = CE_INVALID_CELL_ID` 直接抹掉 Cell B，**没有把 B 内实体迁移到 A**，也没有通知 AOI / 复制 / Router。若 B 在别的进程上，B 的实体连同其客户端会话一起被静默遗弃。

#### (6) `ce_cell_mgr_query_nearby()` 名不副实（对应 2.6）

第 413-420 行：

```c
int ce_cell_mgr_query_nearby(CeCellManager* mgr, CeServerEntityId entity_id,
                             CeServerEntityId* buffer, int max_count) {
    if (!mgr || !mgr->initialized) return 0;
    /* 委托给全局 AOI */
    return ce_aoi_query_nearby(entity_id, buffer, max_count);
}
```

它把 `mgr` 参数完全丢弃，直接委托单进程全局 AOI。因此：**跨 Cell 查询能力为零**，站在 Cell 边界的玩家只能看到本进程 AOI 里的实体，看不到邻居 Cell（尤其是分布在其他进程上的）实体。这正是 2.5 Ghost + 2.6 跨 Cell 查询要解决的问题。

#### (7) 邻居关系重建越界风险（附带发现）

`ce_cell_mgr_split()` 第 348-350 行在 split 后对所有 Cell 调 `mgr_build_neighbors()`，而该函数依赖 `cell_index / mgr->cells_per_row` 的规则网格假设。split 追加的 3 个子 Cell 位于 `cell_count` 尾部，其索引反推出的 row/col **与真实空间位置无关**，得到的 `neighbors[8]` 是错误的。同时 `mgr->cells_per_row/col` 在 split 后未更新，`mgr_pos_to_cell_index()` 也**无法寻址到子 Cell**——`ce_cell_mgr_find_by_position()` 对 split 过的区域返回的是错误 Cell。

> **结论**：`entity_count = 0`（第 332 行）是最显眼的症状，但根因是 **CeCell 缺少实体集合 + 规则网格寻址假设 + Cell/AOI/复制/Router 四方无一致性协议**。2.4 必须一并修掉这些，而不是只补一行计数。
---

## 2. 【2.4】Cell 迁移协议

### 2.1 目标与设计决策

**新增文件**：`src_c/server/ce_cell_migration.c` / `ce_cell_migration.h`（纯 C99）。

| 决策项 | 结论 | 理由 |
|---|---|---|
| 迁移期间玩家处理 | **短暂冻结**，目标 **< 200ms**，**不断线** | 双写需要冲突消解与向量时钟，复杂度与出错面远高于收益；200ms 冻结在 MMO 中可被客户端插值/动作缓冲掩盖 |
| 是否双写 | **不做双写**（单一权威） | 避免 1.3(3) 中"两个进程同时广播 / 同时写 DBProxy"的数据损坏类问题 |
| 传输通道 | 复用 Phase 1 的微服务 RPC（`src_c/rpc/`），底层走 `ce_async_io`（io_uring） | 不新造网络栈；协程 RPC 天然适配"发起-等待-确认"的多阶段协议 |
| 权威切换时机 | 迁入侧 AOI 重建完成 **且** Router 确认路由表更新后 | 保证任意时刻只有一个进程是实体权威 |
| 失败语义 | **原子性 all-or-nothing**，任一阶段失败则整批回滚到迁出侧 | 部分迁移会留下比 bug 本身更难排查的分裂状态 |

### 2.2 迁移单位：Batch

迁移以 **Batch（批）** 为单位，而非单个实体：一次 split 产生 3 个"迁出目标"，一次负载再平衡可能搬走上千实体。单实体一次 RPC 会导致 RTT 放大到不可接受。

```c
/* ce_cell_migration.h（节选，仅签名与结构，实现见 .c） */

typedef uint64_t CeMigrationId;

typedef enum CeMigrationKind {
    CE_MIGRATE_SPLIT   = 0,  /* Cell 分裂引发的重分配 */
    CE_MIGRATE_MERGE   = 1,  /* Cell 合并引发的归并 */
    CE_MIGRATE_REBALANCE = 2 /* 负载再平衡，整 Cell 搬迁 */
} CeMigrationKind;

typedef enum CeMigrationState {
    CE_MIG_IDLE = 0,
    CE_MIG_PREPARING,      /* 冻结实体、快照集合 */
    CE_MIG_SERIALIZING,    /* 序列化为迁移包 */
    CE_MIG_TRANSFERRING,   /* 通过 RPC 发送，等待 ACK */
    CE_MIG_AWAIT_APPLY,    /* 迁入侧反序列化 + AOI 重建中 */
    CE_MIG_NOTIFY_ROUTER,  /* 等待 Router 路由表确认 */
    CE_MIG_COMMITTING,     /* 迁出侧清理 + 解冻 + 重放缓冲 */
    CE_MIG_DONE,
    CE_MIG_ROLLING_BACK,
    CE_MIG_FAILED
} CeMigrationState;

typedef struct CeCellMigration CeCellMigration;  /* 不透明上下文 */

CeCellMigration* ce_cell_migration_create(CeCellManager* mgr,
                                          CeAoiGrid* aoi,
                                          CeReplContext* repl,
                                          CeRpcClient* rpc);
void ce_cell_migration_destroy(CeCellMigration* mig);

/* 每帧驱动状态机（非阻塞，推进超时/重试） */
void ce_cell_migration_tick(CeCellMigration* mig, float dt);

/* 发起一次批量迁移 */
CeResult ce_cell_migration_begin(CeCellMigration* mig,
                                 CeMigrationKind kind,
                                 CeCellId src_cell,
                                 CeCellId dst_cell,
                                 int dst_server_id,
                                 const CeServerEntityId* entities,
                                 int entity_count,
                                 CeMigrationId* out_id);

/* 修掉 1.2 的 bug：split 的正确实体重分配入口 */
CeResult ce_cell_migration_split_redistribute(CeCellMigration* mig,
                                              CeCellId cell_id,
                                              CeCellId out_sub_cells[4]);

/* 迁入侧回调（由 RPC 分发器调用） */
CeResult ce_cell_migration_on_import(CeCellMigration* mig,
                                     const uint8_t* pkt, int pkt_len);

CeMigrationState ce_cell_migration_get_state(const CeCellMigration* mig,
                                             CeMigrationId id);
```

### 2.3 五阶段协议

#### 阶段 1：序列化（迁出侧，`CE_MIG_PREPARING` → `CE_MIG_SERIALIZING`）

1. **冻结**：对批内每个实体打 `CE_ENTITY_FROZEN` 标记。冻结语义 = 该实体不再被 ECS 逻辑系统推进（移动/战斗/技能均跳过），其客户端**连接保持不断**，来自它的输入消息进入重放缓冲（见 2.5）。
2. **快照集合**：从 AOI Grid（唯一持有实体位置的地方）取出批内实体的当前坐标，并从 ECS（`CeEcsWorld`）按 Archetype 取出可迁移组件。
3. **序列化布局**（小端固定序 + 组件 TLV，版本号前置以支持滚动升级）：

```
CeMigrationPacket
  ├─ u32 magic          = 0x43454D47 ("CEMG")
  ├─ u16 version        = 1
  ├─ u16 kind           (CeMigrationKind)
  ├─ u64 migration_id
  ├─ u32 src_cell_id
  ├─ u32 dst_cell_id
  ├─ u32 entity_count
  ├─ u32 payload_crc32                 /* 覆盖 entities[] 全部字节 */
  └─ entities[entity_count]
        ├─ u32 entity_id
        ├─ u64 owner_client_id          /* 0 = NPC，无属主 */
        ├─ f32 x, y, z
        ├─ f32 aoi_radius
        ├─ u16 component_count
        └─ components[component_count]
              ├─ u32 component_id       /* 与 ce_repl_register_component 一致 */
              ├─ u32 flags              /* CeReplFlag 位掩码 */
              ├─ u32 byte_len
              └─ u8  data[byte_len]
```

**不序列化的内容**：AOI 邻居列表（迁入侧重建）、复制脏标位图（迁入侧全量置脏一次即可）、Ghost 副本（迁入侧按新边界重新建立）、网络套接字（属于 Gateway，与 GameServer 解耦）。

4. **持久化点**：序列化完成后，把 `CE_FLAG_PERSIST` 字段先 flush 到 DBProxy（一次 `ce_repl_flush()`），作为宕机兜底（见第 7 章）。

#### 阶段 2：传输（`CE_MIG_TRANSFERRING`）

- 经 `src_c/rpc/` 的协程 RPC 发送 `MSG_CELL_MIGRATE_PUSH`，底层 I/O 由 `ce_async_io`（io_uring）完成，**不阻塞主循环**。
- 单包上限 **`CE_MIGRATION_MAX_PKT_BYTES = 256 KiB`**；超限则按实体边界切成多个**分片**，携带 `frag_index / frag_total`，迁入侧全部收齐才进入阶段 3（分片语义仍是 all-or-nothing）。
- 等待迁入侧 `MSG_CELL_MIGRATE_ACK`。超时 **`CE_MIGRATION_XFER_TIMEOUT_MS = 80`**，最多重试 2 次（幂等：迁入侧按 `migration_id` 去重，重复包直接回同一个 ACK）。

#### 阶段 3：反序列化（迁入侧，`CE_MIG_AWAIT_APPLY`）

1. 校验 `magic` / `version` / `payload_crc32`；任一不符 → 回 `MSG_CELL_MIGRATE_ACK{result=CE_ERR}`，**不做任何部分导入**。
2. 校验每个实体坐标确实落在 `dst_cell_id` 的 `bounds` 内；越界实体 → 整批拒绝（防止 split 边界计算 bug 静默扩散）。
3. 在本地 `CeEcsWorld` 创建实体、按 TLV 恢复组件；实体先置 `CE_ENTITY_FROZEN`（此时尚未取得权威）。
4. 恢复 `ce_repl_set_owner(repl, entity_id, owner_client_id)`。

#### 阶段 4：AOI 重建（迁入侧）

1. 对每个实体 `ce_aoi_grid_enter(aoi, entity_id, x, y, z)` —— O(1)/实体。
2. `ce_repl_mark_dirty(repl, entity_id, CE_REPL_ALL_COMPONENTS)`：全量置脏一次，保证迁入后第一帧向 AOI 范围内客户端下发完整状态，客户端无需感知迁移发生。
3. 更新 `dst_cell->entity_count += entity_count`（**这才是 `entity_count` 的正确维护方式**）。
4. 调用 `ce_ghost_mgr_rebuild_for_cell()`（见第 3 章）重建边界 Ghost。
5. 回 `MSG_CELL_MIGRATE_ACK{result=CE_OK}`。

#### 阶段 5：Router 通知（`CE_MIG_NOTIFY_ROUTER` → `CE_MIG_COMMITTING`）

**本 spec 只定义接口契约，Router 内部实现（Raft 日志复制、多数派提交）见 2.1-2.3 的 spec。**

- 迁入侧成为权威的**前置条件**是 Router 确认 `entity_id → game_server_id` 已更新。
- 由**迁入侧**发起 `MSG_ROUTER_ENTITY_RELOCATE`（迁入侧是新权威，由它宣告最符合"谁负责谁登记"）。
- Router 应保证该批更新的**原子性**（同一 Raft 日志条目内提交整批），并回 `MSG_ROUTER_RELOCATE_ACK`。
- 超时 **`CE_MIGRATION_ROUTER_TIMEOUT_MS = 60`**；超时 → 迁入侧丢弃已导入实体并回报失败，迁出侧回滚。
- Router 侧要求（契约）：**幂等**（按 `migration_id` 去重）、**单调**（携带 `epoch`，旧 epoch 的更新被拒绝，防止回滚竞态把路由表改回旧进程）。

收到 ACK 后，迁出侧进入 `CE_MIG_COMMITTING`：

1. `ce_aoi_grid_leave()` 移除批内实体；
2. 从本地 ECS 销毁实体，清除复制系统 owner 与脏标（新增 `ce_repl_forget_entity()` 契约，避免 1.3(3) 的脏标残留）；
3. `src_cell->entity_count -= entity_count`；
4. **重放缓冲**（见 2.5）：把冻结期间收到的客户端消息转发给迁入侧；
5. 通知 Gateway 更新会话→GameServer 映射（`MSG_GATEWAY_SESSION_REBIND`）；
6. 迁入侧解冻实体（清除 `CE_ENTITY_FROZEN`），迁移完成，`CE_MIG_DONE`。

### 2.4 状态机（ASCII 图）

```
                            ce_cell_migration_begin()
                                      │
                                      ▼
                            ┌──────────────────┐
                            │    CE_MIG_IDLE   │
                            └────────┬─────────┘
                                     │ 冻结实体 + 快照集合
                                     ▼
                            ┌──────────────────┐
                            │  CE_MIG_PREPARING│──── 实体集合为空 ───┐
                            └────────┬─────────┘                     │
                                     │ 序列化                        │
                                     ▼                               │
                            ┌──────────────────┐                     │
                            │CE_MIG_SERIALIZING│── 序列化失败 ──┐    │
                            └────────┬─────────┘                │    │
                                     │ RPC push (io_uring)      │    │
                                     ▼                          │    │
                     ┌────────────────────────────┐             │    │
              重试≤2 │   CE_MIG_TRANSFERRING      │             │    │
              ┌──────┤   超时 80ms                │             │    │
              │      └────────┬───────────────┬───┘             │    │
              └───────────────┘               │ ACK(OK)         │    │
                     重试用尽 ──┐             ▼                 │    │
                                │  ┌────────────────────┐       │    │
                                │  │ CE_MIG_AWAIT_APPLY │       │    │
                                │  │ 反序列化+AOI 重建   │       │    │
                                │  └────────┬───────┬───┘       │    │
                                │           │       │ 校验/导入失败
                                │  Apply OK │       └──────┐    │    │
                                │           ▼              │    │    │
                                │  ┌────────────────────┐  │    │    │
                                │  │CE_MIG_NOTIFY_ROUTER│  │    │    │
                                │  │ 超时 60ms          │──┤    │    │
                                │  └────────┬───────────┘  │    │    │
                                │  RelocACK │              │    │    │
                                │           ▼              │    │    │
                                │  ┌────────────────────┐  │    │    │
                                │  │ CE_MIG_COMMITTING  │  │    │    │
                                │  │ 清理+重放+解冻     │  │    │    │
                                │  └────────┬───────────┘  │    │    │
                                │           ▼              │    │    │
                                │  ┌────────────────────┐  │    │    │
                                │  │    CE_MIG_DONE     │  │    │    │
                                │  └────────────────────┘  │    │    │
                                ▼                          ▼    ▼    │
                       ┌──────────────────────────────────────────┐   │
                       │        CE_MIG_ROLLING_BACK               │   │
                       │  1. 通知迁入侧丢弃 (MIGRATE_ABORT)       │   │
                       │  2. 迁出侧解冻实体，恢复 ACTIVE          │   │
                       │  3. 重放缓冲就地消费（不转发）           │   │
                       │  4. Router 无需回滚（从未提交）          │   │
                       └────────┬──────────────────────┬──────────┘   │
                     回滚成功   │                      │ 回滚也失败    │
                                ▼                      ▼              ▼
                       ┌──────────────────┐   ┌──────────────────┐  ┌──────┐
                       │   CE_MIG_IDLE    │   │  CE_MIG_FAILED   │  │ IDLE │
                       │  （可重试）      │   │ 人工介入+告警    │  │      │
                       └──────────────────┘   └──────────────────┘  └──────┘
```

**总冻结时长预算（P99 < 200ms）**：

| 阶段 | 预算 |
|---|---|
| PREPARING（冻结 + 快照） | 10 ms |
| SERIALIZING | 15 ms |
| TRANSFERRING（含 1 次重试余量） | 80 ms |
| AWAIT_APPLY（反序列化 + AOI 重建） | 20 ms |
| NOTIFY_ROUTER（Raft 一轮提交） | 60 ms |
| COMMITTING（清理 + 重放 + 解冻） | 15 ms |
| **合计** | **200 ms** |

超出 `CE_MIGRATION_TOTAL_DEADLINE_MS = 200` 的**全局硬截止**触发回滚，并打 `CE_LOG_ERROR("MIGRATE", ...)`。

### 2.5 迁移期间新到消息的缓冲与重放

冻结期间实体仍可能收到消息（客户端输入、其他实体的伤害/技能 RPC、定时器回调）。设计如下：

- 每个冻结实体挂一个 **FIFO 环形缓冲**，容量 `CE_MIGRATION_REPLAY_CAP = 64` 条 / 实体，复用 `src_c/replication/ce_mailbox.c` 的 mailbox 语义（**不新写一份队列**，遵守"公共代码只写一份"）。
- **分类处理**：
  - *客户端输入*（`MSG_POSITION_UPDATE` 等）→ 缓冲，提交后转发迁入侧重放；
  - *幂等状态查询* → 迁出侧就地应答（读的是快照，不改状态）；
  - *有副作用的跨实体 RPC*（伤害、buff）→ 缓冲；
  - *定时器/AI tick* → **丢弃**（下一帧会自然重新产生，重放反而造成重复扣血）。
- **溢出策略**：缓冲满 → 保留最新（丢最旧）并打 `CE_LOG_WARN("MIGRATE", "replay buffer overflow: entity=%u dropped=%d", ...)`。由于冻结 < 200ms 而客户端上行通常 ≤ 30Hz，64 条对单实体有约 2s 余量，正常不会触发。
- **重放顺序保证**：转发时按缓冲入队序、逐实体串行；迁入侧在**解冻前**完成重放，避免"新消息插在旧消息之前"。
- **回滚路径**：缓冲消息在迁出侧就地消费，客户端完全无感（只感受到 <200ms 卡顿）。

### 2.6 split 场景下实体按新边界重分配的正确算法（修掉 1.2 的 bug）

替代 `ce_cell_mgr_split()` 里"第 332 行 `entity_count = 0`"的正确流程。核心是**先枚举、后分类、再迁移，最后才改边界**。

```
ce_cell_migration_split_redistribute(mig, cell_id, out_sub_cells[4]):

  /* 步骤 0：前置校验 */
  cell = ce_cell_mgr_get(mgr, cell_id)
  if !cell or cell->state != CE_CELL_ACTIVE:  return CE_ERR
  if cell->entity_count < CE_CELL_MIN_SPLIT_ENTITIES:  return CE_ERR  /* 抗抖动 */
  cell->state = CE_CELL_SPLITTING          /* 拒绝并发 split/merge */

  /* 步骤 1：枚举原 Cell 内的实体（bug 的结构性根因在此补齐） */
  /* 用 AOI Grid 的范围查询取代"CeCell 没有实体列表"的缺陷：
     以 Cell 矩形的外接圆做一次查询，再用矩形精确过滤 */
  cx = (bounds.min_x + bounds.max_x) * 0.5
  cy = (bounds.min_y + bounds.max_y) * 0.5
  r  = 0.5 * sqrt(w*w + h*h)                 /* 外接圆半径 */
  n  = ce_aoi_grid_query(aoi, cx, cy, 0, r, tmp_ids, CAP)
  members = [id for id in tmp_ids[0..n) if point_in_bounds(pos(id), cell->bounds)]

  /* 步骤 2：算中线并分类到 4 个象限（O(N)） */
  mid_x = (bounds.min_x + bounds.max_x) * 0.5
  mid_y = (bounds.min_y + bounds.max_y) * 0.5
  for id in members:
      (x, y) = pos(id)
      qi = (x < mid_x ? 0 : 1) + (y < mid_y ? 0 : 2)   /* 0=LB 1=RB 2=LT 3=RT */
      bucket[qi].push(id)
  /* 边界归属规则：半开区间 [min, max)，与 mgr_pos_to_cell_index 的
     "x >= world_max_x 判越界"语义一致，避免实体同时落入两个子 Cell */

  /* 步骤 3：创建 4 个子 Cell（先建后迁，全部先置 SPLITTING） */
  sub[0..3] = create_sub_cells(cell, mid_x, mid_y)   /* 原 Cell 复用为 sub[0] */
  for i in 0..3:  sub[i].entity_count = 0; sub[i].state = CE_CELL_SPLITTING

  /* 步骤 4：进程分配 —— 决定哪些子 Cell 需要真正跨进程迁移 */
  for i in 0..3:
      sub[i].process_id = ce_load_balancer_pick(bucket[i].size)

  /* 步骤 5：逐子 Cell 迁移 */
  for i in 0..3:
      if bucket[i].empty():  continue
      if sub[i].process_id == self_process_id:
          /* 进程内重分配：无需序列化/传输，仅重挂账 */
          for id in bucket[i]:
              sub[i].entity_count++          /* ← 正确维护计数 */
          /* AOI 无需改动（坐标未变），但需刷新 Cell 归属索引 */
          ce_cell_mgr_bind_entities(mgr, sub[i].id, bucket[i])
      else:
          /* 跨进程：走完整五阶段协议 */
          ce_cell_migration_begin(mig, CE_MIGRATE_SPLIT, cell_id, sub[i].id,
                                  sub[i].process_id, bucket[i], bucket[i].size, &mid)
          /* 等待该批 CE_MIG_DONE；任一批失败 → 全局回滚（步骤 6） */

  /* 步骤 6：提交或回滚 */
  if all batches DONE:
      for i in 0..3:  sub[i].state = CE_CELL_ACTIVE
      rebuild_neighbors_spatial(mgr)         /* 用空间重叠判定，不用行列公式 */
      ce_ghost_mgr_rebuild_for_cell(ghost, sub[0..3])
      /* 不变式断言 */
      assert(sub[0].ec + sub[1].ec + sub[2].ec + sub[3].ec == members.size)
      CE_LOG_INFO("CELL", "Cell %u split -> [%u,%u,%u,%u], entities %d -> %d/%d/%d/%d",
                  cell_id, sub[0].id, sub[1].id, sub[2].id, sub[3].id,
                  sub[0].ec, sub[1].ec, sub[2].ec, sub[3].ec)
      return CE_OK
  else:
      rollback_all_batches()
      restore_original_cell(cell)            /* 恢复原 bounds / entity_count / ACTIVE */
      CE_LOG_ERROR("CELL", "Cell %u split rollback, entities preserved=%d",
                   cell_id, members.size)
      return CE_ERR
```

**相对原实现的 4 处关键修正**：

1. **枚举在前**：先从 AOI 取出成员集合，**再**改 `bounds`；原实现第 331 行先改边界，导致此后再也无法用旧边界枚举出成员。
2. **`entity_count` 由实际分配结果累加**，不再硬置 0；并加 `sum(sub[i]) == members.size` 的**零丢失不变式断言**（Debug 构建下 `assert`，Release 下打 `CE_LOG_ERROR("CELL", ...)`）。
3. **AOI / 复制 / Router 三方同步**：跨进程分支走完整五阶段协议，进程内分支刷新 Cell↔实体绑定索引；不再留悬空脏标与悬空路由。
4. **邻居重建改为空间判定**：`rebuild_neighbors_spatial()` 按 `bounds` 矩形是否相邻/接触判定，取代 `cell_index / cells_per_row` 的规则网格假设（修掉 1.3(7)）。

**merge 的对称算法**：先校验两 Cell `bounds` 真实相邻（共享一条边），再把 B 的成员当作一批 `CE_MIGRATE_MERGE` 迁到 A，全部 `CE_MIG_DONE` 后才 `memset` 掉 B。B 内实体一个不少地进入 A，`ca->entity_count` 由实际迁入数累加。

### 2.7 新增协议消息（加在 `src_c/server/ce_game_protocol.h`）

**唯一权威来源**，客户端直接 include。新增枚举值挂在现有 `CeGameMsgType` 之后，服务端内部消息统一使用 `0x02xx`（S2S）段，避免与 C2S（`0x00xx`）/ S2C（`0x80xx`）冲突：

| 消息 | 值 | 方向 | 说明 |
|---|---|---|---|
| `MSG_CELL_MIGRATE_PUSH` | `0x0201` | 迁出→迁入 | 迁移包（含分片字段） |
| `MSG_CELL_MIGRATE_ACK` | `0x0202` | 迁入→迁出 | 导入 + AOI 重建结果 |
| `MSG_CELL_MIGRATE_ABORT` | `0x0203` | 迁出→迁入 | 回滚指令，丢弃已导入实体 |
| `MSG_CELL_MIGRATE_REPLAY` | `0x0204` | 迁出→迁入 | 冻结期缓冲消息重放 |
| `MSG_ROUTER_ENTITY_RELOCATE` | `0x0205` | 迁入→Router | 批量路由表更新（含 epoch） |
| `MSG_ROUTER_RELOCATE_ACK` | `0x0206` | Router→迁入 | 路由表提交确认 |
| `MSG_GATEWAY_SESSION_REBIND` | `0x0207` | GameServer→Gateway | 会话重绑定，保持连接不断 |
| `MSG_GHOST_SYNC_DELTA` | `0x0208` | Cell→邻居 Cell | Ghost 增量同步（见第 3 章） |
| `MSG_GHOST_DESTROY` | `0x0209` | Cell→邻居 Cell | Ghost 批量销毁 |
| `MSG_ENTITY_MIGRATE_HINT` | `0x8003` | 服务器→客户端 | 可选：提示客户端进入插值容忍窗口 |

---

## 3. 【2.5】Ghost 实体管理

### 3.1 目标与决策

**新增文件**：`src_c/server/ce_ghost_manager.c` / `ce_ghost_manager.h`（纯 C99）。

问题（演进计划第 341 行）：无跨进程 AOI，边界实体无法查询邻居 Cell。解法：把**边界实体**的部分属性复制到邻居 Cell，在邻居进程内形成**只读影子副本（Ghost）**。

| 决策项 | 结论 |
|---|---|
| 同步频率 | **10 Hz 增量同步**（每 100ms 一个批包） |
| 额外触发 | **位置变化阈值触发**：位移 ≥ `CE_GHOST_POS_THRESHOLD = 0.5m` 时立即入队，不等 tick |
| Ghost 权限 | **只读**。Ghost 上的组件不可被本地逻辑写入；写入尝试打 `CE_LOG_ERROR("GHOST", ...)` |
| Ghost 是否参与逻辑 | 参与 **AOI 查询与视野广播**；**不参与**移动积分、AI、伤害结算（权威只在主体所在进程） |
| 幽灵带 | Cell 边界向内 `CE_GHOST_BAND = aoi_radius` 宽度的条带内实体成为 Ghost 候选 |

### 3.2 幽灵带（Ghost Band）判定

实体是 Ghost 候选，当且仅当它与所属 Cell 的**任一边界**的距离 < `aoi_radius`（`ce_cell_mgr_get_aoi_radius()`，当前实现为 `cell_width * 0.5f`）。

```
        Cell A                    │                  Cell B
  ┌──────────────────────────┬────┼────┬──────────────────────────┐
  │                          │ 幽灵带  │                          │
  │      纯内部实体          │ ← band → │      纯内部实体          │
  │   （不产生 Ghost）       │  E1 │ E2  │   （不产生 Ghost）       │
  │                          │    │    │                          │
  └──────────────────────────┴────┼────┴──────────────────────────┘
                                  │
   E1 在 A 内、处于幽灵带  →  在 B 中创建 Ghost(E1)
   E2 在 B 内、处于幽灵带  →  在 A 中创建 Ghost(E2)
   band 宽度 = aoi_radius，保证站在边界上的玩家的 AOI 圆内所有实体
   都能在本进程被查到（本地实体 + Ghost）。
```

目标邻居集合来自 `cell->neighbors[8]`（`mgr_build_neighbors()` 重建后为空间正确的邻居）；只向 `bounds` 与实体 AOI 圆有重叠的邻居推送，避免 8 邻居无脑广播（对角邻居往往不必要，可减少约 40% 包量）。

### 3.3 Ghost 生命周期

```
       实体进入幽灵带 / 迁移完成后重建
                    │
                    ▼
        ┌────────────────────────┐
        │  CREATE（全量快照）    │  MSG_GHOST_SYNC_DELTA(op=CREATE)
        │  发送全部可复制组件    │  邻居侧 ce_aoi_grid_enter() 建索引
        └───────────┬────────────┘
                    │
                    ▼
        ┌────────────────────────┐   位移 ≥ 0.5m → 立即入队
        │  UPDATE（增量）        │◄──────────────────────────
        │  10Hz，仅脏字段        │   组件被 mark_dirty → 下个 tick 带走
        └───────────┬────────────┘
                    │
      ┌─────────────┼──────────────┬──────────────────┐
      │ 离开幽灵带  │ 主体实体销毁 │ 主体迁移到别 Cell │  心跳超时
      ▼             ▼              ▼                   ▼
        ┌────────────────────────────────────────────┐
        │  DESTROY  MSG_GHOST_DESTROY（可批量）      │
        │  邻居侧 ce_aoi_grid_leave() + 释放         │
        └────────────────────────────────────────────┘
```

**生命周期细则**：

- **CREATE**：首包必须是全量（不能是增量），否则邻居侧无法拼出完整状态。CREATE 与 UPDATE 用同一消息类型 + `op` 字段区分，减少协议面。
- **UPDATE**：仅携带脏字段（复用复制系统的脏标机制，见 3.5）。若邻居侧收到未知 `entity_id` 的 UPDATE（乱序/丢包），回 `MSG_GHOST_SYNC_DELTA(op=RESYNC_REQ)` 请求全量补齐。
- **DESTROY**：批量下发（一个包携带多个 `entity_id`），避免边界人群散开时的包风暴。
- **心跳超时**：邻居侧对每个 Ghost 记 `last_update_ms`；超过 `CE_GHOST_TTL_MS = 1000`（10 个同步周期）未更新即自动销毁。这是**兜底机制**，用于主体进程宕机时防止 Ghost 永久残留（对应 1.3(2) 的僵尸实体问题）。
- **迁移交互**：主体实体迁移期间处于冻结，其 Ghost **保持最后已知状态不销毁**（不闪现消失）；迁移 `CE_MIG_DONE` 后由迁入侧调 `ce_ghost_mgr_rebuild_for_cell()` 重建，旧 Ghost 由心跳超时或显式 DESTROY 清理。

### 3.4 组件复制策略：复制什么，不复制什么

| 类别 | 组件示例 | 是否复制到 Ghost | 理由 |
|---|---|---|---|
| **必须复制** | Transform（x/y/z、朝向） | ✅ | AOI 查询与客户端渲染的基础 |
| | 移动速度 / 速度向量 | ✅ | 邻居侧客户端做插值预测，否则边界实体卡顿 |
| | 阵营 / 队伍 ID | ✅ | 敌友判定，影响是否进入视野与显示 |
| | 外观（模型、装备外观 ID） | ✅ | 邻居客户端渲染必需；变化频率极低，几乎不占带宽 |
| | 名字 / 等级 / 称号 | ✅ | 头顶信息显示 |
| | 血量百分比（**不是绝对值**） | ✅ | 血条显示只需百分比，用 `u8` 表达可省带宽 |
| | 存活/死亡、隐身、无敌等可见状态位 | ✅ | 影响是否显示、能否被选中 |
| **不复制** | 背包 / 货币 / 邮件 | ❌ | 邻居侧完全用不到，且属敏感数据 |
| | 技能冷却表 / 内部计时器 | ❌ | 只有权威进程做结算 |
| | AI 黑板 / 寻路路径 | ❌ | 权威进程私有 |
| | 仇恨列表 / 战斗日志 | ❌ | 数据量大且仅权威使用 |
| | 所有 `CE_FLAG_SERVER_ONLY` 字段 | ❌ | 定义上不出进程 |
| | 所有 `CE_FLAG_PERSIST` 存档字段 | ❌ | Ghost 只读，绝不能触发 DBProxy 写入（防 1.3(3) 的双写损坏） |
| | 属主客户端连接信息 | ❌ | 属于 Gateway |

判定规则落到代码上：**Ghost 只复制带 `CE_FLAG_AOI_BROADCAST` 且不带 `CE_FLAG_SERVER_ONLY` 的字段**，与"客户端能看到什么"的口径完全一致，因此**不需要引入第二套字段白名单**（遵守"公共代码只写一份"）。

### 3.5 与复制系统 `src_c/replication/` 的关系

Ghost 管理器**复用**复制系统，不另造机制：

| 复用点 | 说明 |
|---|---|
| 字段注册表 | 直接读 `ce_repl_register_component()` 注册的 `CeReplField` 元数据（name/type/flags），无需 Ghost 专用 schema |
| 脏标 | `ce_repl_mark_dirty()` / `ce_repl_mark_field_dirty()` 打的脏标被 Ghost 管理器**旁路消费**：`ce_repl_tick()` 面向"客户端"，Ghost tick 面向"邻居 Cell 进程"，二者共享同一份脏位图 |
| 序列化 | 复用复制系统按 `CeReplFieldType` 的编解码路径（`CE_REPL_TYPE_VEC3` / `F32` / `U8` 等），Ghost 不定义新的字段编码 |
| 传输 | Ghost 走 `src_c/rpc/`（GameServer↔GameServer），复制系统走 Gateway；二者**通道不同、载荷编码相同** |

**方向性差异（必须明确）**：复制系统是 **server → client** 的下行；Ghost 是 **server → server** 的横向。因此：

- Ghost 到达邻居侧后，**会再次进入邻居侧的复制系统**，作为普通实体被广播给邻居侧客户端。这是 Ghost 存在的意义。
- 但邻居侧**绝不能**为 Ghost 打 `CE_FLAG_PERSIST` 脏标（会双写 DBProxy）。实现上：导入 Ghost 时用 `ce_repl_mark_dirty()` 的一个新变体 `ce_repl_mark_dirty_ex(ctx, id, comp, flag_mask)`，`flag_mask` 显式排除 `CE_FLAG_PERSIST`。

### 3.6 带宽估算

**Ghost UPDATE 增量包（典型）**：

| 字段 | 编码 | 字节 |
|---|---|---|
| 包头（total_len + msg_type） | — | 6（整包一次，非每实体） |
| entity_id | u32 | 4 |
| dirty_mask | u16 | 2 |
| 位置 x,y,z | 3 × f32 | 12 |
| 朝向 yaw | f32 量化为 u16 | 2 |
| 速度 vx,vy | 2 × f32 量化为 i16 | 4 |
| 血量百分比 | u8 | 1 |
| 状态位 | u8 | 1 |
| **单实体增量小计** | | **26 B** |

**CREATE 全量包**：额外携带阵营(2) + 外观 ID(4×2) + 名字(≤16) + 等级(2) ≈ **+28 B**，共约 **54 B**，但仅在进入幽灵带时发一次。

**估算场景**：一个 Cell 承载 1000 实体，幽灵带占 Cell 面积约 20%（band = 半个 Cell 宽时偏保守），即 **200 个 Ghost 候选**；平均每个候选需推送给 **1.5 个**有效邻居（边上 1 个、角上 3 个的加权）。

```
每次同步实体数 = 200 × 1.5 = 300
单次批包大小   = 300 × 26 B + 6 B 包头 ≈ 7.8 KiB
10 Hz 稳态带宽 = 7.8 KiB × 10 = 78 KiB/s ≈ 0.62 Mbps  （单向，单 Cell 出口）
```

**阈值触发的额外量**：假设 30% 的候选在同一个 100ms 周期内位移超过 0.5m 并触发提前发送，合并入下一个批包（不额外建包），带宽增幅上界约 **+30% → 约 0.8 Mbps**。

**上界与保护**：

- 全服 64 个 Cell、每 Cell 8 邻居的最坏情形下，集群内 Ghost 总流量约 **64 × 0.8 Mbps ≈ 51 Mbps**，在万兆内网中占比 0.5%，可接受。
- 硬限流：`CE_GHOST_MAX_BYTES_PER_TICK = 32 KiB`（单 Cell 单 tick）。超限时按"距边界距离升序"优先发送（越靠近边界越可能被邻居看到），剩余顺延至下个 tick，并打 `CE_LOG_WARN("GHOST", "sync throttled: pending=%d bytes=%d", ...)`。
- 位置量化：稳态下可把 x/y 从 f32 降为相对 Cell 原点的 `u16`（1 cm 精度，Cell ≤ 655m 时够用），单实体 26 B → **18 B**，带宽再降 30%。列为 Phase 2 的可选优化项，MVP 不做。

---

## 4. 【2.6】跨 Cell AOI 查询

### 4.1 现状与目标

现状（演进计划第 352 行）：`ce_cell_query_nearby` 直接委托全局 AOI，跨 Cell 只是注释（见 1.3(6)）。目标：在 `src_c/server/ce_aoi_grid.c` 增加 `ce_aoi_grid_query_cross_cell()`，**合并本地结果 + Ghost 结果，去重、按距离排序、结果上限截断**。

### 4.2 函数签名（声明加在 `src_c/server/ce_aoi.h` 的 Grid AOI 段落）

```c
/** 跨 Cell 查询结果项（带距离，供排序与客户端优先级裁剪使用） */
typedef struct CeAoiQueryHit {
    uint32_t entity_id;
    float    dist_sq;      /* 到查询中心的平方距离，避免开方 */
    uint8_t  is_ghost;     /* 1 = 来自 Ghost（非本进程权威） */
    uint8_t  _pad[3];
} CeAoiQueryHit;

/**
 * 跨 Cell AOI 查询：本地 Grid 结果 + Ghost 结果合并，去重、按距离升序、截断。
 *
 * @param grid       本地 Grid AOI 实例（本进程权威实体）
 * @param ghost      Ghost 管理器（可为 NULL，退化为纯本地查询）
 * @param x,y,z      查询中心（当前 2D 实现，z 被忽略）
 * @param radius     查询半径（<=0 时用 grid 的 default_radius）
 * @param out_hits   输出缓冲区，按 dist_sq 升序写入
 * @param max_count  缓冲区容量（结果上限截断点）
 * @return           实际写入的结果数（<= max_count）
 */
int ce_aoi_grid_query_cross_cell(CeAoiGrid* grid,
                                 CeGhostManager* ghost,
                                 float x, float y, float z,
                                 float radius,
                                 CeAoiQueryHit* out_hits,
                                 int max_count);
```

同时把 `ce_cell_mgr_query_nearby()` 改为委托本函数（消除它当前丢弃 `mgr` 参数的问题），并保留旧的 `ce_aoi_query_nearby()` 签名做向后兼容包装。

### 4.3 算法

```
1. 本地查询
   复用现有 ce_aoi_grid_query 的 cell 遍历逻辑：
   world_to_cell(x-r, y-r) .. world_to_cell(x+r, y+r) 范围内所有 cell，
   逐链表节点算 dist_sq，<= r² 则写入临时数组 hits[]（is_ghost=0）。

2. Ghost 查询
   ce_ghost_mgr_query(ghost, x, y, radius, ghost_hits, cap)。
   Ghost 管理器内部维护一个独立的小 Grid（Ghost 数量为本地实体的
   ~20%，用同样的 world_to_cell 网格参数），因此也是 O(k_g)。
   命中项写入 hits[]（is_ghost=1）。

3. 去重
   同一 entity_id 可能同时出现在本地与 Ghost 中，典型场景：
   实体刚迁入本进程但邻居的 DESTROY 尚未到达（收敛窗口 ≤ 100ms）。
   规则：**本地权威优先**，丢弃 is_ghost=1 的重复项。
   实现：一个 open-addressing 的 uint32 哈希集合（栈上固定 1024 槽，
   负载 >0.7 时回退到对 hits[] 按 entity_id 排序后线性去重），
   期望 O(n)，最坏 O(n log n)。

4. 排序
   对去重后的 hits[] 按 dist_sq 升序排序。
   n 较小（典型 50~200）时用插入排序（缓存友好、无递归、无 malloc）；
   n > 64 时用 introsort（qsort 风格但避免 libc qsort 的函数指针开销）。

5. 截断
   若 n > max_count，只保留前 max_count 项（最近的）。
   优化：n >> max_count 时用 **nth_element 式 partial selection**
   （O(n) 期望）先选出前 max_count 个，再只对这 max_count 项排序，
   把排序代价从 O(n log n) 降为 O(n + m log m)。

6. 写出并返回 count。
```

### 4.4 复杂度分析

设：
- `k_l` = 本地查询覆盖的 grid cell 内实体总数
- `k_g` = Ghost 查询覆盖的 Ghost 总数（经验值 `k_g ≈ 0.2 · k_l`）
- `n = k_l + k_g` = 去重前候选数
- `m = max_count` = 结果上限

| 步骤 | 复杂度 | 说明 |
|---|---|---|
| 1. 本地查询 | O(k_l) | 沿用现有 O(k) 特性，无回退 |
| 2. Ghost 查询 | O(k_g) | Ghost 独立 Grid，同为 O(k) |
| 3. 去重 | 期望 O(n)，最坏 O(n log n) | 哈希集合 / 排序回退 |
| 4+5. 选取 + 排序 | O(n + m log m) | partial selection 后仅排前 m 项 |
| **总计** | **期望 O(n + m log m) = O(k_l + m log m)** | |
| 空间 | O(n) 栈上；`n > CE_AOI_QUERY_STACK_CAP(512)` 时一次 `calloc` | 热路径默认零堆分配 |

**与朴素做法对比**：若不做 Ghost 而"跨进程 RPC 实时查询邻居"，每次 AOI 查询要付一次网络 RTT（内网 ~0.5ms，P99 可达数 ms），而 AOI 查询频率是 10-30 Hz × 实体数——完全不可行。Ghost 把跨进程通信从**查询时同步 RTT** 转换为**后台 10Hz 异步推送**，代价是 ≤100ms 的状态陈旧度，这在视野显示语义下完全可接受（权威判定仍在主体进程）。

---

## 5. MVP 范围与验收标准

### 5.1 MVP 范围（In / Out）

**In Scope（Phase 2 必须交付）**

| 编号 | 交付项 |
|---|---|
| M1 | `ce_cell_migration.c/h`：完整五阶段协议 + 状态机 + 超时回滚 + 重放缓冲 |
| M2 | `ce_cell_migration_split_redistribute()`：修掉 1.2 的实体丢失 bug，含零丢失不变式断言 |
| M3 | merge 的对称实现（先迁移后销毁 Cell B，含相邻性校验） |
| M4 | `rebuild_neighbors_spatial()`：邻居重建改空间判定，修掉 1.3(7) |
| M5 | `ce_ghost_manager.c/h`：幽灵带判定 + 生命周期 + 10Hz 增量 + 0.5m 阈值触发 + TTL 兜底 |
| M6 | `ce_aoi_grid_query_cross_cell()` + `ce_cell_mgr_query_nearby()` 改为委托它 |
| M7 | `ce_game_protocol.h` 新增 10 个消息类型与结构体（唯一权威来源） |
| M8 | `ce_repl_forget_entity()` / `ce_repl_mark_dirty_ex()` 两个复制系统契约扩展 |
| M9 | 单元测试 + Lua 压测机器人 + CI 接入 |

**Out of Scope（推后）**

- 迁移期间**双写**与冲突消解（已决策不做）
- 位置量化压缩（3.6 末尾的可选优化）
- Ghost 的**多跳**传播（Ghost 的 Ghost）—— 明确不支持，只传一跳
- 非矩形 / 四叉树 Cell 划分（当前只支持矩形四等分）
- 跨**物理机房**迁移（Phase 4 全球加速议题）
- Ghost 参与伤害结算（永远不做，权威只在主体进程）

### 5.2 可量化验收标准

| ID | 指标 | 目标值 | 测量方法 |
|---|---|---|---|
| **A1** | 单批迁移端到端耗时 **P99** | **< 200 ms**（批 ≤ 500 实体） | `ce_cell_migration_begin()` 到 `CE_MIG_DONE` 打时间戳，导出直方图；Lua 机器人统计 P50/P99 |
| **A2** | 单批迁移耗时 P50 | < 90 ms | 同上 |
| **A3** | 玩家冻结期间**不断线** | 断线率 = 0 | Gateway 侧会话计数在迁移前后恒定；Lua 机器人检测连接保持 |
| **A4** | 迁移过程**实体零丢失** | 丢失 = 0，重复 = 0 | 见 5.3 |
| **A5** | split 后四子 Cell 计数和 | `== split 前 entity_count` | Debug 断言 + Release `CE_LOG_ERROR("CELL", ...)`；单测覆盖 |
| **A6** | 跨 Cell AOI 查询延迟 P99 | **< 200 μs**（1000 实体 / 200 Ghost / max_count=100） | `test_aoi_cross_cell.c` 微基准，10 万次查询取 P99 |
| **A7** | 跨 Cell AOI 结果**正确率** | 100% | 与暴力 O(N²) 全量扫描（本地+Ghost）结果集逐项比对 |
| **A8** | Ghost 稳态带宽 | < 1 Mbps / Cell（1000 实体） | Ghost 管理器内置字节计数器，`ce_ghost_mgr_get_stats()` 导出 |
| **A9** | Ghost 状态陈旧度 | P99 < 150 ms | Ghost UPDATE 携带主体侧发送时间戳，邻居侧计算 apply 延迟 |
| **A10** | 内存泄漏 | 0 | Valgrind job，见 5.4 |
| **A11** | 迁移失败回滚成功率 | 100%（注入故障场景下） | 故障注入单测：强制各阶段超时，断言实体全部回到迁出侧且可正常游戏 |
| **A12** | Cell 分裂抖动 | 10 分钟稳态压测内 split+merge 次数 < 5 | 见 7.3 |

### 5.3 "迁移过程实体零丢失"的验证方法（三层）

**第一层：不变式断言（单元测试，`tests/unit/test_cell_migration.c`）**

- 构造 1000 个实体均匀分布在一个 Cell，调 `ce_cell_migration_split_redistribute()`，断言：
  - `sum(sub[i].entity_count) == 1000`；
  - 4 个子 Cell 的实体 ID 集合的**并集等于**原集合，**两两交集为空**（无丢失、无重复）；
  - 每个实体的坐标确实落在其新子 Cell 的 `bounds` 内（半开区间语义）；
  - `ce_aoi_grid_entity_count()` 在 split 前后**恒定**。
- 故障注入版本：在 TRANSFERRING / AWAIT_APPLY / NOTIFY_ROUTER 三个阶段各注入一次超时，断言回滚后实体集合与 split 前**完全一致**。

**第二层：双向对账（集成测试）**

在双 GameServer 进程环境下做一次跨进程迁移，迁移完成后对账：

```
迁出侧实体集合 S_out（迁移后）
迁入侧实体集合 S_in （迁移后）
Router 路由表快照 R
断言：
  1. S_out ∩ batch == ∅                （迁出侧已完全清理）
  2. batch ⊆ S_in                       （迁入侧全部到位）
  3. ∀ e ∈ batch:  R[e] == dst_server_id （路由表无悬空引用）
  4. ∀ e ∈ batch:  迁移前后关键组件字节级相等（Transform / 血量 / 阵营）
  5. 复制系统中 batch 实体在迁出侧的 owner 记录已清除（无脏标残留）
```

**第三层：Lua 压测机器人（`tests/lua/bot_cell_migration.lua`，Headless）**

- 用 Lua 协程并发建立 **2000 个机器人连接**（共享引擎网络层 C 绑定，不用 Python）；
- 每个机器人以 10Hz 上报位置，做**穿越 Cell 边界的往复运动**，持续 10 分钟；
- 每个机器人给自己的实体维护一个**单调递增序号**写入某个可复制字段；
- 运维侧用 `ce_cell_mgr_split()` 主动触发 20 次分裂 / 合并；
- 验证判据：
  1. 结束时服务端实体总数 **== 2000**（零丢失）；
  2. 每个机器人上报的序号在服务端**无回退、无跳变**（状态未被旧进程覆盖，即无双写）；
  3. 无一个机器人连接断开（A3）；
  4. 输出迁移耗时 **P50 / P99** 与跨 Cell AOI 查询 **QPS / P50 / P99**；
- 停服一律 `scripts/stop_cluster_server.sh`，不用 kill/pkill。

### 5.4 CI 集成（复用现有 5 个 job，不新增独立 job）

`.github/workflows/ci.yml` 现有 job：**Lua 语法检查** / **Gateway 集成测试** / **编译 & 测试 (Debug)** / **Release 编译** / **内存检查 (Valgrind)**。挂接方案：

| 验收项 | 接入 job | 具体做法 |
|---|---|---|
| A4/A5/A7/A11（正确性、零丢失、回滚） | **编译 & 测试 (Debug)** | 新增 `tests/unit/test_cell_migration.c`、`test_ghost_manager.c`、`test_aoi_cross_cell.c` 注册进 `tests/CMakeLists.txt`，由该 job 已有的 `ctest --output-on-failure -j$(nproc)` 一并跑到，**不加新 step** |
| A1/A2/A6/A8/A9（性能指标） | **Release 编译** | 复用该 job 已产出的 Release 二进制，追加一步运行 `test_migration_bench`（内置微基准，≤30s），把 P50/P99 写入 `$GITHUB_STEP_SUMMARY`；超阈值则 `exit 1` |
| A3/A12（不断线、抖动抑制） | **Gateway 集成测试** | 该 job 已启动 Gateway + GameServer，追加运行**缩短版** Lua 机器人（200 连接 / 60 秒 / 3 次 split），复用已有集群启动脚本；收尾调 `scripts/stop_cluster_server.sh` |
| Lua 机器人脚本本身 | **Lua 语法检查** | `tests/lua/bot_cell_migration.lua` 自动被该 job 的 `luac -p` 扫描覆盖，无需改配置 |
| A10（内存泄漏） | **内存检查 (Valgrind)** | 把 3 个新单测加入该 job 已有的 Valgrind 用例列表，`--leak-check=full --error-exitcode=1` |

**关键原则**：新增测试全部以**已注册的 ctest 用例**形式存在，让 4 个已有 job 通过"跑 ctest / 跑已构建产物"自动覆盖；只在 Release job 与 Gateway job 各追加**一个 step**，不重复编译、不新建 job。

### 5.5 实施路线（建议提交序列）

| 序 | 提交信息 |
|---|---|
| 1 | `[docs](server): 增加 Phase 2 Cell 迁移与 Ghost 同步设计规格` |
| 2 | `[feat](server): ce_game_protocol.h 增加 Cell 迁移与 Ghost 同步消息定义` |
| 3 | `[fix](server): 修复 ce_cell_mgr_split 实体丢失，按新边界重分配` |
| 4 | `[feat](server): 新增 ce_cell_migration 五阶段迁移协议与状态机` |
| 5 | `[feat](server): 新增 ce_ghost_manager 边界实体影子副本同步` |
| 6 | `[perf](server): ce_aoi_grid 增加跨 Cell 查询，合并本地与 Ghost 结果` |
| 7 | `[test](server): 增加迁移零丢失单测与 Lua 压测机器人` |

---

## 6. 不变式清单（实现与 Review 的 checklist）

1. **单一权威**：任意时刻，任一 `entity_id` 在整个集群中**只有一个** GameServer 持有权威副本。Ghost 永远只读。
2. **计数守恒**：`Σ cell[i].entity_count == 本进程 AOI 实体数`（不含 Ghost）。split/merge/迁移前后均成立。
3. **无悬空脏标**：实体迁出后，迁出侧 `CeReplContext` 中不再有其 owner 与脏字段记录。
4. **无悬空路由**：Router 路由表中每个 `entity_id` 指向的 server 必然当前持有该实体权威。
5. **无重复广播**：同一 `entity_id` 不会在同一帧被两个进程下发给同一客户端（去重规则见 4.3 步骤 3）。
6. **半开区间**：Cell 归属判定统一用 `[min, max)`，与 `mgr_pos_to_cell_index()` 语义一致；边界实体归属唯一。
7. **原子性**：迁移是 all-or-nothing，不存在"一半实体在 A、一半在 B"的中间态被观察到。
8. **Ghost 不持久化**：Ghost 实体绝不触发 `CE_FLAG_PERSIST` 路径，绝不写 DBProxy。
9. **Ghost 单跳**：Ghost 不再向其邻居产生 Ghost。
10. **纯 C 热路径**：迁移、Ghost 同步、AOI 查询三条路径上不出现任何 Lua 调用。

---

## 7. 风险分析与缓解

### 7.1 迁移中途 GameServer 宕机

| 宕机方 | 时点 | 后果 | 缓解 |
|---|---|---|---|
| **迁出侧** | 阶段 1-2（尚未 push 成功） | 批内实体随进程一起丢失 | 阶段 1 末尾**先 flush `CE_FLAG_PERSIST` 字段到 DBProxy**；玩家重连后从存档恢复，最多回退到冻结时刻（≤200ms 的操作损失） |
| **迁出侧** | 阶段 3-5（迁入侧已导入） | 迁入侧持有实体但等不到 COMMITTING | 迁入侧对 `CE_MIG_AWAIT_APPLY` 之后的状态设 `CE_MIGRATION_ORPHAN_TIMEOUT_MS = 3000` 的**孤儿超时**；超时后向 Router 查询该批实体的权威归属：若 Router 已提交给自己 → **主动完成提交并解冻**（前向恢复）；若未提交 → 丢弃 |
| **迁入侧** | 阶段 2-4（ACK 未回） | 迁出侧超时 | 迁出侧 TRANSFERRING/AWAIT_APPLY 超时 → 回滚，实体在迁出侧解冻继续游戏。**这是最安全的路径**（迁出侧仍是权威，未做任何破坏性操作） |
| **迁入侧** | 阶段 5 之后（已成为权威） | 实体随迁入进程丢失 | 属于普通"GameServer 宕机"场景，由 Phase 2 的服务健康检查 + Router 摘除 + 玩家重连从 DBProxy 恢复处理，不是迁移特有风险 |
| **Router** | 阶段 5 | 无法确认路由 | Router 由 2.2 的 Raft 保证多数派可用；少数派故障对本协议透明。Raft 整体不可用时，`ce_cell_migration_tick()` **停止发起新迁移**（熔断），已在途的迁移超时回滚 |

**通用原则**：**破坏性操作（迁出侧清理）永远排在最后**（阶段 5 之后）。任何早于此的宕机都退化为"回滚 + 迁出侧继续服务"或"存档恢复"，绝不产生两个权威。

**告警**：进入 `CE_MIG_FAILED`（回滚也失败）时必须 `CE_LOG_ERROR("MIGRATE", "migration %llu FAILED, manual intervention required: src_cell=%u dst_cell=%u entities=%d", ...)`，并接入 admin 告警通道。

### 7.2 Ghost 与主体不一致

| 风险 | 表现 | 缓解 |
|---|---|---|
| **状态陈旧** | 10Hz 同步 → Ghost 最多落后 100ms（+ 网络单程） | 复制速度向量让邻居侧客户端做**位置外推插值**，视觉上抹平；A9 监控陈旧度 P99 < 150ms |
| **丢包导致增量断链** | Ghost 状态永久停在旧值 | (a) Ghost 包携带**单调递增 `seq`**，邻居侧检测到 `seq` 跳变即发 `RESYNC_REQ` 请求全量；(b) 每 **50 个同步周期（5s）** 强制发一次全量快照做**自愈基线**；(c) `CE_GHOST_TTL_MS=1000` 心跳超时自动销毁 |
| **主体已死，Ghost 仍在** | 邻居侧显示已死实体（僵尸实体，正是 1.3(2) 的症状） | DESTROY 显式下发 + TTL 兜底双保险；TTL 触发时打 `CE_LOG_WARN("GHOST", "ghost %u expired without DESTROY", ...)` 用于发现协议漏洞 |
| **Ghost 被误当权威** | 逻辑系统在 Ghost 上做移动/伤害结算 → 状态分叉 | Ghost 实体统一打 `CE_ENTITY_GHOST` 标记；所有权威性逻辑系统在 Archetype 查询时**排除**该标记；写入尝试在 Debug 构建下 `assert`，Release 下 `CE_LOG_ERROR("GHOST", ...)`。这条是**最高优先级的 Review 项** |
| **迁移期间 Ghost 与新主体并存** | 短窗口内同一 entity 既是本地实体又是 Ghost | 4.3 步骤 3 的"本地权威优先"去重规则保证查询结果唯一；收敛窗口 ≤ 100ms |
| **双向 Ghost 环** | A 给 B 发 Ghost，B 又把该 Ghost 回传 A | Ghost 单跳约束（不变式 9）：`ce_ghost_mgr_tick()` 只扫描带权威标记的本地实体，`CE_ENTITY_GHOST` 实体不进入候选集 |

### 7.3 Cell 频繁分裂合并抖动的抑制

1.3(1) 已说明现状会产生 split→merge 死循环。抑制策略（**四重**）：

1. **滞回阈值（hysteresis）**：分裂阈值 `max_entities`，合并阈值必须显著低于**分裂后单子 Cell 的期望负载**。约束：`min_entities < max_entities / 4 × 0.6`。例如 `max=2000` → 分裂后每子 Cell 期望 500 → `min ≤ 300`。这从根本上杜绝"刚分裂就够合并条件"。
2. **冷却期（cooldown）**：`CeCell` 增加 `uint64_t last_topology_change_ms` 字段。任一 Cell 在 split 或 merge 后 `CE_CELL_TOPOLOGY_COOLDOWN_MS = 60000`（60s）内**不允许**再次参与 split 或 merge，`ce_cell_mgr_update()` 直接跳过。
3. **持续时间确认（debounce）**：负载越界必须**连续 N 帧**成立才触发。`ce_cell_mgr_update()` 里给每个 Cell 维护 `overload_frames` / `underload_frames` 计数器，要求 `>= CE_CELL_LOAD_CONFIRM_FRAMES = 300`（30Hz 下约 10 秒）。瞬时人群聚集（如世界 BOSS 开场 5 秒）不会触发拓扑变更。
4. **迁移预算（rate limit）**：全局 `CE_MIGRATION_MAX_CONCURRENT = 2` 个在途批次，且单个 Cell 每小时最多 `CE_CELL_MAX_TOPOLOGY_CHANGES_PER_HOUR = 4` 次拓扑变更。超限时打 `CE_LOG_WARN("CELL", "topology change budget exhausted for cell %u", cell_id)` 并推迟。

**补充：负载指标不只看实体数。** 单看 `entity_count` 会误判（100 个满屏 AOE 的玩家比 1000 个静止 NPC 重得多）。建议 `load_score = entity_count + 4 × player_count + tick_cpu_us / 100`，用 EMA（`α = 0.05`）平滑后再与阈值比较。MVP 可先只做 EMA 平滑的 `entity_count`，`load_score` 列为 Phase 3 优化。

### 7.4 其他风险

| 风险 | 缓解 |
|---|---|
| **split 后 `mgr_pos_to_cell_index()` 失效**（1.3(7)） | M4 把寻址改为"规则网格快速路径 + split 过的区域走空间树/线性回退"；`cells_per_row/col` 在 split 后不再作为唯一寻址依据 |
| **`realloc` 使 `CeCell*` 指针失效** | 现有 `ce_cell_mgr_split()` 第 322 行 `realloc` 后仍用 split 前取得的 `cell` 指针（第 331 行）——**这是一个悬空指针 UB**。修复：realloc 后用 `cell_idx` 重新取址 `cell = &mgr->cells[cell_idx]`。必须写进单测并由 Valgrind job 覆盖 |
| **迁移包被篡改 / 版本不匹配** | `magic` + `version` + `payload_crc32` 三重校验；跨版本滚动升级期间拒绝迁移并打 ERROR，而不是尝试兼容解析 |
| **重放缓冲导致操作重复结算** | 只重放客户端输入与跨实体副作用 RPC，**丢弃定时器/AI tick**；有副作用的 RPC 携带唯一 `request_id`，迁入侧幂等去重 |
| **io_uring 在旧内核不可用** | 通过 `ce_async_io` 抽象层的既有能力探测走平台回退路径；本模块不直接依赖 `io_uring_*` 符号 |

---

## 8. 变更文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `src_c/server/ce_cell_migration.h` | 新建 | 迁移协议 API、状态枚举、常量 |
| `src_c/server/ce_cell_migration.c` | 新建 | 五阶段协议实现、状态机、重放缓冲 |
| `src_c/server/ce_ghost_manager.h` | 新建 | Ghost 管理器 API |
| `src_c/server/ce_ghost_manager.c` | 新建 | 幽灵带判定、生命周期、10Hz 增量同步 |
| `src_c/server/ce_game_protocol.h` | 修改 | 新增 10 个消息类型与结构体（**协议唯一权威来源**） |
| `src_c/server/ce_aoi.h` | 修改 | 新增 `CeAoiQueryHit` 与 `ce_aoi_grid_query_cross_cell()` 声明 |
| `src_c/server/ce_aoi_grid.c` | 修改 | 实现跨 Cell 查询（本地 + Ghost 合并、去重、排序、截断） |
| `src_c/server/ce_cell_manager.c` | 修改 | 修 split 实体丢失、修 realloc 悬空指针、merge 对称化、邻居空间重建、滞回/冷却/debounce |
| `src_c/server/ce_server_types.h` | 修改 | `CeCell` 增加 `last_topology_change_ms` / `overload_frames` / `underload_frames` |
| `src_c/replication/ce_replication.h` | 修改 | 新增 `ce_repl_forget_entity()` / `ce_repl_mark_dirty_ex()` |
| `tests/unit/test_cell_migration.c` | 新建 | 零丢失、故障注入回滚、realloc 安全 |
| `tests/unit/test_ghost_manager.c` | 新建 | 生命周期、TTL、字段白名单、单跳约束 |
| `tests/unit/test_aoi_cross_cell.c` | 新建 | 正确性（对暴力法比对）+ 微基准 |
| `tests/lua/bot_cell_migration.lua` | 新建 | Headless Lua 压测机器人（多协程、P50/P99） |
| `tests/CMakeLists.txt` | 修改 | 注册 3 个新 ctest 用例 |
| `.github/workflows/ci.yml` | 修改 | Release job / Gateway job 各追加一个 step（不新增 job） |

---

**文档结束** · v0.1 · 待评审
