# ChaosEngine Phase 2 Router 集群设计规格说明书

- **文档编号**：chaos-engine-phase2-router-spec-v0.1
- **版本**：v0.1（草案）
- **作者**：zhongfangdao
- **覆盖范围**：演进计划 Phase 2 的 2.1 / 2.2 / 2.3 三项
- **上游依据**：`docs/spec/chaos-engine-evolution-plan-v1.md`（第 288–325 行）、`docs/spec/chaos-engine-spec-v0.2.md`、`docs/spec/chaos-engine-architecture-review-v1.md`
- **状态**：待评审

---

## 0. 概述与设计约束

### 0.1 目标

ChaosEngine 是全球同服 MMO 游戏引擎，采用「纯 C 内核（`src_c/`）+ C++ 编辑器（`src_cpp/`）+ Lua 脚本（`src_lua/`）」三层架构，ECS 使用 Archetype 布局，渲染基于 Vulkan。当前 Router（路由/服务发现/集群协调组件）以 Lua 实现，位于 `src_lua/router/`，共 9 个模块约 3400 行代码。它承担三类职责：

1. **服务注册与发现**：Game / DBProxy 等进程向 Router 注册，Gateway 与其它服务查询。
2. **消息路由**：按 `service_id` 直达、按一致性哈希落点、按 N 副本广播。
3. **集群协调**：Router 之间同步注册表、选举 Leader、跨区域（cross-region）转发。

Phase 2 要把这套东西升级为可支撑全球同服规模的分布式路由层，本 spec 覆盖三个子项：

| 子项 | 名称 | 目标 | 预估 |
|---|---|---|---|
| 2.1 | Router 迁移到纯 C | `src_c/router/`，io_uring 事件驱动 | 4 周 |
| 2.2 | Router 增加 Raft 共识 | `ce_raft.c/h`，真 Leader 选举 + 日志复制 | 4 周 |
| 2.3 | 实体路由表 | `ce_router_entity_table.c/h`，全局 `entity_id → game_server_id` | 2 周 |

### 0.2 硬性约束（贯穿全文，实现时不可协商）

| 编号 | 约束 | 本 spec 落实位置 |
|---|---|---|
| C1 | **性能关键组件必须纯 C 实现，坚决拒绝 Lua**。Router 的路由查询、Raft 状态机、实体表都在关键路径上。 | 第 2/3/4 章全部为 C99 设计；Lua 仅保留为运维旁路（第 6 章） |
| C2 | **异步 I/O 必须用 io_uring（liburing 2.14），不允许用 epoll**。跨平台通过既有 `src_c/network/ce_async_io.h` 抽象层（Windows 走 IOCP）。 | 第 2.3 节事件循环设计，复用 `ce_async_init/submit/wait/get_event` |
| C3 | **多协议倾向共用同一端口号**。Gateway 已在 9000 端口共用 TCP/KCP/WebSocket 三协议。 | 第 2.4 节：Router 单端口 9100 复用「Game 面 + Raft 面 + 跨区域面」 |
| C4 | **公共代码只写一份**，禁止客户端/服务端各抄一份。`src_c/server/ce_game_protocol.h` 是协议常量与结构体的唯一权威来源，新增协议消息必须加在那里。 | 第 2.5 节协议扩展全部落在 `ce_game_protocol.h` |
| C5 | **测试必须集成进现有 GitHub Actions CI**（`.github/workflows/ci.yml`，已有 5 个中文 job），复用已有构建产物，避免新增独立 job。 | 第 5.3 节逐条映射到既有 job |
| C6 | **日志宏签名是 `CE_LOG_ERROR(cat, fmt, ...)`**，第一个参数是分类 tag 字符串。 | 全文示例统一使用 `"ROUTER"` / `"RAFT"` / `"ENTITAB"` |
| C7 | **压测/机器人脚本用 Lua**（Headless 模式，共享引擎网络层 C 绑定），不要用 Python。 | 第 5.2 节压测方案 |
| C8 | 提交格式 `[type](scope): description`，type ∈ {feat,fix,refactor,docs,perf,test}，scope ∈ {core,ecs,render,network,server,plugin,editor,script,engine}。 | 第 5.4 节路线图 |

### 0.3 关键设计决策（已定，不再讨论）

- **D1：自研精简版 Raft**。只做 Leader 选举 + 日志复制，**不做**快照压缩、**不做**成员动态变更（joint consensus）。集群成员表由静态配置文件给定，变更需滚动重启。原因：避免引入 etcd（Go）/ braft（C++）依赖，破坏纯 C 内核的编译与部署模型；MMO Router 集群规模固定为 3 或 5 节点，动态成员变更收益极低而实现复杂度极高。
- **D2：Router 集群规模固定 3 或 5**（奇数，quorum = N/2+1）。跨区域不组成同一个 Raft group，每个 region 一个独立 Raft group，region 之间用跨区域转发链路（最终一致）。
- **D3：实体路由表写走 Raft、读走本地**。Follower 也可提供读（可能读到略旧数据），Gateway 侧接受最终一致；对一致性敏感的操作（Cell 迁移确认）必须走 Leader 的线性读。

---

## 1. 现状分析：`src_lua/router/` 逐模块

### 1.1 模块清单与规模

| 文件 | 行数 | 职责 | 处置 |
|---|---|---|---|
| `init.lua` | 854 | 进程入口、CLI 解析、协程调度器、双端口 TCP 服务、消息分发、若干后台 loop、优雅退出 | **重写为 C**（事件循环 + 分发） |
| `cluster.lua` | 496 | Router 对等节点管理、连接、伪选举、注册表同步 | **重写为 C**（选举部分由 Raft 取代） |
| `cross_region.lua` | 427 | 远端 region Router 连接、跨区域消息封装、TTL/hop 防环 | **重写为 C** |
| `protocol.lua` | 341 | 二进制协议打包/解包、消息类型常量 | **废弃**，常量并入 `ce_game_protocol.h` |
| `registry.lua` | 320 | 服务注册表、心跳、过期清理、按类型查询 | **重写为 C** |
| `hash_ring.lua` | 307 | 一致性哈希环、虚拟节点、权重、lookup / lookup_n | **重写为 C** |
| `health.lua` | 307 | 心跳跟踪器、PING/PONG、超时回调 | **重写为 C**（并入连接层） |
| `router.lua` | 251 | 连接注册表、三种路由策略（by service / by conn / by hash） | **重写为 C** |
| `config.lua` | 97 | 集中配置常量 | **重写为 C**（配置文件解析） |

### 1.2 `init.lua`（854 行）— 进程入口与事件循环

**现有逻辑：**

- **Bootstrap**：用 `debug.getinfo` 推导脚本目录，拼 `package.path`，`require` 其余 8 个模块。
- **CLI 解析**：支持 `--node-id / --region / --game-port / --cluster-port / --peer node:host:port / --remote-region region:host:port / --log-level`。`--peer` 与 `--remote-region` 用 `string.match` 解析三段式，追加到 `config.peers` / `config.remote_regions`。
- **日志**：自建 `LOG_LEVELS = {trace=0,...,error=4}`，`log(level, mod, msg)` 写 `io.stderr` 并 flush，格式 `[时间] [级别] [模块] 消息`。
- **协程调度器**：`spawn(fn, name)` 用 `coroutine.create` + `pcall` 包裹；`wait_readable(sock)` 把 socket 登记到 `sockets_to_watch` 后 `coroutine.yield()`；`sleep(seconds)` 用 `socket.gettime()` 计算 deadline 存入全局 `_timers` 后 yield。**这里有明显缺陷**：`sleep` 用 `tostring(co):match("0x(%x+)")` 当 timer key，依赖协程地址字符串，地址复用会导致 timer 串号；`_timers` 是隐式全局变量。
- **TCP 服务器**：`create_server(port, label, on_accept)` 建 `socket.tcp()`、`setoption("reuseaddr")`、`bind`、`listen(max_conn)`、`settimeout(0)` 非阻塞，然后 spawn 一个 acceptor 协程死循环 `accept()`，`err == "timeout"` 时 `wait_readable`。
- **Game 连接处理**：`handle_game_connection` 为每连接维护 `recv_buf` 字符串，循环 `client:receive("*a")`，把 `data`/`partial` 追加到 buffer，然后内层 while 解帧：`peek_len` 取 `total_len`，校验 `< HEADER_SIZE` 或 `> MAX_MSG_SIZE`（256 KiB）则丢弃整个 buffer，`#recv_buf < total_len` 则 break 等更多数据，否则 `sub(1,total_len)` 切出消息、`protocol.unpack`、`handle_game_message`。**缺陷**：`recv_buf = recv_buf .. data` 是 O(n²) 字符串拼接，`sub` 又产生新字符串，GC 压力极大；校验失败直接清空整个缓冲区会丢掉后续正常消息。
- **消息分发** `handle_game_message`：PING → `health.heartbeat_received` + 回 PONG；PONG → 记心跳；`MSG_SERVICE_REGISTER` → 用 `gmatch("[^|]+")` 按 `|` 分割 `service_id|service_type|host|port|metadata` 后调 `registry.register`；同理处理 unregister / query / heartbeat / cross-region / cluster 系列。**缺陷**：管道分隔的文本协议，字段内含 `|` 即解析错误，且 `gmatch("[^|]+")` 会吞掉空字段导致字段错位。
- **后台 loop**：`cluster_sync_loop`（周期性向 peer 推注册表）、`service_cleanup_loop`（周期清理过期服务）、`stats_reporter_loop`（打印统计）、`main_loop`（select 驱动所有协程）、`setup_shutdown_watcher` / `graceful_shutdown`。

**迁移判定：**

| 逻辑 | 处置 | 说明 |
|---|---|---|
| CLI 解析 | 迁移到 C | `ce_router_config_parse_argv()` |
| 日志 | **废弃** | 改用引擎统一 `CE_LOG_INFO("ROUTER", ...)` |
| 协程调度器 + `wait_readable` + `sleep` | **废弃** | 由 io_uring 完成队列 + 定时器轮取代，无需协程 |
| select 事件循环 | **废弃** | 违反约束 C2，改 `ce_async_wait()` |
| TCP 监听/accept | 迁移到 C | `ce_async_accept()` |
| 字符串拼接式解帧 | 迁移到 C 并重构 | 固定 ring buffer，零拷贝解帧 |
| 管道分隔文本协议 | **废弃** | 改二进制定长结构体，定义在 `ce_game_protocol.h` |
| 消息分发表 | 迁移到 C | 数组跳转表替代 if-elseif 链 |
| 后台 loop | 迁移到 C | 统一进 tick 定时器 |

### 1.3 `cluster.lua`（496 行）— 集群与伪选举

**现有逻辑：**

- `new_node(node_id, host, port, region)` 造节点表；模块级 `peers` 表、`local_node`、`leader_id`、`running` 标志。
- `add_peer / remove_peer / get_peer / get_all_peers / get_alive_peers`：纯表操作。
- `connect_peers()`：遍历 `peers`，对非 `connected` 的建 `socket.tcp()`、`settimeout(3)` 同步 connect、成功后 `settimeout(0)` 转非阻塞，置 `state="connected"`、`alive=true`，发 `MSG_CLUSTER_JOIN`（`node_id|host|port|region`），最后 `elect_leader()`。**缺陷**：3 秒同步 connect 会阻塞整个单线程进程，N 个不可达 peer 就是 3N 秒卡死。
- **`elect_leader()` — 伪选举**：把本节点 id 和所有 `alive` peer 的 id 收集成 `candidates`，`table.sort` 字典序，取 `candidates[1]` 作为 leader。**这是本 spec 要替换的核心问题**：
  - 无任期（term）概念，无法区分新旧 Leader。
  - 无投票、无 quorum 检查：网络分区两侧各自 `elect_leader()`，各自认为自己那一侧的最小 id 是 Leader → **必然脑裂，双 Leader 同时写**。
  - 选举结果依赖 `alive` 标志，而 `alive` 由心跳超时驱动，收敛时间不可控。
  - 无日志复制，Leader 切换后状态（注册表、路由表）不保证连续。
