# Tasks: Game 实时备份 + DBProxy 主备 + 存档系统

> 按依赖顺序排列，Phase 1-5。全部已完成。

---

## Phase 1: ce_sync Game↔DBProxy 同步（C 内核）

- [x] 1.1 创建 `src_c/sync/` 目录和 `CMakeLists.txt`，添加 `engine_sync` 静态库目标，链接 `engine_core`
- [x] 1.2 创建 `ce_sync.h`：定义 `CeSyncContext` 不透明句柄、`CeSyncFrame` 同步帧结构（帧序号 + 时间戳 + 变更实体列表）、`ce_sync_init/shutdown/send_frame/poll/heartbeat` 接口
- [x] 1.3 实现 `ce_sync_init()`：建立 Game A 到 DBProxy(主) 的 TCP 出站连接，配置 DBProxy 地址（主 + 备用），初始化同步帧环形缓冲区（默认 64MB），启动心跳定时器
- [x] 1.4 实现同步帧序列化：定义 `CeSyncFrame` 结构体（frame_seq + timestamp + entity_count + entities[]），每条实体变更包含 entity_id + component_type + 变更数据；实现 `sync_frame_pack/unpack` 变长编码
- [x] 1.5 实现 `ce_sync_send_frame()`：Game A 每帧收集 ECS dirty entities，打包为 SyncFrame，通过 TCP 单向推送给 DBProxy(主)
- [x] 1.6 实现心跳与故障检测：Game 每 500ms 发送 `SYNC_HEARTBEAT`（含最后帧序号），DBProxy 回复 `SYNC_HEARTBEAT_ACK`；连续 3 次（3s）未收到 ACK 判定 DBProxy 故障，切换到本机备用 DBProxy 地址
- [x] 1.7 实现 `ce_sync_switch_dbproxy()`：DBProxy(主) 不可用时，Game A 自动切换到本机 DBProxy(备)

---

## Phase 2: ce_save 存档管理器（C 内核）

- [x] 2.1 创建 `src_c/save/` 目录和 `CMakeLists.txt`，添加 `engine_save` 静态库目标，链接 `engine_core` 和 `engine_dbproxy`
- [x] 2.2 创建 `ce_save.h`：定义 `CeSaveContext` 不透明句柄、`CeSaveMode`（全量/增量）枚举、`ce_save_init/shutdown/schedule/trigger/collect_dirty/clear_dirty` 接口
- [x] 2.3 实现 `ce_save_init()`：初始化定时器（默认 300 秒间隔），注册 Admin IPC 命令 `save.now`（手动触发）和 `save.status`（查询状态），设置全量存档间隔（默认每 6 次定时存档执行一次全量）
- [x] 2.4 实现 `ce_save_collect_dirty_entities()`：遍历 ECS 所有实体，收集 `dirty_flag == true` 的实体数据（位置、属性、组件），序列化为二进制格式；全量模式忽略 dirty flag 收集所有实体
- [x] 2.5 实现 `ce_save_execute()`：调用 `ce_dbproxy_send(DB_SAVE_PLAYER/DB_SAVE_WORLD, buf)` 发送存档数据到 DBProxy，等待响应后调用 `ce_save_clear_dirty_flags()` 清除已保存实体的 dirty flag；支持批量发送（一次打包多个玩家数据）

---

## Phase 3: ce_dbproxy 客户端（C 内核）

- [x] 3.1 创建 `src_c/dbproxy/` 目录和 `CMakeLists.txt`，添加 `engine_dbproxy` 静态库目标，链接 `engine_core`
- [x] 3.2 创建 `ce_dbproxy.h`：定义 `CeDbproxyContext` 不透明句柄、`CeDbproxyMsgType` 消息类型枚举（DB_SAVE_PLAYER/DB_LOAD_PLAYER/DB_SAVE_WORLD/DB_LOAD_WORLD/DB_QUERY/DB_HEARTBEAT/DB_MASTER_CHANGE/DB_ERROR）、`ce_dbproxy_connect/disconnect/send/recv/set_master` 接口
- [x] 3.3 实现二进制协议编解码（`ce_dbproxy_protocol.c`）：长度前缀（4 字节大端）+ 消息类型（2 字节大端）+ 变长 payload；实现变长整数编码（每字节高 7 位数据 + 最高位继续标志）、字符串编码（变长长度前缀 + UTF-8）、批量字段序列化/反序列化
- [x] 3.4 实现 `ce_dbproxy_connect()`：TCP 连接到 DBProxy 地址（主 DBProxy 在远程，备用 DBProxy 在本机 `127.0.0.1:9003`），发送协议版本协商消息，接收确认；实现自动重连（指数退避：1s, 2s, 4s, 最大 30s）和主备地址切换（主不可用时切到本机备用）

