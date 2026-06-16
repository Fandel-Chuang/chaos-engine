# Tasks: Router 集群 — 服务发现 + 消息路由 + 微服务拆分

> 按依赖顺序排列，Phase 1-5。全部待开始。

---

## Phase 1: ce_net_base 共享网络库（C 内核）

- [ ] 1.1 创建 `src_c/network/ce_net_base.h`：定义 `CeNetBaseConn` 连接句柄、`CeNetBasePool` 连接池句柄、消息类型枚举、`ce_net_base_connect/disconnect/reconnect` 连接管理接口、`ce_net_base_send/recv` 消息收发接口、`ce_net_base_heartbeat_start/stop/is_alive` 心跳接口、`ce_net_base_pool_create/acquire/release` 连接池接口、`ce_net_base_pack/unpack` 协议编解码接口
- [ ] 1.2 实现 `ce_net_base_connect()`：建立 TCP 连接（非阻塞 socket + io_uring/posix 异步 I/O），支持连接超时配置（默认 5s），返回 `CeNetBaseConn` 句柄
- [ ] 1.3 实现二进制协议编解码（`ce_net_base_pack/unpack`）：长度前缀（4 字节大端）+ 消息类型（2 字节大端）+ 变长 payload；复用 `ce_dbproxy` 的变长整数编码实现
- [ ] 1.4 实现 `ce_net_base_send/recv`：基于协议编解码的消息收发，支持非阻塞模式，内部处理 TCP 粘包/拆包（长度前缀分隔）
- [ ] 1.5 实现心跳检测：`ce_net_base_heartbeat_start` 启动定时心跳（可配置间隔，默认 1s），`ce_net_base_is_alive` 检查连接是否存活（连续 3 次超时判定断开），心跳回调通知上层
- [ ] 1.6 实现连接池（`CeNetBasePool`）：最小/最大连接数配置，连接复用（acquire/release），空闲连接回收（默认 60s），连接健康检查
- [ ] 1.7 实现自动重连（`ce_net_base_reconnect`）：指数退避策略（1s, 2s, 4s, 8s, 最大 30s），最大重试次数可配置
- [ ] 1.8 更新 `src_c/network/CMakeLists.txt`：添加 `ce_net_base.c` 源文件，编译为 `engine_net_base` 静态库，链接 `engine_core`
- [ ] 1.9 编写 ce_net_base 单元测试：连接建立/断开、消息收发（粘包/拆包）、心跳超时检测、连接池获取/归还、自动重连

---

## Phase 2: Gateway 重构（链接 ce_net_base）

- [ ] 2.1 重构 Gateway 的 TCP 连接管理：替换内联的 socket 操作为 `ce_net_base_connect/recv/send/disconnect` 调用
- [ ] 2.2 重构 Gateway 的消息编解码：替换内联的协议解析为 `ce_net_base_pack/unpack` 调用
- [ ] 2.3 重构 Gateway 的心跳检测：替换内联的心跳逻辑为 `ce_net_base_heartbeat_start/is_alive` 调用
- [ ] 2.4 Gateway 路由查询改为从 Router 动态获取：Gateway 启动时连接 Router，定期（30s TTL）查询路由表，替代静态配置文件路由
- [ ] 2.5 更新 Gateway 的 CMakeLists.txt：链接 `engine_net_base` 替代内联网络代码
- [ ] 2.6 回归测试：验证 Gateway 重构后 TCP/KCP/WebSocket 接入、连接管理、消息路由功能不受影响

---

## Phase 3: Router 进程（Lua）

- [ ] 3.1 创建 `src_lua/router/` 目录，创建 `init.lua` 入口：启动 TCP 服务器监听 `:9100`（服务注册/路由查询端口）和 `:9101`（集群内部通信端口），加载各子模块，进入事件循环
- [ ] 3.2 创建 `config.lua`：定义 Router 配置（服务端口 9100、集群端口 9101、集群成员列表、心跳间隔 1s、超时阈值 3s、虚拟节点数 150、区域 ID），支持命令行参数和配置文件覆盖
- [ ] 3.3 创建 `registry.lua`：实现服务注册表 — 数据结构（按 service_type 索引的进程列表）、`register(service_type, process_id, address)`、`deregister(process_id)`、`query(service_type)`、`lookup(process_id)`、`list_all()`、`update_heartbeat(process_id)`
- [ ] 3.4 创建 `hash_ring.lua`：实现一致性哈希环 — 虚拟节点生成（150 per 进程）、`add_node(process_id, address)`、`remove_node(process_id)`、`get_node(key)` 二分查找路由、`rebuild()` 全量重建
- [ ] 3.5 创建 `router.lua`：实现消息路由逻辑 — 解析路由请求（直接路由 vs 哈希路由）、调用 `hash_ring.get_node()` 或 `registry.lookup()` 获取目标地址、转发消息到目标进程、处理响应回传、路由失败处理（SERVICE_UNAVAILABLE / DESTINATION_UNREACHABLE）
- [ ] 3.6 创建 `health.lua`：实现健康检查 — 每 1s 向所有注册进程发送 PING、等待 PONG 响应、连续 3 次超时判定故障、故障时调用 `registry.deregister()` + `hash_ring.remove_node()` + 广播 `HEALTH_CHANGE`、进程恢复时重新加入
- [ ] 3.7 创建 `cluster.lua`：实现 Router 集群内部通信 — 集群成员管理（加入/离开）、广播同步（REGISTER_NOTIFY / DEREGISTER_NOTIFY / HEALTH_CHANGE）、全量同步（SYNC_REQUEST / SYNC_RESPONSE，新 Router 加入时）、序列号机制（防重复）、网络分区恢复合并
- [ ] 3.8 创建 `cross_region.lua`：实现跨区域转发 — 区域路由表管理（`{region_id → [router_addresses]}`）、跨区域 TCP 连接管理（全互联）、跨区域消息转发（识别目标区域 → 选择目标 Router → 转发）、连接断开时本地队列暂存
- [ ] 3.9 创建 `protocol.lua`：实现 Router 内部协议编解码（复用 ce_net_base 的二进制协议格式），定义 Router 消息类型（REGISTER/DEREGISTER/QUERY/ROUTE/PING/PONG/REGISTER_NOTIFY/DEREGISTER_NOTIFY/HEALTH_CHANGE/SYNC_REQUEST/SYNC_RESPONSE/REGION_UPDATE/CROSS_REGION_FORWARD）
- [ ] 3.10 创建 `src_c/runtime/ce_router_main.c`：Router 进程入口 — 初始化 Lua VM，加载 `init.lua`，注册 ce_net_base C 函数到 Lua，进入主循环
- [ ] 3.11 更新 CMakeLists.txt：添加 `CHAOS_HAS_ROUTER` option，添加 `ce_router_main` 可执行文件目标，链接 `engine_net_base` 和 Lua