- `handle_message(from_node_id, msg_type, body)`：处理 `MSG_CLUSTER_JOIN`（管道解析后 `add_peer`）、`MSG_CLUSTER_LEAVE`、`MSG_CLUSTER_SYNC`（`handle_sync_data`）、`MSG_CLUSTER_ELECT`（直接 `elect_leader()`）、`MSG_ROUTER_HELLO`（更新 `last_seen`）、`MSG_ROUTER_BYE`。
- `stats()`：汇总 peer 状态；`_log(level,msg)` 又是一套独立日志。

**迁移判定：** peer 表管理迁移到 C（`ce_router_cluster.c`）；同步 connect **废弃**，改 io_uring 异步 connect；`elect_leader()` 及 `MSG_CLUSTER_ELECT` **整体废弃**，由 `ce_raft` 取代；`MSG_CLUSTER_SYNC` 全量推送 **废弃**，改 Raft AppendEntries 增量复制；`_log` **废弃**。

### 1.4 `hash_ring.lua`（307 行）— 一致性哈希环

**现有逻辑：** `try_load_md5()` 尝试 require md5 库，失败回落 `hash_djb2`（`h = h*33 + byte`，取 32 位）。`add_node(node_id, vnode_count, weight)` 生成 `vnode_count * weight` 个虚拟节点（默认 `config.hash_ring_vnodes = 150`），每个 vnode 的 key 是 `node_id#i`，插入有序数组后重新排序。`remove_node` 线性扫描删除。`find_node(target_hash)` 在有序数组上二分找第一个 `hash >= target`，找不到回绕到首元素。`lookup(key)` = `find_node(hash_str(key))`。`lookup_n(key, n)` 从落点顺序向后取 n 个**不同**物理节点（跳过重复）。`update_weight` 通过「先删再加」实现。

**迁移判定：** **全部迁移到 C**，这是路由热路径。C 版改进点：
- 哈希函数固定为 xxHash64 或 MurmurHash3_x86_32（去掉 md5 可选依赖带来的**跨节点哈希不一致风险**——两个 Router 一个装了 md5 一个没装，同一 key 会路由到不同节点，这是现实存在的严重 bug）。
- vnode 数组预分配 + 一次性排序，`add_node` 不再每次全量 `table.sort`。
- `lookup` 目标 O(log V)，V = 节点数 × 150。3 节点 = 450 vnode，二分 9 次比较，可完全放进 L1 cache。
### 1.5 `router.lua`（251 行）— 路由策略

**现有逻辑：** 模块级 `connections` 表（`conn_id → {sock, service_id, host, port, registered_at}`）。
- `register_connection(conn_id, sock, service_id, host, port)` / `unregister_connection` / `get_connection`。
- `find_connection_by_service(service_id)`：**线性遍历** `connections` 找匹配，O(N)。
- `route_to_service(service_id, msg_type, body)`：先 `find_connection_by_service` 再 `route_to_connection`。
- `route_to_connection(conn_id, msg_type, body)`：`protocol.pack` 后 `sock:send`，pcall 包裹。
- `route_by_hash(key, msg_type, body)`：`hash_ring.lookup(key)` 得 node_id（此处 node_id 即 service_id），再 `route_to_service`。
- `route_to_replicas(key, n, msg_type, body)`：`hash_ring.lookup_n` 后逐个发送。

**迁移判定：** 全部迁移到 C。改进点：`service_id → conn` 建哈希索引消除 O(N) 线性查找；`sock:send` 改 io_uring 异步 send + 每连接发送队列（当前实现遇到 TCP 发送缓冲满时 LuaSocket 会返回部分发送，代码里 pcall 直接吞掉错误 → **静默丢消息**，C 版必须实现 partial-send 续传）。

### 1.6 `registry.lua`（320 行）— 服务注册表

**现有逻辑：** `new_entry(service_id, service_type, host, port, metadata)`，字段含 `alive`、`registered_at`、`last_heartbeat`。`register` 重复注册返回 false。`unregister` 删除。`heartbeat(service_id)` 更新 `last_heartbeat`。`cleanup_stale(timeout_sec)` 遍历把超时的置 `alive=false` 并返回计数。`purge_dead()` 真正移除。`get(service_id)` / `query(service_type, alive_only)` 遍历过滤。

**迁移判定：** 全部迁移到 C。注册表内容需要**成为 Raft 状态机的一部分**（当前是各 Router 本地表 + 全量同步，Leader 切换后可能丢注册信息）。改进：按 `service_type` 建二级索引，避免 `query` 全表扫描。

### 1.7 `health.lua`（307 行）— 心跳

**现有逻辑：** `DEFAULT_INTERVAL = 1.0`，`DEFAULT_TIMEOUT = 3.0`。`new_tracker(conn_id, label, on_timeout)`，`tracker_pong` 更新时间戳，`tracker_is_timeout` 比较。`register / heartbeat_received / unregister / is_alive`，以及扫描所有 tracker 触发 `on_timeout` 回调的 tick 函数。

**迁移判定：** **并入 C 版连接层**，不单独成模块。C 版用「时间轮（timing wheel）」替代全表扫描：1024 槽 × 10ms 精度，O(1) 插入与到期检查，避免连接数上万时每 tick 遍历。

### 1.8 `cross_region.lua`（427 行）— 跨区域路由

**现有逻辑：** `DEFAULT_TTL = 16`、`MAX_HOPS = 32`。`add_region / remove_region / get_region / get_all_regions`。`connect_region(region)` 同步 connect 远端 Router（同样阻塞问题）。`connect_all_regions` / `disconnect_region` / `disconnect_all`。跨区域消息封装：在 body 前加 `region|ttl|hop_count` 头，转发时 TTL 递减，到 0 丢弃防环。

**迁移判定：** 迁移到 C。跨区域链路**不加入本地 Raft group**（决策 D2），保持最终一致语义。TTL/hop 防环逻辑保留但改二进制头。

### 1.9 `protocol.lua`（341 行）— 协议编解码

**现有逻辑：** `HEADER_SIZE = 6`（4B total_len 大端 + 2B msg_type 大端），`MAX_MSG_SIZE = 256 KiB`。消息类型常量：
```
MSG_PING 0x0001  MSG_PONG 0x0002  MSG_LOGIN 0x0010  MSG_LOGIN_RESP 0x0011
MSG_GAME_DATA 0x0100  MSG_DISCONNECT 0xFFFF
MSG_CROSS_REGION 0x1000  MSG_REGION_SYNC 0x1001  MSG_ROUTER_HELLO 0x1002  MSG_ROUTER_BYE 0x1003
MSG_CLUSTER_SYNC 0x2000  MSG_CLUSTER_JOIN 0x2001  MSG_CLUSTER_LEAVE 0x2002  MSG_CLUSTER_ELECT 0x2003
MSG_SERVICE_REGISTER 0x3000 ... MSG_SERVICE_HEARTBEAT 0x3004
MSG_USER_BASE 0x8000
```
`pack_u16/pack_u32/unpack_u16/unpack_u32` 手写大端；`pack(msg_type, body)`、`unpack(data)`、`peek_len(data)`。

**迁移判定：** **模块整体废弃**。帧格式 `[4B total_len][2B msg_type][payload]` 与 `ce_game_protocol.h` 中的 `CE_GAME_HEADER_SIZE = 6` 完全一致，说明两套代码在描述同一协议——这正是约束 C4 要消灭的重复。所有消息类型常量与结构体**上移到 `src_c/server/ce_game_protocol.h`**，Lua 侧若仍需（仅压测脚本）通过 C 绑定读取，不再手抄。

注意 `MSG_USER_BASE = 0x8000` 与 `ce_game_protocol.h` 现有的 `MSG_JOIN_RESPONSE = 0x8001`、`MSG_ENTITY_STATE = 0x8002` **已经冲突**（Lua 把 0x8000+ 当用户自定义区，C 侧当服务器→客户端区）。合并时必须重新划分号段，见第 2.5 节。

### 1.10 `config.lua`（97 行）— 配置

字段：`node_id="router-1"`、`region="default"`、`game_port=9100`、`cluster_port=9101`、`peers={}`、`remote_regions={}`、`hash_ring_vnodes=150`、`heartbeat_interval=1`、`heartbeat_timeout=3`、`max_game_connections=1024`、`max_cluster_connections=64`、`recv_buf_size=128KiB`、`send_buf_size=128KiB`、`log_level="info"`、`version="0.1.0"`。

**迁移判定：** 迁移到 C 结构体 + 简易 INI/KV 文件解析器。**`cluster_port=9101` 废弃**：按约束 C3，Router 只监听 9100 一个端口，靠首帧 magic 区分 Game 面 / Raft 面 / 跨区域面（见 2.4）。

### 1.11 现状缺陷汇总（迁移必须修掉）

| # | 缺陷 | 严重度 | C 版对策 |
|---|---|---|---|
| B1 | 伪选举必然脑裂（分区两侧各自出 Leader） | 致命 | 真 Raft，quorum 投票（第 3 章） |
| B2 | 无任期概念，旧 Leader 复活后继续写 | 致命 | term 单调递增 + 拒绝低 term 请求 |
| B3 | md5 可选依赖导致跨节点哈希环不一致 | 致命 | 固定 xxHash64，无可选路径 |
| B4 | 同步 connect 阻塞单线程进程（3N 秒） | 高 | io_uring 异步 connect |
| B5 | `recv_buf` 字符串拼接 O(n²) + GC 压力 | 高 | 固定 ring buffer 零拷贝 |
| B6 | 解帧校验失败清空整个 buffer，丢后续消息 | 高 | 按帧丢弃并重同步，或直接断连 |
| B7 | 管道分隔文本协议，字段含 `\|` 或空字段即错位 | 高 | 二进制定长结构体 |
| B8 | `sock:send` pcall 吞错，partial send 静默丢消息 | 高 | 发送队列 + partial-send 续传 |
| B9 | `find_connection_by_service` O(N) 线性扫描 | 中 | 哈希索引 |
| B10 | `sleep` 用协程地址字符串当 timer key，地址复用串号 | 中 | 时间轮，句柄化 timer id |
| B11 | 三套独立日志实现（init/cluster/其它） | 中 | 统一 `CE_LOG_*(cat, ...)` |
| B12 | 心跳全表扫描 | 中 | 时间轮 O(1) |
| B13 | 无实体位置映射，Gateway 无法智能路由 | 高 | 第 4 章实体路由表 |

---

## 2. 子项 2.1：Router 迁移到纯 C（io_uring）

### 2.1 目录与文件清单

新建 `src_c/router/`，共 8 个 `.c` + 8 个 `.h`：

| 文件 | 职责 | 约行数 |
|---|---|---|
| `ce_router.c/h` | Router 实例生命周期、主循环、模块编排、对外总入口 | 600 |
| `ce_router_config.c/h` | 配置结构体、命令行与配置文件解析、集群成员静态表 | 300 |
| `ce_router_conn.c/h` | 连接对象、ring buffer 收发、解帧、发送队列与 partial-send 续传、时间轮心跳 | 700 |
| `ce_router_dispatch.c/h` | 消息类型 → handler 跳转表，三个协议面的分发 | 400 |
| `ce_router_registry.c/h` | 服务注册表（Raft 状态机的一部分），按 type 二级索引 | 450 |
| `ce_router_hashring.c/h` | 一致性哈希环，vnode + 权重，lookup / lookup_n | 350 |
| `ce_router_cluster.c/h` | peer 连接管理、跨区域链路、TTL 防环转发 | 550 |
| `ce_router_stats.c/h` | 指标采集（QPS、延迟直方图、Raft 状态），供 admin-web 拉取 | 250 |

外加两个由 2.2 / 2.3 引入：`ce_raft.c/h`、`ce_router_entity_table.c/h`。

### 2.2 `ce_router.h` — 实例与主循环

