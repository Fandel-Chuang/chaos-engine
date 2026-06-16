# Tasks: Router 集群 — 服务发现 + 消息路由 + Game 进程按功能模块拆分

> 按依赖顺序排列，Phase 1-4。全部待开始。

---

## Phase 1: ce_net_base 共享网络库 + 跨区路由基础（C 内核）

- [x] 1.1 创建 `src_c/network/ce_net_base.h`：定义 `CeNetBaseConn` 连接句柄、`CeNetBasePool` 连接池句柄、消息类型枚举、`ce_net_base_connect/disconnect/reconnect` 连接管理接口、`ce_net_base_send/recv` 消息收发接口、`ce_net_base_heartbeat_start/stop/is_alive` 心跳接口、`ce_net_base_pool_create/acquire/release` 连接池接口、`ce_net_base_pack/unpack` 协议编解码接口
- [x] 1.2 实现 `ce_net_base_connect()`：建立 TCP 连接（非阻塞 socket + io_uring/posix 异步 I/O），支持连接超时配置（默认 5s），返回 `CeNetBaseConn` 句柄
- [x] 1.3 实现二进制协议编解码（`ce_net_base_pack/unpack`）：长度前缀（4 字节大端）+ 消息类型（2 字节大端）+ 变长 payload；复用 `ce_dbproxy` 的变长整数编码实现
- [x] 1.4 实现 `ce_net_base_send/recv`：基于协议编解码的消息收发，支持非阻塞模式，内部处理 TCP 粘包/拆包（长度前缀分隔）
- [x] 1.5 实现心跳检测：`ce_net_base_heartbeat_start` 启动定时心跳（可配置间隔，默认 1s），`ce_net_base_is_alive` 检查连接是否存活（连续 3 次超时判定断开），心跳回调通知上层
- [x] 1.6 实现连接池（`CeNetBasePool`）：最小/最大连接数配置，连接复用（acquire/release），空闲连接回收（默认 60s），连接健康检查
- [x] 1.7 实现自动重连（`ce_net_base_reconnect`）：指数退避策略（1s, 2s, 4s, 8s, 最大 30s），最大重试次数可配置
- [x] 1.8 更新 `src_c/network/CMakeLists.txt`：添加 `ce_net_base.c` 源文件，编译为 `engine_net_base` 静态库，链接 `engine_core`
- [x] 1.9 编写 ce_net_base 单元测试：连接建立/断开、消息收发（粘包/拆包）、心跳超时检测、连接池获取/归还、自动重连
- [x] 1.10 定义跨区消息格式：在 `ce_net_base.h` 中定义跨区消息头结构（length + type + source_region + target_region + payload），定义大区标识枚举（asia-east=0x0001, eu-west=0x0002, us-west=0x0003, us-east=0x0004, sa-east=0x0005）
- [x] 1.11 实现跨区 TCP 长连接框架：`ce_net_base_cross_region_connect(region_id, router_list)` 建立到目标大区 Router 的 TCP 长连接，`ce_net_base_cross_region_send(conn, source_region, target_region, msg)` 发送跨区消息，`ce_net_base_cross_region_recv(conn, ...)` 接收跨区消息
- [x] 1.12 实现全球 Router 网格连接管理：维护跨区连接表 `{target_region → CeNetBaseConn}`，支持全互联拓扑自动建立连接，新增大区时动态添加连接
- [x] 1.13 Gateway 网络层迁移：替换 Gateway 内联的 socket 操作、消息编解码、心跳检测为 `ce_net_base` 调用（仅替换网络底层实现，Gateway 保持直连 Game，不引入路由功能）
- [x] 1.14 更新 Gateway 的 CMakeLists.txt：链接 `engine_net_base` 替代内联网络代码
- [x] 1.15 回归测试：验证 Gateway 迁移后 TCP/KCP/WebSocket 接入、连接管理、消息转发（直连 Game）功能不受影响

---

## Phase 2: Router 进程（Lua）