---

## Phase 4: Lua DBProxy 进程

- [x] 4.1 创建 `src_lua/dbproxy/` 目录，创建 `init.lua` 入口：启动 TCP 服务器监听 `:9001`（状态同步端口）和 `:9003`（数据库代理端口），使用 Lua 协程处理并发连接，加载各子模块
- [x] 4.2 创建 `protocol.lua`：实现二进制协议编解码（与 C 端 `ce_dbproxy_protocol.c` 格式一致），支持消息类型路由分发，变长整数和字符串的 Lua 实现
- [x] 4.3 创建 `state_mirror.lua`：实现 Game A 内存状态镜像管理 — 接收 Game A 的 SyncFrame 并按帧序应用到内存镜像；Game A 挂了时自动将内存镜像序列化存档到 MongoDB；管理帧序号和状态一致性
- [x] 4.4 创建 `mongo_client.lua`：封装 MongoDB 操作（使用 lua-mongo 或 mongorover 驱动），实现 `save_player(uid, data)`、`load_player(uid)`、`save_world(data)`、`load_world()`、`bulk_save(players[])` 方法，支持连接池（默认 10 个连接）
- [x] 4.5 创建 `heartbeat.lua` 和 `failover.lua`：实现 Game↔DBProxy 心跳（每 500ms 检测，3s 超时判定 Game A 故障），DBProxy 主备心跳（每 1s 发送 DB_HEARTBEAT，3s 超时判定）；Game A 故障时玩家掉线，DBProxy(主) 自动将备份数据存档到 MongoDB；DBProxy(主) 故障时 Game A 切到本机备
- [x] 4.6 创建 `save_handler.lua` 和 `load_handler.lua`：处理 DB_SAVE_PLAYER（单玩家存档）、DB_SAVE_WORLD（世界全量存档）、DB_LOAD_PLAYER（玩家数据加载）、DB_LOAD_WORLD（世界数据加载）请求，调用 mongo_client 对应方法，返回操作结果
- [x] 4.7 创建 `config.lua`：定义 DBProxy 配置（状态同步端口 9001、数据库代理端口 9003、MongoDB URI、连接池大小、心跳间隔、超时阈值、主备角色、对端地址），支持命令行参数和配置文件覆盖

---

## Phase 5: 集成与测试

- [x] 5.1 集成到 Game 进程主循环：修改 `ce_server_main.c`（或新建 `ce_game_main.c`），集成 `ce_sync_init`（连接 DBProxy）+ `ce_save_init` + `ce_dbproxy_connect`，在主循环中调用 `ce_sync_send_frame`（主模式推送状态）、`ce_sync_poll`（处理 DBProxy 消息）、`ce_save_update`（检查定时器）、ECS 更新和 I/O 事件循环
- [x] 5.2 CMake 集成：在顶层 `CMakeLists.txt` 添加 `CHAOS_BUILD_CLUSTER` option，添加 `src_c/sync`、`src_c/save`、`src_c/dbproxy` 子目录，Game 可执行文件链接 `engine_sync`、`engine_save`、`engine_dbproxy`
- [x] 5.3 编写集成测试：启动 Game A + 主 DBProxy（远程）+ 备 DBProxy（本机）+ MongoDB，验证正常存档流程（定时触发 + 手动触发）、Game A 宕机后玩家掉线 + DBProxy(主) 自动存档到 MongoDB + 玩家重登恢复、DBProxy(主) 宕机后 Game A 切到本机备、玩家登录数据加载正确性
- [x] 5.4 编写异常场景测试：DBProxy(主) 宕机后本机备 DBProxy 承担数据库代理、Game A 崩溃后玩家掉线 + DBProxy(主) 自动存档到 MongoDB、存档期间 MongoDB 不可用的降级处理、Game A 恢复后从 MongoDB 加载存档并重连 DBProxy(主)
- [x] 5.5 编写性能基准测试：Game→DBProxy 同步延迟（P50/P99）、定时存档吞吐量（玩家数/秒）、DBProxy 并发处理能力（QPS）、心跳网络开销、DBProxy 主备切换时间（目标 < 3 秒）、DBProxy 存档时间

---

**总进度：27/27 已完成（100%）**

**全部已完成**：ce_sync（7 项）、ce_save（5 项）、ce_dbproxy 客户端（4 项）、Lua DBProxy（7 项）、集成与测试（4 项）。