```c
#ifndef CE_ROUTER_H
#define CE_ROUTER_H

#include "public_api/ce_types.h"
#include "network/ce_async_io.h"
#include "router/ce_router_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CeRouter CeRouter;   /* 不透明句柄 */

/** Router 运行角色（由 Raft 决定，只读） */
typedef enum CeRouterRole {
    CE_ROUTER_ROLE_FOLLOWER  = 0,
    CE_ROUTER_ROLE_CANDIDATE = 1,
    CE_ROUTER_ROLE_LEADER    = 2,
} CeRouterRole;

/** 创建 Router 实例（不启动监听） */
CeRouter*   ce_router_create(const CeRouterConfig* cfg);

/** 销毁实例，释放所有连接与 Raft 状态 */
void        ce_router_destroy(CeRouter* r);

/** 启动：绑定单一端口、注册首批 accept SQE、初始化 Raft */
CeResult    ce_router_start(CeRouter* r);

/** 请求停机（信号处理器里调用，仅置标志，异步安全） */
void        ce_router_request_stop(CeRouter* r);

/** 运行主循环，阻塞直到 stop。内部即 submit → wait → 处理事件 → tick */
CeResult    ce_router_run(CeRouter* r);

/** 单次迭代（供单元测试驱动，不进死循环）
 *  @param timeout_ms  传给 ce_async_wait 的超时 */
CeResult    ce_router_tick(CeRouter* r, int timeout_ms);

/* ---- 只读查询 ---- */
CeRouterRole ce_router_role(const CeRouter* r);
const char*  ce_router_leader_id(const CeRouter* r);   /* 无 Leader 返回 NULL */
uint64_t     ce_router_current_term(const CeRouter* r);

#ifdef __cplusplus
}
#endif
#endif /* CE_ROUTER_H */
```

主循环骨架（伪代码，说明 io_uring 用法，**非交付代码**）：

```
while (!r->stop_requested) {
    ce_async_submit(r->async);                     /* 提交积压 SQE */
    int n = ce_async_wait(r->async, 1, tick_ms);   /* 等完成事件 */
    for (int i = 0; i < n; ++i) {
        const CeAsyncEvent* ev = ce_async_get_event(r->async, i);
        switch (ev->type) {
        case CE_ASYNC_ACCEPT: on_accept(r, ev); break;   /* 立即补发新 accept SQE */
        case CE_ASYNC_RECV:   on_recv(r, ev);   break;   /* 解帧 → dispatch */
        case CE_ASYNC_SEND:   on_send(r, ev);   break;   /* partial-send 续传 */
        case CE_ASYNC_CLOSE:  on_close(r, ev);  break;
        case CE_ASYNC_ERROR:
            CE_LOG_ERROR("ROUTER", "async 事件错误 fd=%d errno=%d", ev->fd, ev->error);
            on_close(r, ev);
            break;
        default: break;
        }
    }
    ce_router_timerwheel_advance(r);   /* 心跳超时、Raft 选举/心跳超时、注册表过期 */
}
```

**关于 io_uring（约束 C2）**：全部 I/O 只经由 `ce_async_*` 接口，`src_c/router/` 中**不得出现** `epoll_create`、`epoll_ctl`、`epoll_wait`、`select`、`poll` 任何符号。CI 中加一条 grep 门禁（第 5.3 节）。Linux 后端为 `ce_async_uring.c`（liburing 2.14），POSIX 回落仅用于开发机无 io_uring 的情形，Windows 后端由抽象层的 IOCP 实现负责。可用时启用 `ce_async_register_buffers()` 固定缓冲区与 `ce_async_has_zcrx()` 零拷贝接收。
### 2.3 `ce_router_conn.h` — 连接层（零拷贝解帧 + 发送队列）

```c
#ifndef CE_ROUTER_CONN_H
#define CE_ROUTER_CONN_H

#include "public_api/ce_types.h"
#include <stdint.h>

#define CE_ROUTER_RECV_BUF_SIZE   (128 * 1024)   /* 对齐 config.lua recv_buf_size */
#define CE_ROUTER_SEND_BUF_SIZE   (128 * 1024)
#define CE_ROUTER_MAX_MSG_SIZE    (256 * 1024)   /* 对齐 protocol.lua MAX_MSG_SIZE */
#define CE_ROUTER_MAX_CONNS       4096

/** 连接所属协议面（首帧 magic 决定，见 2.4） */
typedef enum CeRouterFace {
    CE_ROUTER_FACE_UNKNOWN = 0,
    CE_ROUTER_FACE_GAME    = 1,   /* Game/Gateway/DBProxy 等业务进程 */
    CE_ROUTER_FACE_RAFT    = 2,   /* 同 region 内 Router peer */
    CE_ROUTER_FACE_REGION  = 3,   /* 跨 region Router */
} CeRouterFace;

typedef enum CeRouterConnState {
    CE_CONN_STATE_HANDSHAKE = 0,
    CE_CONN_STATE_ACTIVE    = 1,
    CE_CONN_STATE_DRAINING  = 2,  /* 已发 BYE，等发送队列排空 */
    CE_CONN_STATE_CLOSED    = 3,
} CeRouterConnState;

/** 单条连接。固定数组池分配，索引即 slot，不用 malloc */
typedef struct CeRouterConn {
    int                 fd;
    uint32_t            slot;          /* 池内下标 */
    uint32_t            generation;    /* slot 复用代号，防悬垂 user_data */
    CeRouterFace        face;
    CeRouterConnState   state;

    /* 收：单缓冲 + 读写游标，解帧原地进行，不拷贝 */
    uint8_t*            recv_buf;      /* 容量 CE_ROUTER_RECV_BUF_SIZE */
    uint32_t            recv_len;      /* 已填充字节 */
    uint32_t            recv_parsed;   /* 已消费字节，供 compact 用 */

    /* 发：环形缓冲 + in-flight 标志，处理 partial send（修 B8） */
    uint8_t*            send_buf;
    uint32_t            send_head;
    uint32_t            send_tail;
    CeBool              send_in_flight;

    /* 身份 */
    char                service_id[64];
    char                service_type[32];
    char                node_id[64];    /* RAFT/REGION 面用 */
    char                peer_addr[64];   /* "ip:port"，等价旧 conn_id */

    /* 心跳（时间轮，修 B12） */
    uint64_t            last_recv_ms;
    uint32_t            tw_slot;        /* 时间轮槽位 */

    uint64_t            bytes_in;
    uint64_t            bytes_out;
    uint64_t            msgs_in;
    uint64_t            msgs_out;
} CeRouterConn;

/** 连接池 */
typedef struct CeRouterConnPool CeRouterConnPool;

CeRouterConnPool* ce_router_conn_pool_create(uint32_t max_conns);
void              ce_router_conn_pool_destroy(CeRouterConnPool* pool);

CeRouterConn*     ce_router_conn_acquire(CeRouterConnPool* pool, int fd);
void              ce_router_conn_release(CeRouterConnPool* pool, CeRouterConn* c);
CeRouterConn*     ce_router_conn_by_slot(CeRouterConnPool* pool, uint32_t slot, uint32_t gen);

/** service_id → 连接 的哈希索引（修 B9，O(1) 替代线性扫描） */
CeResult          ce_router_conn_index_put(CeRouterConnPool* pool, const char* service_id, CeRouterConn* c);
CeRouterConn*     ce_router_conn_index_get(CeRouterConnPool* pool, const char* service_id);
void              ce_router_conn_index_del(CeRouterConnPool* pool, const char* service_id);

/* ---- 解帧 ---- */

/** 从 recv_buf 中取出下一个完整帧（零拷贝，返回内部指针）
 *  @param out_type     消息类型
 *  @param out_payload  指向 recv_buf 内部，调用者不得持有跨 tick
 *  @param out_len      payload 长度
 *  @return CE_OK 取到一帧；CE_ERR_AGAIN 数据不足；CE_ERR 协议错误（调用者应断连，修 B6） */
CeResult ce_router_conn_next_frame(CeRouterConn* c, uint16_t* out_type,
                                   const uint8_t** out_payload, uint32_t* out_len);

/** 消费完若干帧后整理缓冲区（memmove 剩余字节到头部） */
void     ce_router_conn_compact(CeRouterConn* c);

/* ---- 发送 ---- */

/** 把一帧压入发送队列；队列满返回 CE_ERR_FULL（**绝不静默丢弃**，修 B8） */
CeResult ce_router_conn_enqueue(CeRouterConn* c, uint16_t msg_type,
                                 const void* payload, uint32_t len);

/** 若队列非空且无 in-flight 发送，则提交一次 ce_async_send */
void     ce_router_conn_flush(struct CeRouter* r, CeRouterConn* c);

/** send 完成回调：推进 send_head，若仍有剩余则继续 flush（partial-send 续传） */
void     ce_router_conn_on_sent(struct CeRouter* r, CeRouterConn* c, int nbytes);

#endif /* CE_ROUTER_CONN_H */
```

**关于 `user_data`**：`ce_async_*` 的 `user_data` 编码为 `(slot << 32) | generation`，完成事件回来时用 `ce_router_conn_by_slot()` 校验 generation。这样 fd 关闭后延迟到达的 CQE 不会误操作已复用的 slot——io_uring 场景下这是必须处理的经典问题，epoll 版本代码里没有对应逻辑。

### 2.4 单端口多协议面复用（约束 C3）

Gateway 已在 9000 端口共用 TCP/KCP/WebSocket。Router 沿用同一思路：**只监听 9100**，废弃 9101。连接建立后读首 8 字节 magic 判定协议面：

| Magic（8B ASCII） | 协议面 | 用途 |
|---|---|---|
| `CEROUTER` | `FACE_GAME` | Game / Gateway / DBProxy 注册与路由查询 |
| `CERAFT01` | `FACE_RAFT` | 同 region Router peer 的 Raft RPC |
| `CEREGION` | `FACE_REGION` | 跨 region Router 转发链路 |

判定规则：`HANDSHAKE` 状态下累积到 8 字节即比对；不匹配任何 magic 则立即断连并 `CE_LOG_WARN("ROUTER", "未知协议面 magic，断开 %s", c->peer_addr)`。首 8 字节被消费掉，之后统一按 `[4B total_len][2B msg_type][payload]` 解帧。

好处：部署只开一个端口（防火墙/K8s Service 配置简化）；Raft 面与业务面共享同一套连接层、时间轮与统计代码，符合约束 C4「公共代码只写一份」。

风险与对策：Raft 面与业务面共享 accept 队列，业务侧连接风暴可能挤压 Raft 心跳。对策——`FACE_RAFT` 连接在分发时优先处理（事件循环内先扫 Raft 面事件），且 Raft peer 连接数上限单独计（配置成员表大小 - 1），不占业务连接配额。

### 2.5 协议扩展：全部落在 `ce_game_protocol.h`（约束 C4）

`src_c/server/ce_game_protocol.h` 是协议常量与结构体的**唯一权威来源**。当前它只定义了 4 个消息（`MSG_JOIN_REQUEST 0x0001`、`MSG_POSITION_UPDATE 0x0002`、`MSG_JOIN_RESPONSE 0x8001`、`MSG_ENTITY_STATE 0x8002`）与 `CE_GAME_HEADER_SIZE 6`。Router 相关消息按下列**重新划分的号段**追加到同一枚举/同一头文件，`protocol.lua` 的常量表随之删除：

| 号段 | 用途 | 说明 |
|---|---|---|
| `0x0001–0x00FF` | 客户端 → 服务器（现有） | 保持不变 |
| `0x0100–0x01FF` | 通用控制 | PING / PONG / DISCONNECT |
| `0x3000–0x30FF` | 服务注册与发现 | REGISTER / UNREGISTER / QUERY / QUERY_RESP / HEARTBEAT |
| `0x4000–0x40FF` | **Raft RPC**（新增） | REQUEST_VOTE / VOTE_RESP / APPEND_ENTRIES / APPEND_RESP |
| `0x5000–0x50FF` | **实体路由表**（新增） | LOOKUP / LOOKUP_RESP / BIND / UNBIND / BATCH_LOOKUP |
| `0x6000–0x60FF` | 跨区域 | CROSS_REGION_FWD / REGION_SYNC |
| `0x8001–0x8FFF` | 服务器 → 客户端（现有） | 保持不变，**旧 `MSG_USER_BASE = 0x8000` 废弃** |
| `0x9000–0x9FFF` | 用户/插件自定义 | 新的用户扩展区，替代废弃的 0x8000 |