- [x] 2.1 创建 `src_lua/router/` 目录，创建 `init.lua` 入口：启动 TCP 服务器监听 `:9100`（服务注册/路由查询端口）和 `:9101`（集群内部通信端口），加载各子模块，进入事件循环
- [x] 2.2 创建 `config.lua`：定义 Router 配置（服务端口 9100、集群端口 9101、集群成员列表、心跳间隔 1s、超时阈值 3s、虚拟节点数 150、区域 ID），支持命令行参数和配置文件覆盖
- [x] 2.3 创建 `registry.lua`：实现服务注册表 — 数据结构（按 service_type 索引的进程列表）、`register(service_type, process_id, address)`、`deregister(process_id)`、`query(service_type)`、`lookup(process_id)`、`list_all()`、`update_heartbeat(process_id)`
- [x] 2.4 创建 `hash_ring.lua`：实现一致性哈希环 — 虚拟节点生成（150 per 进程）、`add_node(process_id, address)`、`remove_node(process_id)`、`get_node(key)` 二分查找路由、`rebuild()` 全量重建
- [x] 2.5 创建 `router.lua`：实现 Game↔Game 消息路由逻辑 — 解析路由请求（直接路由 vs 哈希路由）、调用 `hash_ring.get_node()` 或 `registry.lookup()` 获取目标地址、转发消息到目标进程、处理响应回传、路由失败处理（SERVICE_UNAVAILABLE / DESTINATION_UNREACHABLE）。Router 不处理 Gateway→Game 的客户端消息
- [x] 2.6 创建 `health.lua`：实现健康检查 — 每 1s 向所有注册进程发送 PING、等待 PONG 响应、连续 3 次超时判定故障、故障时调用 `registry.deregister()` + `hash_ring.remove_node()` + 广播 `HEALTH_CHANGE`、进程恢复时重新加入
- [x] 2.7 创建 `cluster.lua`：实现 Router 集群内部通信 — 集群成员管理（加入/离开）、广播同步（REGISTER_NOTIFY / DEREGISTER_NOTIFY / HEALTH_CHANGE）、全量同步（SYNC_REQUEST / SYNC_RESPONSE，新 Router 加入时）、序列号机制（防重复）、网络分区恢复合并
- [x] 2.8 创建 `cross_region.lua`：实现跨大区路由 — 全球区域路由表管理（`{region_id → [router_addresses]}`）、跨区 TCP 长连接管理（基于 Phase 1 的 ce_net_base 跨区连接框架，全互联拓扑组成全球 Router 网格）、跨区消息转发（识别目标大区 → 选择目标 Router → 转发，消息携带 source_region + target_region）、连接断开时本地队列暂存、新增大区时动态加入全球网格
- [x] 2.9 创建 `protocol.lua`：实现 Router 内部协议编解码（复用 ce_net_base 的二进制协议格式），定义 Router 消息类型（REGISTER/DEREGISTER/QUERY/ROUTE/PING/PONG/REGISTER_NOTIFY/DEREGISTER_NOTIFY/HEALTH_CHANGE/SYNC_REQUEST/SYNC_RESPONSE/REGION_UPDATE/CROSS_REGION_FORWARD）
- [x] 2.10 创建 `src_c/runtime/ce_router_main.c`：Router 进程入口 — 初始化 Lua VM，加载 `init.lua`，注册 ce_net_base C 函数到 Lua，进入主循环
- [x] 2.11 更新 CMakeLists.txt：添加 `CHAOS_HAS_ROUTER` option，添加 `ce_router_main` 可执行文件目标，链接 `engine_net_base` 和 Lua

---

## Phase 3: Game 进程按功能模块拆分（好友功能示例）