---

## Phase 4: 微服务拆分（好友服务示例）

- [ ] 4.1 创建 `src_lua/shared/` 目录，创建 `service_registry.lua`：封装微服务注册/注销/心跳逻辑 — `register(service_type, process_id, address)` 自动完成 TCP 连接、发送 REGISTER、处理 ACK、启动心跳协程；`deregister()` 发送 DEREGISTER 并关闭连接
- [ ] 4.2 创建 `src_lua/shared/msg_router.lua`：封装微服务间消息通信 — `send(dst_service, dst_key, body)` 构造消息并发送到 Router、`request(dst_service, dst_key, body, timeout)` 发送请求并等待响应（支持超时）、`on_message(callback)` 注册消息处理回调
- [ ] 4.3 定义微服务类型枚举：在 `src_lua/shared/service_types.lua` 中定义 `SERVICE_TYPE` 枚举（GAME=0x01, FRIEND=0x02, PVP=0x03, PVE=0x04, TRADE=0x05, GUILD=0x06, CHAT=0x07, MAIL=0x08）
- [ ] 4.4 创建好友服务示例 `src_lua/services/friend/`：`init.lua`（入口，加载共享库，向 Router 注册）、`friend_logic.lua`（好友业务逻辑：添加/删除好友、好友列表、在线状态查询）、`friend_db.lua`（好友数据持久化，通过 DBProxy 读写）
- [ ] 4.5 好友服务集成 DBProxy：好友服务进程启动时连接独立的 DBProxy 实例（端口 9013），使用数据库 `chaos_{game_id}_friend`，集合 `relationships`（好友关系）、`requests`（好友请求）
- [ ] 4.6 更新 Gateway 路由配置：添加 msg_type 0x0300-0x030F → FRIEND 服务的路由规则，Gateway 从 Router 动态查询 FRIEND 服务地址
- [ ] 4.7 编写好友服务集成测试：好友添加/删除流程、好友列表查询、跨进程在线状态查询（FRIEND → Router → GAME → Router → FRIEND）

---

## Phase 5: 跨区域路由 + 集成测试

- [ ] 5.1 跨区域 Router 互联：配置两个模拟区域（region=us-west, region=ap-southeast），各启动 1 个 Router + 1 个 Gateway + 微服务，验证 Router 间 TCP 连接建立和跨区域消息转发
- [ ] 5.2 跨区域消息路由测试：us-west 玩家向 ap-southeast 玩家发送消息（如 PVP 挑战），验证消息经 us-west Router → ap-southeast Router → 目标微服务的完整链路
- [ ] 5.3 全链路集成测试：启动 Gateway + Router 集群（2 实例）+ GAME 服务 + FRIEND 服务 + PVP 服务 + 各自 DBProxy + MongoDB，验证客户端 → Gateway → Router → 微服务 → DBProxy → MongoDB 的完整数据流
- [ ] 5.4 异常场景测试：Router 单实例故障（集群中其他 Router 接管）、微服务进程故障（Router 健康检查剔除 + 一致性哈希环更新）、跨区域连接断开（本地队列暂存 + 恢复后重发）、Router 集群网络分区恢复（路由表合并）
- [ ] 5.5 扩缩容测试：好友服务从 1 进程扩容到 3 进程（验证一致性哈希再均衡 + 新请求正确路由）、缩容（DRAINING → DEREGISTER → 请求迁移到其他进程）、故障缩容（进程崩溃 → Router 检测 → 自动剔除）
- [ ] 5.6 性能基准测试：Router 路由查询延迟（P50/P99）、一致性哈希查找性能（O(log N) vs 进程数）、Router 集群广播延迟、跨区域消息延迟（模拟 50ms/150ms 网络延迟）、微服务注册/注销延迟、Gateway 路由表查询缓存命中率

---

**总进度：0/34 待开始（0%）**

**依赖关系**：
- Phase 2 依赖 Phase 1（Gateway 重构需要 ce_net_base）
- Phase 3 依赖 Phase 1（Router 使用 ce_net_base）
- Phase 4 依赖 Phase 3（微服务注册和通信需要 Router）
- Phase 5 依赖 Phase 3 + Phase 4（集成测试需要完整链路）