追加内容示意（写进 `ce_game_protocol.h` 的 `CeGameMsgType` 枚举，客户端与服务端**同一份 include**）：

```c
    /* ---- 通用控制 ---- */
    MSG_PING                 = 0x0100,
    MSG_PONG                 = 0x0101,
    MSG_DISCONNECT           = 0x0102,

    /* ---- 服务注册与发现 ---- */
    MSG_SERVICE_REGISTER     = 0x3000,
    MSG_SERVICE_UNREGISTER   = 0x3001,
    MSG_SERVICE_QUERY        = 0x3002,
    MSG_SERVICE_QUERY_RESP   = 0x3003,
    MSG_SERVICE_HEARTBEAT    = 0x3004,

    /* ---- Raft RPC（2.2） ---- */
    MSG_RAFT_REQUEST_VOTE    = 0x4000,
    MSG_RAFT_VOTE_RESP       = 0x4001,
    MSG_RAFT_APPEND_ENTRIES  = 0x4002,
    MSG_RAFT_APPEND_RESP     = 0x4003,

    /* ---- 实体路由表（2.3） ---- */
    MSG_ENTITY_LOOKUP        = 0x5000,
    MSG_ENTITY_LOOKUP_RESP   = 0x5001,
    MSG_ENTITY_BIND          = 0x5002,
    MSG_ENTITY_BIND_RESP     = 0x5003,
    MSG_ENTITY_UNBIND        = 0x5004,
    MSG_ENTITY_BATCH_LOOKUP  = 0x5005,
    MSG_ENTITY_BATCH_RESP    = 0x5006,

    /* ---- 跨区域 ---- */
    MSG_CROSS_REGION_FWD     = 0x6000,
    MSG_REGION_SYNC          = 0x6001,
```

所有新增载荷结构体（`CeRaftRequestVote` 等，见第 3/4 章）同样定义在此头文件中。**字节序统一大端**，字段使用定长 `uint32_t/uint64_t`，结构体内不留隐式 padding（必要时显式补 `uint8_t _pad[N]`），彻底废弃管道分隔文本（修 B7）。

### 2.6 `ce_router_hashring.h` — 一致性哈希环

```c
#ifndef CE_ROUTER_HASHRING_H
#define CE_ROUTER_HASHRING_H

#include "public_api/ce_types.h"
#include <stdint.h>

#define CE_HASHRING_DEFAULT_VNODES  150   /* 对齐 config.lua hash_ring_vnodes */
#define CE_HASHRING_MAX_NODES       64
#define CE_HASHRING_MAX_VNODES      (CE_HASHRING_MAX_NODES * 512)

/** 虚拟节点条目，8 字节紧凑排布，利于 cache 与二分 */
typedef struct CeHashRingVNode {
    uint32_t hash;        /* xxHash32("node_id#i") */
    uint16_t node_index;  /* 指向 nodes[] 下标，不存字符串 */
    uint16_t _pad;
} CeHashRingVNode;

typedef struct CeHashRingNode {
    char     node_id[64];
    float    weight;
    uint16_t vnode_count;
    CeBool   alive;
} CeHashRingNode;

typedef struct CeHashRing {
    CeHashRingNode   nodes[CE_HASHRING_MAX_NODES];
    uint16_t         node_count;
    CeHashRingVNode* vnodes;       /* 按 hash 升序 */
    uint32_t         vnode_count;
    uint32_t         vnode_cap;
    CeBool           dirty;        /* 需要重排序 */
} CeHashRing;

CeResult    ce_hashring_init(CeHashRing* ring, uint32_t vnode_cap);
void        ce_hashring_free(CeHashRing* ring);

/** 添加节点。vnode_count=0 时用默认值 × weight */
CeResult    ce_hashring_add_node(CeHashRing* ring, const char* node_id,
                                  uint16_t vnode_count, float weight);
CeResult    ce_hashring_remove_node(CeHashRing* ring, const char* node_id);
CeResult    ce_hashring_update_weight(CeHashRing* ring, const char* node_id, float weight);

/** 批量变更后调用一次，完成排序（避免每次 add 都全量排序，修 hash_ring.lua 的低效） */
void        ce_hashring_rebuild(CeHashRing* ring);

/** 查找 key 的归属节点。O(log V) 二分。ring 为空返回 NULL */
const char* ce_hashring_lookup(const CeHashRing* ring, const char* key, uint32_t key_len);

/** 查找 N 个互不相同的物理节点（副本用）
 *  @param out_node_ids  调用者提供的指针数组，容量 >= n
 *  @return 实际填充数量 */
int         ce_hashring_lookup_n(const CeHashRing* ring, const char* key, uint32_t key_len,
                                  int n, const char** out_node_ids);

/** 固定哈希函数：xxHash32。**不提供可替换实现**，杜绝跨节点哈希不一致（修 B3） */
uint32_t    ce_hashring_hash(const void* data, uint32_t len);

#endif /* CE_ROUTER_HASHRING_H */
```

### 2.7 其余头文件要点

**`ce_router_config.h`**
```c
typedef struct CeRouterPeerCfg {
    char     node_id[64];
    char     host[64];
    uint16_t port;
    char     region[32];
} CeRouterPeerCfg;

typedef struct CeRouterConfig {
    char             node_id[64];
    char             region[32];
    uint16_t         listen_port;              /* 默认 9100，单端口（C3） */
    CeRouterPeerCfg  peers[8];                 /* Raft 静态成员表（决策 D1） */
    uint8_t          peer_count;               /* 含自身，须为 3 或 5（D2） */
    CeRouterPeerCfg  remote_regions[16];
    uint8_t          remote_region_count;
    uint16_t         hashring_vnodes;          /* 150 */
    uint32_t         heartbeat_interval_ms;    /* 1000 */
    uint32_t         heartbeat_timeout_ms;     /* 3000 */
    uint32_t         max_connections;          /* 4096 */
    /* Raft 调参 */
    uint32_t         raft_election_timeout_min_ms;  /* 150 */
    uint32_t         raft_election_timeout_max_ms;  /* 300 */
    uint32_t         raft_heartbeat_interval_ms;    /* 50 */
    char             raft_log_dir[256];             /* 持久化目录 */
    /* 实体表 */
    uint32_t         entity_table_buckets;          /* 1<<20 */
    uint32_t         entity_cache_entries;          /* 65536 */
} CeRouterConfig;

void     ce_router_config_defaults(CeRouterConfig* cfg);
CeResult ce_router_config_parse_argv(CeRouterConfig* cfg, int argc, char** argv);
CeResult ce_router_config_load_file(CeRouterConfig* cfg, const char* path);
CeResult ce_router_config_validate(const CeRouterConfig* cfg);  /* 校验 peer_count 为奇数等 */
```

**`ce_router_registry.h`**
```c
typedef struct CeServiceEntry {
    char     service_id[64];
    char     service_type[32];
    char     host[64];
    uint16_t port;
    CeBool   alive;
    uint64_t registered_at_ms;
    uint64_t last_heartbeat_ms;
    char     metadata[128];
} CeServiceEntry;

typedef struct CeServiceRegistry CeServiceRegistry;

CeServiceRegistry* ce_registry_create(uint32_t capacity);
void               ce_registry_destroy(CeServiceRegistry* reg);
CeResult           ce_registry_put(CeServiceRegistry* reg, const CeServiceEntry* e);
CeResult           ce_registry_remove(CeServiceRegistry* reg, const char* service_id);
const CeServiceEntry* ce_registry_get(const CeServiceRegistry* reg, const char* service_id);
CeResult           ce_registry_heartbeat(CeServiceRegistry* reg, const char* service_id, uint64_t now_ms);
/** 按类型查询（二级索引，非全表扫描） */
int                ce_registry_query_by_type(const CeServiceRegistry* reg, const char* service_type,
                                              CeBool alive_only, const CeServiceEntry** out, int max_out);
/** 标记超时服务为 dead，返回数量 */
int                ce_registry_mark_stale(CeServiceRegistry* reg, uint64_t now_ms, uint32_t timeout_ms);
int                ce_registry_purge_dead(CeServiceRegistry* reg);
```

**`ce_router_cluster.h`** 提供 `ce_cluster_connect_peers()`（**异步** connect，修 B4）、`ce_cluster_on_peer_up/down()`、`ce_cluster_forward_cross_region(region, msg_type, payload, len, ttl)`（TTL 递减防环，`ttl<=0` 丢弃并告警）。

**`ce_router_dispatch.h`** 提供 `typedef CeResult (*CeRouterHandler)(CeRouter*, CeRouterConn*, uint16_t, const uint8_t*, uint32_t);` 与按号段建立的三张跳转表（GAME / RAFT / REGION），替换 Lua 的 if-elseif 链。
---

## 3. 子项 2.2：Raft 共识（`ce_raft.c/h`）

### 3.1 替换目标

当前 `cluster.lua:elect_leader()` 是「存活节点中最小 node_id 字典序 wins」的伪选举，缺陷已在 1.3 与 B1/B2 说明：**无任期、无投票、无 quorum、无日志、分区必脑裂**。本节以自研精简版 Raft 取代（决策 D1）：

- **做**：Leader 选举（term + 随机化选举超时 + RequestVote）、日志复制（AppendEntries）、提交索引推进、持久化、脑裂防护。
- **不做**：快照压缩（Log Compaction）、成员动态变更（joint consensus）、Learner/只读副本、PreVote 优化（v0.1 不做，v0.2 视需要加）。
- **不引入**：etcd、braft、NuRaft 等任何 C++/Go 依赖。纯 C99 + `ce_async_io`。

### 3.2 状态机图

```
                            启动 / 恢复持久化状态
                                     |
                                     v
                       +-------------------------+
       收到 term 更大的  |                         |  选举超时(随机 150~300ms)到期
       任何 RPC，或发现  |        FOLLOWER         |  且未收到 Leader 心跳
       更高 term 的 Leader|                        |----------------+
            +----------->|  currentTerm 已知        |                |
            |            |  votedFor 可能已投       |                |
            |            +-------------------------+                |
            |                        ^                             v
            |                        |                +-------------------------+
            |     发现更高 term       |                |                         |
            |     (收到 term > me)   |                |       CANDIDATE          |
            |                        +----------------|  currentTerm += 1        |
            |                        |   选举失败/     |  votedFor = self         |
            |                        |   收到合法      |  广播 RequestVote        |
            |                        |   Leader 心跳   |  重置随机选举计时器       |
            |                        |                +-------------------------+
            |                        |                             |
            |                        |          获得 quorum 投票   |
            |                        |         (>= N/2 + 1，含自身)|
            |                        |                             v
            |            +-------------------------+   +-------------------------+
            |            |                         |   |                         |
            +------------|         LEADER          |<--+   成为 Leader:          |
                         |  周期(50ms) 广播        |       nextIndex[i]=lastLog+1
                         |  AppendEntries 心跳      |       matchIndex[i]=0
                         |  复制日志、推进 commitIdx |       追加 no-op 条目确立权威
                         |                         |
                         +-------------------------+
                                     |
                     心跳无法获得 quorum 响应 (选举超时 × 2)
                     → 主动降级为 FOLLOWER（防止孤立分区中的
                       旧 Leader 继续对外声称自己是 Leader）
                                     |
                                     v
                                 FOLLOWER
```

**三条不变式（实现必须断言）：**
1. **Election Safety**：任一 term 内最多一个 Leader。由「每节点每 term 只投一票（`votedFor` 持久化）」+「须获 quorum」保证。
2. **Log Matching**：若两份日志在某 index 上 `(index, term)` 相同，则该 index 之前的所有条目完全相同。由 AppendEntries 的 `prevLogIndex/prevLogTerm` 一致性检查保证。
3. **Leader Completeness**：已提交的条目必然出现在所有后续 Leader 的日志中。由 RequestVote 的「候选人日志必须至少与投票者一样新」检查保证。

### 3.3 数据结构与持久化字段