- [x] 3.1 创建 `src_lua/services/` 目录，定义功能模块类型枚举：在 `src_lua/services/service_types.lua` 中定义 `SERVICE_TYPE` 枚举（GAME=0x01, FRIEND=0x02, PVP=0x03, PVE=0x04, TRADE=0x05, GUILD=0x06, CHAT=0x07, MAIL=0x08）
- [x] 3.2 实现 Game 进程内 `ce_rpc_call()` 函数：在引擎核心中提供 `ce_rpc_call()` 函数，封装与 Router 的通信（注册、消息发送、心跳）。Game 进程启动时自动向 Router 注册，业务脚本只需调用 `ce_rpc_call()` 即可与其他 Game 进程通信
- [x] 3.3 创建好友功能 Lua 业务脚本 `src_lua/services/friend.lua`：好友业务逻辑（添加/删除好友、好友列表、在线状态查询），通过 `ce_rpc_call()` 与其他 Game 进程通信
- [x] 3.4 创建好友 Game 进程配置示例 `config_friend.lua`：定义 `service_type = "FRIEND"`、`process_id`、`region`、`service_script = "services/friend.lua"`、Router 地址、DBProxy 地址。启动命令：`./chaos_game -c config_friend.lua`
- [x] 3.5 好友 Game 进程集成 DBProxy：进程启动时连接独立的 DBProxy 实例（端口 9013），使用数据库 `chaos_{game_id}_friend`，集合 `relationships`（好友关系）、`requests`（好友请求）
- [x] 3.6 编写好友功能集成测试：好友添加/删除流程、好友列表查询、跨进程在线状态查询（GAME → Router → FRIEND → Router → GAME），验证 `ce_rpc_call()` 通信正确性

---

## Phase 4: 全球多区域集成 + 全链路测试

- [ ] 4.1 全球多区域部署：配置三个大区（region=asia-east, region=eu-west, region=us-west），每个大区独立启动完整集群（1 个 Gateway + Router 集群 + GAME 服务 + FRIEND 服务 + PVP 服务 + 各自 DBProxy + MongoDB），验证大区之间 Router 全球网格 TCP 连接建立
- [ ] 4.2 跨大区消息路由测试：asia-east 玩家向 us-west 玩家发送消息（如 PVP 挑战），验证消息经 asia-east Router → us-west Router → 目标微服务的完整链路，验证跨区消息格式（source_region + target_region）正确传递
- [ ] 4.3 全球同服交互测试：跨服 PVP（asia-east vs us-west）、全球聊天（三区互通）、跨区交易（eu-west ↔ asia-east），验证多区域玩家在同一游戏世界中的交互正确性
- [ ] 4.4 全链路集成测试：启动完整多区域环境，验证客户端 → Gateway 直连 Game + Game↔Game 经 Router + 微服务 → DBProxy → MongoDB 的完整数据流，覆盖同区和跨区两种场景
- [ ] 4.5 异常场景测试：Router 单实例故障（集群中其他 Router 接管）、微服务进程故障（Router 健康检查剔除 + 一致性哈希环更新）、跨区连接断开（本地队列暂存 + 恢复后重发）、Router 集群网络分区恢复（路由表合并）、单个大区整体故障（其他大区正常运行不受影响）
- [ ] 4.6 扩缩容测试：好友服务从 1 进程扩容到 3 进程（验证一致性哈希再均衡 + 新请求正确路由）、缩容（DRAINING → DEREGISTER → 请求迁移到其他进程）、故障缩容（进程崩溃 → Router 检测 → 自动剔除）
- [ ] 4.7 性能基准测试：Router 路由查询延迟（P50/P99）、一致性哈希查找性能（O(log N) vs 进程数）、Router 集群广播延迟、跨大区消息延迟（模拟 50ms/150ms/300ms 网络延迟，对应同洲/跨洲/跨洋）、微服务注册/注销延迟

---

**总进度：32/39 已完成（82%）**

**已完成**：Phase 1 ce_net_base（15 项）、Phase 2 Router 进程（11 项）、Phase 3 微服务拆分（6 项）。

**待完成**：Phase 4 全球多区域集成 + 全链路测试（7 项）。

**依赖关系**：
- Phase 2 依赖 Phase 1（Router 使用 ce_net_base，跨区路由使用 Phase 1 的跨区连接框架）
- Phase 3 依赖 Phase 2（微服务注册和 Game↔Game 通信需要 Router）
- Phase 4 依赖 Phase 1 + Phase 2 + Phase 3（全球多区域集成需要完整链路和跨区路由基础）
