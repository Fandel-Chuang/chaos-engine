# add-game-cluster-save

Game 实时备份 + DBProxy 主备 + 定时/手动存档

> 来源: add-game-cluster-save | 状态: 规划中 | 变更类型: ADDED

---

## Why

ChaosEngine v0.2 的服务端（`chaos_server`）目前是单进程部署，Game 进程和 Cell/AOI 空间子系统运行在同一进程中，存在以下核心风险：

1. **数据丢失风险**：当前无任何持久化机制。ECS 实体状态、AOI 位置、Cell 负载分布、玩家背包/属性等关键数据仅存在于内存中。进程崩溃或所在机器宕机，整个游戏世界状态全部丢失，所有在线玩家断开连接。对于 MMO 类游戏，这意味着数小时甚至数天的游戏进度付诸东流，用户体验灾难性。
2. **无实时备份能力**：Game 进程运行期间，所有状态仅存于自身内存，没有任何外部备份。一旦进程异常退出，内存数据不可恢复。
3. **数据库直连风险**：若未来 Game 进程直连 MongoDB，每个 Game 进程都持有数据库连接，连接数膨胀，且 Game 进程崩溃可能导致未提交事务丢失。
4. **存档时机不可控**：无定时自动存档机制，也无程序化触发手动存档的接口。存档完全依赖人工操作，容易遗漏。

引入实时备份和存档系统后，可实现：
- Game A 通过 TCP 向 DBProxy(主) 实时同步内存状态，DBProxy 存储 Game A 的备份数据于自身内存中
- Game A 崩溃后，DBProxy(主) 把内存中的备份数据存档到 MongoDB，玩家重新登录时从 MongoDB 恢复，数据不丢失
- DBProxy 中间层隔离 Game 与数据库，DBProxy(主) 故障时 Game A 自动切到本机 DBProxy(备)
- N 分钟定时存档 + 关键操作（如玩家交易、Boss 击杀）手动触发立即存档
- 纯 C 内核 + Lua DBProxy 进程，保持与引擎现有技术栈一致

## What Changes

### 新增模块

1. **Game 同步模块（`ce_sync`，C）**：`src_c/sync/ce_sync.h/c` — Game A 与 DBProxy 之间的 TCP 实时状态同步。Game A 将每帧 ECS 状态变更、AOI 事件、Cell 迁移等序列化为二进制协议，通过 TCP 长连接单向推送给 DBProxy(主)。DBProxy 在自身内存中维护 Game A 的状态镜像（实时备份）。同步协议为 Game A → DBProxy(主) 单向推送，DBProxy 不主动拉取。

2. **存档模块（`ce_save`，C）**：`src_c/save/ce_save.h/c` — 定时存档 + 手动存档接口。N 分钟（可配置）定时将完整游戏状态（ECS 实体快照、AOI 位置、Cell 分布）序列化为二进制存档文件。提供 `ce_save_trigger()` 公共 API 供关键操作调用立即存档。存档文件带时间戳和校验和，支持回档时选择特定时间点。

3. **DBProxy 客户端（`ce_dbproxy`，C）**：`src_c/dbproxy/ce_dbproxy.h/c` — Game 进程侧的 DBProxy 通信客户端。Game 不直连 MongoDB，所有数据库操作（读/写/查询）通过 DBProxy 中转。支持主备 DBProxy 地址配置，主 DBProxy 不可用时自动切换到本机备用 DBProxy。协议采用二进制请求-响应模式，支持连接池和请求队列。

4. **DBProxy 进程（Lua）**：`src_lua/dbproxy/` — 独立 Lua 进程，基于 Lua 5.4 + LuaSocket + lua-mongo 驱动。接收来自 Game 进程的数据库请求，转发到 MongoDB 执行，返回结果。支持主备模式：远程主 DBProxy + 本机备用 DBProxy，Game 优先连接远程主 DBProxy，连接失败时自动降级到本机备用。进程由 `chaos_dbproxy` 可执行文件启动。

### 架构决策