```c
#ifndef CE_RAFT_H
#define CE_RAFT_H

#include "public_api/ce_types.h"
#include <stdint.h>

#define CE_RAFT_MAX_PEERS        8      /* 3 或 5，留余量 */
#define CE_RAFT_MAX_ENTRY_SIZE   4096   /* 单条日志载荷上限 */
#define CE_RAFT_MAX_BATCH        64     /* 单次 AppendEntries 携带条目上限 */
#define CE_RAFT_INVALID_INDEX    0ULL   /* index 从 1 开始，0 表示空 */

typedef enum CeRaftState {
    CE_RAFT_FOLLOWER  = 0,
    CE_RAFT_CANDIDATE = 1,
    CE_RAFT_LEADER    = 2,
} CeRaftState;

/** 日志条目类型 */
typedef enum CeRaftEntryType {
    CE_RAFT_ENTRY_NOOP        = 0,  /* Leader 上任时的空条目，用于确立提交点 */
    CE_RAFT_ENTRY_ENTITY_BIND = 1,  /* 实体绑定：entity_id → server_id（2.3） */
    CE_RAFT_ENTRY_ENTITY_UNBIND = 2,
    CE_RAFT_ENTRY_SERVICE_PUT = 3,  /* 服务注册表变更 */
    CE_RAFT_ENTRY_SERVICE_DEL = 4,
} CeRaftEntryType;

/** 日志条目（磁盘与网络同一布局，大端序列化） */
typedef struct CeRaftLogEntry {
    uint64_t index;        /* 从 1 单调递增 */
    uint64_t term;         /* 写入时的 Leader term */
    uint32_t type;         /* CeRaftEntryType */
    uint32_t data_len;
    uint32_t crc32;        /* 覆盖 header + data，检测磁盘损坏 */
    uint8_t  data[];       /* 变长载荷 */
} CeRaftLogEntry;
```

**持久化字段（每次变更必须 fsync 后才可回复 RPC，否则违反 Raft 安全性）：**

| 字段 | 类型 | 何时更新 | 文件 |
|---|---|---|---|
| `current_term` | `uint64_t` | 发现更高 term、自己发起选举时 +1 | `raft_state.dat` |
| `voted_for` | `char[64]` | 投票给某候选人时；term 变更时清空 | `raft_state.dat` |
| `log[]` | 条目序列 | AppendEntries 追加、Leader 本地 append | `raft_log.dat` + `raft_log.idx` |

`raft_state.dat` 布局（定长 96 字节，写入用「写临时文件 → fsync → rename」保证原子性）：
```
[4B magic "RFST"][4B version=1][8B current_term][64B voted_for][8B last_applied][4B crc32]
```

`raft_log.dat` 为追加写；`raft_log.idx` 为 `index → 文件偏移` 的定长索引（每项 16 字节：`8B offset + 8B length`），便于 O(1) 定位任意 index。启动时校验尾部条目 CRC，损坏则截断到最后一个完好条目（Raft 允许日志尾部丢失未提交条目）。

**易失状态：**
```c
typedef struct CeRaftPeer {
    char     node_id[64];
    /* Leader 专用 */
    uint64_t next_index;    /* 下一条要发给该 peer 的 index */
    uint64_t match_index;   /* 已确认复制到该 peer 的最高 index */
    uint64_t last_contact_ms;
    CeBool   vote_granted;  /* 本轮选举是否投给了我 */
    CeBool   in_flight;     /* 是否有未响应的 AppendEntries，防拥塞 */
} CeRaftPeer;

typedef struct CeRaftNode {
    /* 身份与配置（静态，D1） */
    char        self_id[64];
    CeRaftPeer  peers[CE_RAFT_MAX_PEERS];
    uint8_t     peer_count;        /* 含自身；quorum = peer_count/2 + 1 */

    /* 持久化状态（内存镜像） */
    uint64_t    current_term;
    char        voted_for[64];

    /* 易失状态 */
    CeRaftState state;
    uint64_t    commit_index;      /* 已知已提交的最高 index */
    uint64_t    last_applied;      /* 已应用到状态机的最高 index */
    uint64_t    last_log_index;
    uint64_t    last_log_term;
    char        leader_id[64];     /* 当前已知 Leader，空串表示未知 */

    /* 计时器（毫秒） */
    uint64_t    election_deadline_ms;   /* 随机化，见 3.4 */
    uint64_t    heartbeat_deadline_ms;
    uint32_t    election_timeout_min_ms;  /* 150 */
    uint32_t    election_timeout_max_ms;  /* 300 */
    uint32_t    heartbeat_interval_ms;    /* 50 */
    uint64_t    quorum_contact_ms;        /* Leader 最近一次获得 quorum 响应的时间 */

    /* 依赖注入 */
    struct CeRaftLog*   log;
    void*               fsm_ctx;           /* 状态机上下文（实体表 + 注册表） */
    CeResult (*apply_fn)(void* fsm_ctx, const CeRaftLogEntry* e);
    CeResult (*send_fn)(void* transport, const char* peer_id,
                        uint16_t msg_type, const void* payload, uint32_t len);
    void*               transport;         /* 指向 CeRouter，走 FACE_RAFT 连接 */

    uint64_t    rng_state;      /* xorshift64，选举超时随机化 */
} CeRaftNode;
```

### 3.4 选举超时随机化（脑裂/活锁防护的第一道闸）

```c
/** 重置选举计时器，超时值在 [min, max] 内均匀随机
 *  随机化是 Raft 避免「选举活锁」（多个候选人反复瓜分选票）的关键。
 *  伪选举没有这个概念，因此每次网络抖动都以相同顺序产出相同 Leader，
 *  但在分区两侧会同时产出两个 Leader。 */
static inline void ce_raft_reset_election_timer(CeRaftNode* n, uint64_t now_ms) {
    uint32_t span = n->election_timeout_max_ms - n->election_timeout_min_ms + 1;
    uint64_t x = n->rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;   /* xorshift64 */
    n->rng_state = x;
    n->election_deadline_ms = now_ms + n->election_timeout_min_ms + (uint32_t)(x % span);
}
```

取值依据：Router 集群同机房内 RTT P99 < 5ms，选举超时下限取 150ms ≈ 30× RTT，满足 Raft 论文建议的「选举超时 >> 广播时间」。跨机房部署（同 region 内多可用区）RTT 可达 20ms，配置需上调到 `[500, 1000]`。

### 3.5 RPC 消息结构体

均定义在 `src_c/server/ce_game_protocol.h`（约束 C4），大端序列化，走 `FACE_RAFT` 面。

```c
/* ---- RequestVote (MSG_RAFT_REQUEST_VOTE = 0x4000) ---- */
typedef struct CeRaftRequestVote {
    uint64_t term;              /* 候选人的 term */
    char     candidate_id[64];
    uint64_t last_log_index;    /* 候选人最后日志 index */
    uint64_t last_log_term;     /* 候选人最后日志 term */
} CeRaftRequestVote;

/* ---- RequestVote 响应 (MSG_RAFT_VOTE_RESP = 0x4001) ---- */
typedef struct CeRaftVoteResponse {
    uint64_t term;              /* 响应者的 currentTerm，用于候选人更新自己 */
    char     voter_id[64];
    uint8_t  vote_granted;      /* 1 = 投票；0 = 拒绝 */
    uint8_t  _pad[7];
} CeRaftVoteResponse;

/* ---- AppendEntries (MSG_RAFT_APPEND_ENTRIES = 0x4002) ---- */
typedef struct CeRaftAppendEntries {
    uint64_t term;
    char     leader_id[64];
    uint64_t prev_log_index;    /* 新条目紧前一条的 index */
    uint64_t prev_log_term;     /* 新条目紧前一条的 term */
    uint64_t leader_commit;     /* Leader 的 commitIndex */
    uint32_t entry_count;       /* 0 = 纯心跳 */
    uint32_t entries_bytes;     /* 后随的条目总字节数 */
    /* 之后紧跟 entry_count 个序列化的 CeRaftLogEntry */
} CeRaftAppendEntries;

/* ---- AppendEntries 响应 (MSG_RAFT_APPEND_RESP = 0x4003) ---- */
typedef struct CeRaftAppendResponse {
    uint64_t term;
    char     follower_id[64];
    uint8_t  success;           /* 1 = 一致性检查通过并已追加 */
    uint8_t  _pad[7];
    uint64_t match_index;       /* 成功时 = 追加后的 lastLogIndex */
    uint64_t conflict_index;    /* 失败时提示 Leader 回退到的 index（加速回溯） */
    uint64_t conflict_term;
} CeRaftAppendResponse;
```

**冲突回溯优化**：标准 Raft 中 Leader 失败后 `nextIndex--` 逐条回退，最坏 O(logLen) 轮 RPC。这里 Follower 在响应里带回 `conflict_index/conflict_term`（其日志中该 term 的第一条 index），Leader 一次跳过整个冲突 term，把回溯轮数从条目数级降到 term 数级。

### 3.6 核心接口

```c
/* ---- 生命周期 ---- */
CeRaftNode* ce_raft_create(const char* self_id, const char* const* peer_ids, uint8_t peer_count,
                            const char* log_dir);
void        ce_raft_destroy(CeRaftNode* n);

/** 从磁盘恢复 current_term / voted_for / 日志，并重放到状态机（last_applied → commit_index） */
CeResult    ce_raft_recover(CeRaftNode* n);

/** 注册状态机应用回调与传输回调 */
void        ce_raft_set_fsm(CeRaftNode* n, void* fsm_ctx,
                            CeResult (*apply_fn)(void*, const CeRaftLogEntry*));
void        ce_raft_set_transport(CeRaftNode* n, void* transport,
                                  CeResult (*send_fn)(void*, const char*, uint16_t, const void*, uint32_t));

/* ---- 驱动：由 Router 主循环的时间轮每 10ms 调一次 ---- */
/** 推进计时器：Follower/Candidate 检查选举超时；Leader 发心跳、检查 quorum 存活 */
CeResult    ce_raft_tick(CeRaftNode* n, uint64_t now_ms);

/* ---- RPC 处理（返回值仅表示处理是否出错，响应通过 send_fn 发出） ---- */
CeResult    ce_raft_handle_request_vote(CeRaftNode* n, const CeRaftRequestVote* req, uint64_t now_ms);
CeResult    ce_raft_handle_vote_response(CeRaftNode* n, const CeRaftVoteResponse* resp, uint64_t now_ms);
CeResult    ce_raft_handle_append_entries(CeRaftNode* n, const CeRaftAppendEntries* req,
                                          const uint8_t* entries_blob, uint32_t blob_len, uint64_t now_ms);
CeResult    ce_raft_handle_append_response(CeRaftNode* n, const CeRaftAppendResponse* resp, uint64_t now_ms);

/* ---- 写入：仅 Leader 可调 ---- */
/** 提交一条命令到 Raft 日志。异步：条目被 quorum 复制并 apply 后触发 cb。
 *  非 Leader 返回 CE_ERR_NOT_LEADER，调用者应把请求重定向到 ce_raft_leader_id()。 */
CeResult    ce_raft_propose(CeRaftNode* n, CeRaftEntryType type,
                            const void* data, uint32_t len,
                            void (*cb)(void* ud, CeResult r, uint64_t index), void* ud);

/* ---- 查询 ---- */
CeRaftState ce_raft_state(const CeRaftNode* n);
uint64_t    ce_raft_current_term(const CeRaftNode* n);
const char* ce_raft_leader_id(const CeRaftNode* n);
uint64_t    ce_raft_commit_index(const CeRaftNode* n);
/** Leader 是否仍持有有效租约（最近 election_timeout 内获得过 quorum 响应）。
 *  线性读的前置条件（见 4.5）。 */
CeBool      ce_raft_has_quorum_lease(const CeRaftNode* n, uint64_t now_ms);
```

### 3.7 脑裂防护的四重机制