| 决策 | 选择 | 理由 |
|------|------|------|
| Game 同步方式 | TCP 长连接 + 二进制协议（Game → DBProxy 单向推送） | TCP 保证可靠有序传输，二进制协议比 JSON/Protobuf 更轻量，无外部依赖；Game 之间无直接连接，所有状态同步经 DBProxy 中转 |
| 同步粒度 | 帧级增量同步（操作指令或脏数据） | 每帧只传输变更数据（dirty entities），而非全量快照，带宽开销可控 |
| 故障检测 | 心跳超时（可配置，默认 3s） | 简单可靠，无需外部服务（如 etcd/Consul），适合游戏服务器场景 |
| 故障恢复 | A 挂了 → 玩家掉线 → DBProxy(主) 存档到 MongoDB → 玩家重登时恢复；DBProxy(主) 挂了 → A 切到本机 DBProxy(备) | 数据不丢失；DBProxy 主备保证数据库服务不中断 |
| DBProxy 语言 | Lua 5.4 | 与引擎现有 Lua 生态一致（`src_lua/admin/` 已使用 Lapis + Lua），lua-mongo 驱动成熟 |
| DBProxy 架构 | 独立进程 + 主备模式 + 存 Game 内存备份 | 进程隔离避免 Game 崩溃影响数据库连接；主备保证 DBProxy 自身可用性；DBProxy(主) 在远程，本机为备，主不可用时切到本机备用 |
| 存档格式 | 二进制快照 + 校验和 | 比 JSON/BSON 更紧凑，序列化/反序列化快；校验和防止存档文件损坏 |
| 存档触发 | 定时（N 分钟）+ 手动 API | 定时保证最低保存频率，手动 API 覆盖关键操作（交易、Boss 击杀等） |
| 编译策略 | CMake option + `#ifdef` 条件编译 | `CHAOS_HAS_CLUSTER` 控制，非 Server 构建零开销 |

## Impact

### 受影响的代码

| 文件/目录 | 影响类型 | 说明 |
|-----------|----------|------|
| `src_c/sync/ce_sync.h` | **新增** | Game 同步模块接口（连接管理、状态推送、心跳、故障检测） |
| `src_c/sync/ce_sync.c` | **新增** | Game 同步模块实现（TCP 长连接、二进制协议、增量同步） |
| `src_c/save/ce_save.h` | **新增** | 存档模块接口（定时存档、手动触发、回档加载） |
| `src_c/save/ce_save.c` | **新增** | 存档模块实现（二进制序列化、校验和、文件管理） |
| `src_c/dbproxy/ce_dbproxy.h` | **新增** | DBProxy 客户端接口（连接管理、请求/响应、主备切换） |
| `src_c/dbproxy/ce_dbproxy.c` | **新增** | DBProxy 客户端实现（二进制协议、连接池、自动切换） |
| `src_lua/dbproxy/` | **新增** | Lua DBProxy 进程（init.lua, app.lua, mongo_client.lua, config.lua） |
| `src_c/runtime/ce_dbproxy_main.c` | **新增** | DBProxy 进程入口（嵌入 Lua VM，启动 DBProxy 服务） |
| `src_c/runtime/ce_server_main.c` | 修改 | 集成 ce_sync/ce_save/ce_dbproxy 初始化和主循环调用 |
| `src_c/CMakeLists.txt` | 修改 | 添加 `CHAOS_HAS_CLUSTER` 选项、新增子目录和链接库 |
| `src_c/sync/CMakeLists.txt` | **新增** | sync 模块构建脚本 |
| `src_c/save/CMakeLists.txt` | **新增** | save 模块构建脚本 |
| `src_c/dbproxy/CMakeLists.txt` | **新增** | dbproxy 客户端构建脚本 |

### 不受影响的模块

- ECS、AOI、Cell、Memory、Log、Render 核心逻辑不变（sync 模块读取其状态但不修改其内部实现）
- Network 模块（`ce_network.h/c`）不受影响（sync 使用现有 TCP socket API）
- Admin IPC 模块不受影响
- Client/Headless 编译目标不受影响
- 不带 `CHAOS_HAS_CLUSTER` 编译时，server 行为完全不变（单进程模式）

### MVP 范围

**Phase 1：Game↔DBProxy 同步基础** — TCP 长连接建立、二进制协议定义、帧级增量状态推送（Game A → DBProxy(主) 单向）、心跳检测。Game A 将每帧 ECS dirty entities 推送给 DBProxy(主)，DBProxy 在内存中维护状态镜像。

**Phase 2：故障处理** — 心跳超时检测、Game A 挂了 → 玩家掉线 → DBProxy(主) 将内存备份存档到 MongoDB、DBProxy(主) 挂了 → Game A 切到本机 DBProxy(备)。包含故障检测和切换日志。

**Phase 3：存档系统** — 定时存档（可配置间隔，默认 5 分钟）、手动存档 API（`ce_save_trigger()`）、存档文件管理（时间戳 + 校验和）、回档加载接口。存档数据经 DBProxy 写入 MongoDB。

**Phase 4：DBProxy 主备** — C 侧 DBProxy 客户端（连接池、主备切换）、Lua DBProxy 进程（MongoDB 驱动、请求转发、Game 内存状态备份）、Game → DBProxy 二进制协议。DBProxy(主) 在远程机器，本机 DBProxy 为备用。

**延后到后续版本**：
- 多 Game 进程负载均衡（当前仅支持 1 主 1 备）
- 存档增量备份（当前为全量快照）
- DBProxy 读写分离（主写从读）
- 跨 Cell 进程迁移时的状态同步
- 存档加密和压缩
- 基于 Raft/Paxos 的多副本一致性（当前为主备模式）