| 机制 | 说明 | 对应缺陷 |
|---|---|---|
| **M1 quorum 投票** | 成为 Leader 必须获得 `peer_count/2+1` 票（含自身）。3 节点分区为 2:1 时，1 侧永远无法当选。 | B1 |
| **M2 term 单调 + 每 term 一票** | `voted_for` 与 `current_term` 持久化并 fsync。重启后不会在同一 term 重复投票。 | B2 |
| **M3 Leader 租约自降级** | Leader 在 `2 × election_timeout_max` 内未获得 quorum 的 AppendEntries 响应，主动 `state = FOLLOWER`、清空 `leader_id`、拒绝所有写。阻止孤立的旧 Leader 继续接受写请求。 | B1 |
| **M4 写必须过 quorum 才 apply** | `commit_index` 仅在 `matchIndex` 的中位数（第 `quorum` 大值）对应条目 term == current_term 时才推进。少数派 Leader 的写永远不 commit，客户端收到超时而非成功。 | B1 |

`CE_LOG_WARN("RAFT", "Leader 租约失效（%llu ms 未获 quorum），降级为 Follower", elapsed);`

### 3.8 提交索引推进算法

```
Leader 收到 AppendResponse(success=1) 时：
  peers[i].match_index = resp->match_index
  peers[i].next_index  = resp->match_index + 1

  /* 取所有节点（含自身 last_log_index）的 match_index 降序排列，
     第 quorum 个值即为「被多数派复制到的最高 index」 */
  sorted = sort_desc([self.last_log_index] + [p.match_index for p in peers])
  candidate = sorted[quorum - 1]

  /* Raft 安全性要求：只能提交当前 term 的条目（否则可能提交被覆盖的旧条目） */
  if candidate > commit_index && log_term_at(candidate) == current_term:
      commit_index = candidate
      apply_committed_entries()   /* last_applied → commit_index 逐条 apply_fn */
```

Follower 侧：`commit_index = min(leader_commit, last_log_index)`，随后同样 apply。

### 3.9 与 Router 主循环的集成

- Raft 不占独立线程。`ce_raft_tick()` 由 Router 时间轮以 10ms 精度驱动，符合「单线程事件循环」模型。
- Raft RPC 的收发**完全复用** `ce_router_conn` 的 io_uring 收发路径（`send_fn` 内部即 `ce_router_conn_enqueue` + `ce_router_conn_flush`），不新建 socket 抽象（约束 C4）。
- **持久化 fsync 是唯一的阻塞点**。v0.1 采用同步 `write + fsync`（Raft 语义要求 fsync 完成才能回 RPC）。为避免阻塞主循环，日志追加走批量提交：一次 tick 内累积的多条 `propose` 合并成一次 write + 一次 fsync（group commit），预期 fsync 频率 ≤ 100 次/秒。v0.2 可迁移到 `ce_async_write` + `IORING_OP_FSYNC` 使其真正异步，接口预留但 v0.1 不启用。
---

## 4. 子项 2.3：实体路由表（`ce_router_entity_table.c/h`）

### 4.1 问题与目标

现状无任何全局实体位置映射：Gateway 收到某玩家的消息后，只能靠一致性哈希猜一个 Game 进程，而实体在 Cell 分裂/迁移后实际所在进程与哈希落点无关，导致消息投递到错误进程。目标是维护权威的 `entity_id (uint64) → game_server_id (uint32)` 映射，**写走 Raft 日志保证一致性，读走本地 state machine + 缓存保证性能**。

### 4.2 数据结构

```c
#ifndef CE_ROUTER_ENTITY_TABLE_H
#define CE_ROUTER_ENTITY_TABLE_H

#include "public_api/ce_types.h"
#include "router/ce_raft.h"
#include <stdint.h>

#define CE_ENTITY_TABLE_DEFAULT_BUCKETS  (1u << 20)   /* 1048576 桶 */
#define CE_ENTITY_TABLE_MAX_LOAD_FACTOR  0.75f
#define CE_ENTITY_CACHE_DEFAULT_ENTRIES  65536
#define CE_ENTITY_INVALID_SERVER         0u

/** 表项。24 字节，3 个恰好塞进一条 64B cache line 余 8B */
typedef struct CeEntityBinding {
    uint64_t entity_id;        /* key */
    uint32_t server_id;        /* value: game_server_id */
    uint32_t cell_id;          /* 所属 Cell，供迁移与 AOI 用 */
    uint64_t raft_index;       /* 写入该绑定的 Raft 日志 index，用于冲突裁决与读一致性判断 */
} CeEntityBinding;

/** 开放地址法哈希表（线性探测）。
 *  选型理由：无指针、无 per-entry malloc、cache 友好；
 *  删除用 tombstone（server_id == CE_ENTITY_INVALID_SERVER 且 entity_id != 0）。 */
typedef struct CeEntityTable {
    CeEntityBinding* buckets;
    uint32_t         bucket_count;    /* 2 的幂，掩码取模 */
    uint32_t         mask;            /* bucket_count - 1 */
    uint32_t         used;            /* 有效项数 */
    uint32_t         tombstones;
    uint64_t         applied_index;   /* 已应用的最高 Raft index == raft.last_applied */
    /* 统计 */
    uint64_t         lookup_total;
    uint64_t         lookup_hit;
    uint64_t         probe_total;      /* 累计探测次数，用于监控平均探测长度 */
} CeEntityTable;
```

**哈希函数**：`entity_id` 由引擎递增分配，低位连续，直接掩码取模会聚簇。采用 splitmix64 finalizer 混淆：

```c
static inline uint32_t ce_entity_hash(uint64_t id, uint32_t mask) {
    uint64_t z = id + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (uint32_t)(z & mask);
}
```

**读缓存**：Gateway 侧查询呈明显热点（在线玩家集合远小于总实体数）。在 Router 内再加一层直接映射的定长缓存，避免主表探测：

```c
typedef struct CeEntityCacheLine {
    uint64_t entity_id;
    uint32_t server_id;
    uint32_t cell_id;
    uint64_t stamp_index;   /* 写入缓存时的 applied_index，用于失效判断 */
} CeEntityCacheLine;        /* 24 B */

typedef struct CeEntityCache {
    CeEntityCacheLine* lines;
    uint32_t           line_count;   /* 2 的幂，默认 65536 */
    uint32_t           mask;
    uint64_t           hit;
    uint64_t           miss;
} CeEntityCache;
```

缓存失效策略：**写穿透 + 单项失效**。每次 apply `ENTITY_BIND/UNBIND` 时，直接更新或清除对应缓存行（O(1)，不做全表 flush）。因为缓存与主表在同一进程、同一线程内被同一个 apply 路径更新，不存在缓存与主表不一致的窗口。

### 4.3 容量与内存估算

| 项 | 计算 | 结果 |
|---|---|---|
| 单表项 | `sizeof(CeEntityBinding)` = 8+4+4+8 | **24 B** |
| 主表（1M 桶，负载因子 0.75） | 1048576 × 24 B | **24 MiB**，可容纳 786432 个实体 |
| 主表（4M 桶，扩容后） | 4194304 × 24 B | **96 MiB**，可容纳 3145728 个实体 |
| 读缓存（64K 行） | 65536 × 24 B | **1.5 MiB**（可全部驻留 L3） |
| Raft 日志单条 BIND 条目 | header 28 B + payload 16 B | **44 B** |
| Raft 日志（100 万次绑定） | 1e6 × 44 B | **≈ 42 MiB** 磁盘 |
| 日志索引文件 | 1e6 × 16 B | **16 MiB** |

**目标规模**：全球同服单 region 预期在线 50 万实体（玩家 + NPC + 可交互物件），峰值 100 万。默认 `bucket_count = 1<<20`（1M 桶，容 78 万），超过负载因子 0.75 时倍增扩容到 `1<<21`。扩容是 O(n) rehash，100 万项约 24MiB 内存拷贝 + rehash，实测预期 < 30ms——**这会阻塞主循环**，因此：
- 启动时按配置 `entity_table_buckets` 预分配足够大的表（生产环境直接配 `1<<21` = 2M 桶 = 48 MiB），**避免运行期扩容**。
- 若仍触发扩容，记录 `CE_LOG_WARN("ENTITAB", "实体表扩容 %u → %u 桶，主循环将阻塞", old, new);` 并计入指标告警。

**Raft 日志无限增长问题**：决策 D1 明确不做快照压缩。缓解方案：
1. 日志按段（segment）切分，每段 64 MiB。
2. 提供离线工具 `chaos_raft_compact`（**不在本 spec 交付范围**，Phase 3 补），在集群滚动重启的维护窗口内把状态机快照落盘并截断日志。
3. v0.1 运维约束：单 region Router 集群需每 30 天维护窗口做一次离线压缩。按每天 1000 万次绑定变更估算，日志日增 440 MiB，30 天 ≈ 13 GiB，磁盘可接受。此约束写入部署文档。

### 4.4 接口

```c
/* ---- 生命周期 ---- */
CeEntityTable* ce_entity_table_create(uint32_t bucket_count, uint32_t cache_lines);
void           ce_entity_table_destroy(CeEntityTable* t);

/* ---- 状态机 apply（仅由 ce_raft 的 apply_fn 调用，绝不由外部直接调） ---- */
/** 应用一条已提交的 Raft 条目。type 须为 ENTITY_BIND / ENTITY_UNBIND。
 *  幂等：同一 index 重复 apply 无副作用（重放恢复时依赖此性质）。 */
CeResult ce_entity_table_apply(CeEntityTable* t, const CeRaftLogEntry* e);

/* ---- 读路径（本地，无网络、无 Raft，O(1)） ---- */
/** 查询实体所在服务器。命中返回 CE_OK 并填充 out_*；未绑定返回 CE_ERR_NOT_FOUND。
 *  读的是本地 state machine，Follower 上可能落后 Leader 若干条目（最终一致）。 */
CeResult ce_entity_table_lookup(CeEntityTable* t, uint64_t entity_id,
                                 uint32_t* out_server_id, uint32_t* out_cell_id);

/** 批量查询，减少 RPC 往返（Gateway 批量场景） */
int      ce_entity_table_lookup_batch(CeEntityTable* t, const uint64_t* ids, int count,
                                       uint32_t* out_server_ids, uint32_t* out_cell_ids);

/** 当前已应用的 Raft index，供调用方判断数据新鲜度 */
uint64_t ce_entity_table_applied_index(const CeEntityTable* t);

/* ---- 写路径（走 Raft，仅 Leader 可发起） ---- */
/** 绑定实体到服务器。内部构造 CE_RAFT_ENTRY_ENTITY_BIND 条目并 ce_raft_propose。
 *  非 Leader 返回 CE_ERR_NOT_LEADER。cb 在条目被 quorum 提交并 apply 后触发。 */
CeResult ce_entity_table_bind(struct CeRouter* r, uint64_t entity_id,
                              uint32_t server_id, uint32_t cell_id,
                              void (*cb)(void* ud, CeResult), void* ud);

CeResult ce_entity_table_unbind(struct CeRouter* r, uint64_t entity_id,
                                void (*cb)(void* ud, CeResult), void* ud);

/** 批量绑定（Cell 迁移场景：一次迁移数千实体）。合并为**单条** Raft 条目，
 *  保证整批原子提交——迁移必须全成或全败，不能半途。 */
CeResult ce_entity_table_bind_batch(struct CeRouter* r, const CeEntityBinding* bindings, int count,
                                    void (*cb)(void* ud, CeResult), void* ud);

/* ---- 统计 ---- */
typedef struct CeEntityTableStats {
    uint32_t used;
    uint32_t bucket_count;
    float    load_factor;
    float    avg_probe_len;
    uint64_t lookup_total;
    uint64_t cache_hit;
    uint64_t cache_miss;
    float    cache_hit_rate;
    uint64_t applied_index;
} CeEntityTableStats;

void ce_entity_table_stats(const CeEntityTable* t, CeEntityTableStats* out);
```

**日志条目载荷布局**（定义在 `ce_game_protocol.h`）：
```c
/** CE_RAFT_ENTRY_ENTITY_BIND 的 data 部分 */
typedef struct CeEntityBindRecord {
    uint64_t entity_id;
    uint32_t server_id;
    uint32_t cell_id;
} CeEntityBindRecord;          /* 16 B */

/** 批量绑定：count 后紧跟 count 个 CeEntityBindRecord */
typedef struct CeEntityBindBatchRecord {
    uint32_t count;
    uint32_t _pad;
    /* CeEntityBindRecord records[count]; */
} CeEntityBindBatchRecord;
```

### 4.5 Gateway 侧查询接口

Gateway（已在 9000 端口共用三协议）作为 Router 的客户端，通过 `FACE_GAME` 面查询。消息定义在 `ce_game_protocol.h`，客户端与服务端同一份 include（约束 C4）。

```c
/* MSG_ENTITY_LOOKUP = 0x5000 */
typedef struct CeEntityLookupReq {
    uint32_t request_id;      /* 请求关联 id，响应回显 */
    uint32_t flags;           /* bit0: 1 = 要求线性读（强制转发 Leader） */
    uint64_t entity_id;
} CeEntityLookupReq;

/* MSG_ENTITY_LOOKUP_RESP = 0x5001 */
typedef struct CeEntityLookupResp {
    uint32_t request_id;
    uint32_t result;          /* CE_OK / CE_ERR_NOT_FOUND / CE_ERR_NOT_LEADER */
    uint32_t server_id;
    uint32_t cell_id;
    uint64_t applied_index;   /* Router 侧数据新鲜度 */
} CeEntityLookupResp;

/* MSG_ENTITY_BATCH_LOOKUP = 0x5005：request_id + count + uint64 ids[count] */
/* MSG_ENTITY_BATCH_RESP   = 0x5006：request_id + count + (server_id, cell_id)[count] */
```

**两级读语义：**

| 模式 | flags | 路径 | 延迟 | 一致性 |
|---|---|---|---|---|
| **最终一致读**（默认） | `flags & 1 == 0` | 任意 Router 节点本地缓存/主表直接答 | 亚微秒级本地 + 一次网络往返 | 可能落后 Leader 若干条目（通常 < 50ms） |
| **线性读** | `flags & 1 == 1` | 必须 Leader 处理，且 `ce_raft_has_quorum_lease()` 为真才答，否则回 `CE_ERR_NOT_LEADER` 并附 leader_id | 一次往返 + 可能一次重定向 | 强一致 |

Gateway 使用建议：
- **普通游戏消息转发**用最终一致读。即使读到旧的 `server_id`，目标 Game 进程发现实体已迁走时会回 `MSG_ENTITY_MOVED` 让 Gateway 重查（线性读），这是「乐观转发 + 兜底纠正」模式，把 99.9% 的查询留在快路径。
- **Cell 迁移确认、玩家登录首次定位**用线性读。

**Gateway 本地缓存**：Gateway 侧也维护一份 `entity_id → server_id` 的 LRU（建议 16K 项），Router 在 apply 绑定变更时向订阅了该实体的 Gateway 主动推 `MSG_ENTITY_BIND_RESP` 作失效通知。这使得稳态下 Gateway 根本不发起查询，只在冷启动与实体迁移时查。

### 4.6 写路径完整时序

```
Game 进程 (实体在此创建)
   |  MSG_ENTITY_BIND (entity_id, server_id, cell_id)
   v
Router-A (Follower)  ──→ 非 Leader，回 CE_ERR_NOT_LEADER + leader_id="router-2"
   |
Game 重发到 Router-B (Leader)
   |
   ├─ ce_entity_table_bind() → ce_raft_propose(ENTITY_BIND)
   |     ├─ 追加到本地日志（内存）
   |     ├─ group commit: write + fsync（同 tick 内其它 propose 合批）
   |     └─ 向所有 Follower 广播 AppendEntries（io_uring send）
   |
   ├─ 收到 quorum 个 AppendResponse(success=1)
   |     └─ 推进 commit_index → apply_fn → ce_entity_table_apply()
   |            ├─ 主表插入/更新
   |            └─ 缓存行写穿透
   |
   └─ 触发 propose 回调 → 回 MSG_ENTITY_BIND_RESP(CE_OK) 给 Game

Follower 侧：收到含该条目的 AppendEntries → 追加 + fsync → 回 success
             下一轮 AppendEntries 带来更高的 leader_commit → 本地 apply
             → 本地实体表可服务最终一致读
```

**写延迟预算**（同机房 3 节点）：Leader 本地 fsync 1~3ms + 网络往返 P99 5ms + Follower fsync 1~3ms ≈ **P99 < 10ms**。这是写 QPS 的上限约束：单条串行写 ≈ 100/s，但通过 pipeline（多条 in-flight）+ group commit + 批量绑定，可达数万次/秒。
---

## 5. MVP 范围与验收标准

### 5.1 MVP 范围界定

**In Scope（v0.1 必须交付）：**

| # | 内容 | 子项 |
|---|---|---|
| S1 | `src_c/router/` 8 个模块，单端口 9100 三协议面复用，io_uring 事件驱动 | 2.1 |
| S2 | 服务注册/发现/心跳/超时摘除，功能对齐 Lua 版 | 2.1 |
| S3 | 一致性哈希环（150 vnode，固定 xxHash32），`lookup` / `lookup_n` | 2.1 |
| S4 | 跨区域转发链路 + TTL 防环 | 2.1 |
| S5 | 3 节点 Raft：选举、日志复制、提交推进、持久化、崩溃恢复 | 2.2 |
| S6 | Leader 租约自降级、少数派拒写 | 2.2 |
| S7 | 实体路由表：apply / lookup / bind / unbind / bind_batch + 读缓存 | 2.3 |
| S8 | Gateway 查询接口（最终一致读 + 线性读 + 批量） | 2.3 |
| S9 | 协议常量与结构体统一到 `ce_game_protocol.h`，删除 `protocol.lua` | 全部 |
| S10 | Lua 压测/机器人脚本（Headless）+ CI 集成 | 全部 |

**Out of Scope（明确不做，v0.2+）：**

| # | 内容 | 理由 |
|---|---|---|
| O1 | Raft 快照压缩 / Log Compaction | 决策 D1；离线工具 Phase 3 补 |
| O2 | Raft 成员动态变更（joint consensus） | 决策 D1；成员表静态配置，变更靠滚动重启 |
| O3 | PreVote / Leader Transfer | v0.1 不需要；网络抖动导致的无谓选举可接受 |
| O4 | 5 节点以上、跨 region 单一 Raft group | 决策 D2；跨 region 走最终一致转发 |
| O5 | 异步 fsync（`IORING_OP_FSYNC`） | v0.1 用 group commit 同步 fsync 已够；接口预留 |
| O6 | 实体表运行期在线扩容不阻塞 | 靠启动预分配规避 |
| O7 | Windows 平台验证 | `ce_async_io` 的 IOCP 后端本身尚未完成，非本 spec 范围 |

### 5.2 压测与机器人脚本（Lua，约束 C7）

新增 `tests/bench/router/`，全部 Lua，Headless 模式共享引擎网络层 C 绑定，**不使用 Python**：

| 脚本 | 用途 |
|---|---|
| `bench_route_qps.lua` | N 个并发连接持续发 `MSG_ENTITY_LOOKUP`，测路由查询 QPS 与延迟分布（P50/P99/P999） |
| `bench_entity_bind.lua` | 持续 bind/unbind，测写 QPS 与提交延迟 |
| `bench_batch_lookup.lua` | 批量查询吞吐（每包 256 个 id） |
| `robot_election_chaos.lua` | 反复 kill/restart Leader，测选举收敛时间分布 |
| `robot_partition.lua` | 用 iptables/tc 制造分区，验证少数派拒写、无双 Leader |
| `robot_registry_churn.lua` | 高频服务注册/注销/心跳超时，测注册表稳定性 |

延迟统计在 Lua 侧用 HDR-style 对数分桶直方图实现，避免存全部样本。

### 5.3 验收标准（可量化）+ CI job 映射（约束 C5）

复用现有 5 个 job，**不新增独立 job**：

| 验收项 | 指标 | 接入的 CI job | 新增步骤 |
|---|---|---|---|
| **A1 编译** | `src_c/router/` 在 Debug/Release 下零警告编译（`-Wall -Wextra -Werror`） | `编译 & 测试 (Debug)` + `Release 编译` | 复用现有 `make` 步骤，Router 目标加入默认构建 |
| **A2 无 epoll 门禁** | `src_c/router/` 中 grep 不到 `epoll_/select(/poll(` 符号 | `编译 & 测试 (Debug)` | 在「CMake 配置」步骤前加一条 grep 检查，命中即 `::error::` 失败 |
| **A3 单元测试** | 新增 `tests/unit/test_router_*.c`（哈希环、解帧、注册表、实体表）+ `test_raft_*.c`（选举、日志匹配、提交推进、冲突回溯），全部通过 | `编译 & 测试 (Debug)` 的「全量测试」（ctest） | 注册进 `tests/CMakeLists.txt`，由现有 `ctest` 自动跑，零新增 job |
| **A4 Raft 确定性仿真** | `test_raft_sim.c`：内存内模拟 3/5 节点 + 可注入的消息丢失/重排/分区，跑 10000 轮随机场景，断言三条不变式（3.2 节）不被破坏 | `编译 & 测试 (Debug)` 的 ctest | 同上，纯内存无 I/O，单测耗时 < 10s |
| **A5 内存安全** | Valgrind 下 Router 单测与冒烟 0 definitely-lost、0 invalid read/write | `内存检查 (Valgrind)` | 复用该 job 已编译的 Debug 产物，追加 Router 单测到 valgrind 目标列表 |
| **A6 Lua 语法** | `tests/bench/router/*.lua` 通过 `luac -p` | `Lua 语法检查` | 该 job 已对 `src_lua/**` 做语法检查，扩展 glob 覆盖 `tests/bench/router/` |
| **A7 集成：3 节点选举** | 3 节点启动后 **5 秒内**产出唯一 Leader；kill Leader 后 **选举收敛时间 P99 < 1000ms**（收敛定义：新 Leader 发出首个 AppendEntries 心跳），P50 < 400ms | `Gateway 集成测试` | 该 job 已编译 `chaos_gateway`，追加 `make chaos_router` 并跑 `robot_election_chaos.lua`（Lua，非 Python），复用同一 build 目录 |
| **A8 集成：无脑裂** | `robot_partition.lua` 制造 2:1 分区，持续 30s：少数派侧写请求 100% 返回 `CE_ERR_NOT_LEADER` 或超时，**0 次成功写**；分区恢复后两侧 `commit_index` 与实体表内容完全一致 | `Gateway 集成测试` | 同上 |
| **A9 路由查询 QPS** | 单 Router 进程、单线程、最终一致读：`bench_route_qps.lua` 达 **≥ 200,000 QPS**（16 并发连接，pipeline 深度 32，本机 loopback），延迟 **P99 < 1ms**；批量查询（每包 256 id）**≥ 2,000,000 实体/秒** | `Gateway 集成测试` | 追加 bench 步骤，`--duration 20` 短时长版本，避免拖长 CI |
| **A10 实体表写 QPS** | 3 节点 Raft、`bench_entity_bind.lua`：单条写 **≥ 8,000 QPS**（pipeline 64 in-flight + group commit），提交延迟 **P99 < 30ms**；`bind_batch`（每批 1000 实体）**≥ 500,000 实体/秒** | `Gateway 集成测试` | 同上 |
| **A11 实体表容量** | 200 万实体绑定后：内存占用 **< 120 MiB**（含 Raft 内存日志），平均探测长度 **< 2.0**，lookup P99 不退化超过 20% | `Gateway 集成测试` | `bench_route_qps.lua --preload 2000000` |
| **A12 崩溃恢复** | kill -9 任一节点后重启，**3 秒内**恢复日志并追平 `commit_index`；重启前后已提交的实体绑定 **100% 不丢** | `Gateway 集成测试` | `robot_election_chaos.lua --mode kill9` |
| **A13 缓存命中率** | 稳态热点场景（20% 实体承载 80% 查询）读缓存命中率 **≥ 85%** | `Gateway 集成测试` | bench 结束打印 `CeEntityTableStats` |
| **A14 功能对齐** | Lua Router 的服务注册/发现/哈希路由/跨区域转发行为，C 版逐项对齐（同一组 Lua 测试脚本同时打 Lua Router 与 C Router，结果一致） | `Gateway 集成测试` | 双跑对比模式，是回退方案（第 6 章）的前置验证 |

**CI 耗时控制**：所有 bench 步骤在 CI 中用短时长（20s）+ 小规模（预载 20 万实体）参数，只做**门槛校验**（QPS 不低于目标的 60%，因 GitHub Runner 性能波动大）；完整指标由本地/预发环境跑长时长版本产出，结果记录到 `docs/bench/`。QPS 硬门槛不放在 CI 上作红线，防止 runner 抖动造成假失败——CI 只保证功能正确与不出现数量级退化。

### 5.4 实施路线图（提交粒度，约束 C8）

| 周 | 内容 | 示例提交 |
|---|---|---|
| W1 | 协议统一：`ce_game_protocol.h` 号段重划 + 新增结构体；删 `protocol.lua` | `[refactor](network): 协议常量统一到 ce_game_protocol.h` |
| W2 | 连接层 + io_uring 事件循环 + 单端口三面复用 | `[feat](network): Router 连接层基于 ce_async_io 实现零拷贝解帧` |
| W3 | 哈希环 + 注册表 + 分发表 | `[feat](server): Router 一致性哈希环与服务注册表 C 实现` |
| W4 | 跨区域链路 + 统计 + 2.1 单测 | `[test](server): Router 模块单元测试接入 ctest` |
| W5–W6 | Raft 日志持久化 + 选举 | `[feat](server): 实现 Raft Leader 选举替换伪选举` |
| W7 | Raft 日志复制 + 提交推进 + 冲突回溯 | `[feat](server): Raft 日志复制与提交索引推进` |
| W8 | Raft 确定性仿真测试 + 分区/崩溃场景 | `[test](server): Raft 确定性仿真与分区场景测试` |
| W9 | 实体路由表 + 缓存 | `[feat](server): 实体路由表 entity_id→server_id 映射` |
| W10 | Gateway 查询接口 + 双跑对齐验证 + 文档 | `[docs](server): Phase 2 Router 集群部署与运维说明` |

---

## 6. 风险分析与回退方案

### 6.1 风险清单

| # | 风险 | 概率 | 影响 | 缓解措施 |
|---|---|---|---|---|
| R1 | **Raft 实现存在细微安全性 bug**（如提交了未被多数派复制的条目、同 term 双 Leader），线上表现为实体路由表数据分歧 | 中 | 致命（玩家消息投递错误进程，跨服数据错乱） | ① A4 确定性仿真 10000 轮随机场景断言三条不变式；② 每次 apply 时断言 `entry.index == last_applied + 1`，不连续立即 `CE_LOG_ERROR("RAFT", ...)` 并进入只读安全模式；③ 一致性巡检：每 60s 各节点上报 `(commit_index, 实体表 CRC)`，不一致告警；④ 见 6.2 回退方案 |
| R2 | fsync 阻塞主循环，导致 Raft 心跳延迟触发无谓选举 | 中高 | 高（Leader 频繁切换，写不可用） | ① group commit 合批降低 fsync 频率；② fsync 耗时纳入监控，超过 `heartbeat_interval/2` 告警；③ 选举超时下限 150ms 相对 fsync 3ms 有 50× 余量；④ 若实测不足，v0.2 切 `IORING_OP_FSYNC` 异步化（接口已预留） |
| R3 | io_uring 在目标内核版本不可用或行为差异（liburing 2.14 需内核 ≥ 5.10 才有完整特性） | 中 | 中 | ① `ce_async_io` 抽象层已提供 POSIX 回落，`ce_async_backend_name()` 启动时打印实际后端；② 部署文档明确要求内核 ≥ 5.15；③ CI runner（ubuntu-latest，内核 6.x）验证 uring 路径；④ **回落路径仅供开发机使用，生产必须 uring** |
| R4 | CQE 延迟到达导致操作已关闭 fd 的悬垂 `user_data` | 中 | 中（崩溃或串数据） | slot + generation 双字段校验（2.3 节），generation 不匹配直接丢弃事件 |
| R5 | 单端口多协议面复用下，业务连接风暴挤压 Raft 心跳 | 中 | 高（误触发选举） | ① Raft 面事件在事件循环内优先处理；② Raft peer 连接不占业务连接配额；③ 业务面加 accept 限速 |
| R6 | Raft 日志无限增长（无快照压缩，决策 D1 的已知代价） | 高 | 中（磁盘耗尽 → 写不可用） | ① 日志分段 + 磁盘水位监控，80% 告警；② 30 天维护窗口离线压缩（写入部署文档）；③ 磁盘按 30 天 × 日增量 × 3 倍余量规划（建议 ≥ 100 GiB） |
| R7 | 实体表运行期扩容 rehash 阻塞主循环 30ms | 低 | 中（一次心跳周期抖动） | 启动预分配 `1<<21` 桶，生产环境永不触发；触发即告警 |
| R8 | C 版功能与 Lua 版行为不一致，切换后出现回归 | 中 | 高 | A14 双跑对比验收，同一组 Lua 测试脚本打两个实现比对结果 |
| R9 | 协议号段重划（`MSG_USER_BASE 0x8000` 废弃）导致已有插件/客户端不兼容 | 中 | 中 | ① 号段变更集中在一次提交内完成，`ce_game_protocol.h` 加协议版本号 `CE_GAME_PROTOCOL_VERSION = 2`；② 握手时校验版本，不匹配明确拒绝并打印期望版本，避免静默错乱 |
| R10 | 3 节点集群中 2 节点同时故障，集群完全失去写能力 | 低 | 高 | ① 3 个 Router 分布在不同物理机/不同可用区；② 失去 quorum 时明确对外返回不可用而非降级为单机写（宁可不可用也不能数据分歧）；③ 见 6.2 应急回退 |

### 6.2 回退方案：C Router 出问题如何退回 Lua Router

回退能力的**前提设计**：C Router 与 Lua Router 在 `FACE_GAME` 面上**协议完全兼容**（同一份 `ce_game_protocol.h` 语义，Lua 侧通过 C 绑定读常量，见 1.9 节的废弃决定不影响兼容——Lua 压测脚本改为从 C 绑定取常量，行为与 C 版一致）。因此 Game / Gateway / DBProxy **无需改动**即可在两个实现之间切换。

**保留策略**：`src_lua/router/` 在 Phase 2 交付后**不删除**，保留至 Phase 3 结束、C Router 稳定运行满 3 个月后再移除。期间由 A14 双跑测试持续保证两者行为一致（CI 每次都跑，防止 Lua 版腐烂）。唯一例外是 `protocol.lua` —— 它必须删除以满足约束 C4，Lua Router 改为通过 C 绑定 `ce_protocol_const()` 获取常量，这同时也确保了两个实现的协议定义永不分叉。

**三级回退阶梯：**

**Level 1 — 功能降级（不换实现，秒级）**
适用：Raft 出现疑似一致性问题，但 C Router 本身进程健康。
- 通过 admin 接口（或配置热更）把 Router 切到 `RAFT_DISABLED` 模式：
  - Raft 停止选举与复制，节点固定为 `FOLLOWER`，实体表转为**只读**。
  - 写请求全部返回 `CE_ERR_READONLY`，Game 侧降级为「本地权威 + 哈希环路由」（即 Phase 2 之前的行为）。
  - 读继续由本地状态机提供，服务不中断。
- 影响：Cell 迁移暂不可用，跨服功能受限，普通游戏流程正常。
- 触发条件：一致性巡检（R1 缓解措施③）报告节点间实体表 CRC 不一致。

**Level 2 — 切回 Lua Router（分钟级，无需改 Game/Gateway 代码）**
适用：C Router 存在严重 bug（崩溃、内存错误、Raft 无法收敛）。
1. 停止全部 C Router 进程。
2. 启动 Lua Router：`lua src_lua/router/init.lua --node-id router-1 --game-port 9100 --peer ...`。**注意端口**：Lua 版 Game 面默认也是 9100，与 C 版一致，Game/Gateway 的连接配置无需改动；Lua 版会额外监听 9101 集群端口（C 版不用），防火墙需保持该端口开放以支持回退。
3. Lua 版不识别 `FACE_GAME` 的 8 字节 magic 握手 —— 因此 Game/Gateway 侧的握手逻辑必须实现为「发送 magic 后若对端在 500ms 内未按 magic 协议响应则回落为无 magic 模式」，或更简单地：**在配置中提供 `router_handshake = "magic" | "legacy"` 开关**，回退时统一切 `legacy`。此开关必须在 W2（连接层）实现时一并交付，并纳入 A14 双跑验证，否则回退路径不成立。
4. **数据损失范围**：Lua Router 无实体路由表，回退后全局实体位置映射丢失。Game 进程需在启动/重连时重新上报其持有的实体（`MSG_ENTITY_BIND` 在 Lua 版被忽略，Gateway 退化为哈希环路由）。这意味着 Cell 迁移功能在 Level 2 下完全不可用 —— 这是可接受的代价，因为 Cell 迁移（2.4）本身依赖 2.3，回退时一并停用。
5. 恢复时间目标（RTO）：**< 5 分钟**（脚本化启停）。

**Level 3 — Raft 日志离线修复后重新上线（小时级）**
适用：C Router 逻辑正常但日志已损坏/分歧。
1. 各节点导出 `raft_log.dat` 与实体表状态快照（提供 `chaos_router_dump` 调试工具）。
2. 人工比对选定**权威节点**（通常是 `commit_index` 最高且 CRC 与多数派一致者）。
3. 其余节点删除本地 `raft_log.dat`/`raft_state.dat`，以空日志启动，从权威节点全量追赶。**注意**：无快照机制意味着追赶需重放全部日志，13 GiB 日志重放预估 10~20 分钟，这是决策 D1（不做快照）的直接成本，已知并接受。
4. 期间集群运行于 Level 1 只读模式。

**回退演练要求**：Level 1 与 Level 2 必须在 W10 完成一次完整演练并记录耗时到 `docs/runbook/router-fallback.md`。未演练不得上线。

---

## 7. 附录

### 7.1 术语表

| 术语 | 含义 |
|---|---|
| term | Raft 任期，单调递增整数，用于识别过期 Leader |
| quorum | 多数派，`N/2 + 1`。3 节点为 2，5 节点为 3 |
| commit_index | 已知被多数派复制、可安全应用的最高日志 index |
| last_applied | 已应用到状态机的最高日志 index，`<= commit_index` |
| vnode | 一致性哈希环上的虚拟节点，用于均衡负载 |
| 协议面（face） | 单端口复用下按首帧 magic 区分的逻辑通道 |
| 线性读 | 保证读到最新已提交状态的读，须经 Leader 且持有 quorum 租约 |
| Cell | 空间分区单元，一个 Game 进程可承载多个 Cell |

### 7.2 与其它 Phase 2 子项的接口约定

| 下游子项 | 依赖本 spec 提供的能力 |
|---|---|
| 2.4 Cell 迁移协议 | `ce_entity_table_bind_batch()` 原子批量改绑；线性读确认迁移完成 |
| 2.5 Ghost 实体管理 | `ce_entity_table_lookup()` 查邻居 Cell 所在 server_id |
| 2.6 跨 Cell AOI 查询 | 同上，批量查询接口 |
| 2.7 DBProxy 分片 | 复用 `ce_hashring_*` 做 `player_id` 分片路由 |

### 7.3 遗留决策项（需在 W1 评审前敲定）

1. 一致性哈希函数最终选定 xxHash32 还是 MurmurHash3_x86_32（两者性能与分布接近，倾向 xxHash32，因引擎可能已有依赖，需确认）。
2. `raft_log_dir` 默认路径与权限模型（是否与引擎数据目录同级）。
3. admin-web（见 `chaos-engine-admin-web-spec-v0.2.md`）展示 Raft 状态的接口形态：主动推送还是轮询拉取。

---

**文档结束。**
