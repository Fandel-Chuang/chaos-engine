# Router — 服务发现与消息路由（v0.3 Delta）

> 来源: add-router-cluster | 状态: 草案 | 变更类型: ADDED

## 概述

新增 Router 进程，作为服务注册中心和服务器间消息路由层。Router 负责服务注册与发现（Game 进程启动时注册，Gateway 查询路由表）、消息路由（按 player_id 一致性哈希路由到目标微服务进程）、**跨大区路由（核心能力，Router 间 TCP 长连接组成全球 Router 网格）**、健康检查与故障剔除、路由表同步（Router 集群内广播）。Router 以独立的 Lua 进程运行，网络层使用共享 C 库 `ce_net_base`。

---

## ADDED Requirements

### Requirement: 服务注册与发现

Router SHALL 作为服务注册中心，管理所有后端微服务进程的注册信息，并提供查询接口。

服务注册 MUST 满足以下要求：
- 微服务进程启动时 MUST 向 Router 发送注册请求，包含服务类型、进程 ID、监听地址和端口
- Router MUST 维护全局服务注册表，按服务类型索引
- 注册信息 MUST 包含：`service_type`（服务类型枚举）、`process_id`（唯一进程标识）、`address`（IP:Port）、`status`（ONLINE/OFFLINE/DRAINING）、`registered_at`（注册时间戳）、`last_heartbeat`（最近心跳时间）
- Gateway 和其他微服务 MUST 能通过 Router 查询服务注册表，获取目标服务的地址列表
- 微服务进程正常关闭时 MUST 向 Router 发送注销请求
- Router MUST 支持服务注册表的全量和增量查询

#### Scenario: 微服务进程注册

- **WHEN** 好友服务进程（service_type=FRIEND, process_id=2001）启动并连接到 Router
- **THEN** 进程发送 REGISTER 请求：`{service_type: "FRIEND", process_id: 2001, address: "192.168.1.10:9100"}`
- **AND** Router 将注册信息写入服务注册表
- **AND** Router 返回 REGISTER_ACK 确认注册成功
- **AND** Router 向集群内其他 Router 广播此注册信息

#### Scenario: Gateway 查询路由表

- **WHEN** Gateway 需要将 msg_type=0x0300（好友请求）路由到好友服务
- **THEN** Gateway 向 Router 发送 QUERY 请求：`{service_type: "FRIEND"}`
- **AND** Router 返回所有 ONLINE 状态的好友服务进程列表：`[{process_id: 2001, address: "192.168.1.10:9100"}, {process_id: 2002, address: "192.168.1.11:9100"}]`
- **AND** Gateway 缓存查询结果，TTL 为 30 秒

#### Scenario: 微服务进程注销

- **WHEN** 好友服务进程正常关闭（收到 SIGTERM）
- **THEN** 进程向 Router 发送 DEREGISTER 请求：`{service_type: "FRIEND", process_id: 2001}`
- **AND** Router 将进程状态标记为 OFFLINE
- **AND** Router 向集群内其他 Router 广播此注销信息
- **AND** 已缓存该进程信息的 Gateway 在 TTL 过期后获取更新列表

---

### Requirement: 消息路由（一致性哈希）

Router SHALL 使用一致性哈希算法将玩家请求路由到目标微服务进程，确保同一玩家的请求始终到达同一进程。

消息路由 MUST 满足以下要求：
- 路由键为 `player_id`，使用一致性哈希（虚拟节点数 150）映射到目标微服务进程
- 一致性哈希环 MUST 在服务注册表变更时（进程上线/下线）自动更新
- 同一 `player_id` 的请求 MUST 始终路由到同一进程（会话亲和性），除非该进程下线
- 进程下线时，其负责的 player_id 范围 MUST 自动重新分配到其他可用进程
- 路由查询 MUST 为 O(log N) 时间复杂度（N 为虚拟节点数）
- Router MUST 支持直接路由（指定目标 process_id）和哈希路由（指定 player_id）两种模式

#### Scenario: 一致性哈希路由

- **WHEN** 客户端发送好友请求（player_id=42），Gateway 转发到 Router
- **THEN** Router 对 player_id=42 计算哈希值，在一致性哈希环上查找最近的虚拟节点
- **AND** 虚拟节点映射到好友服务进程 process_id=2001
- **AND** Router 将消息转发到 192.168.1.10:9100
- **AND** 后续 player_id=42 的所有好友请求均路由到 process_id=2001

#### Scenario: 进程下线时重新分配

- **WHEN** 好友服务进程 process_id=2001 下线（故障或主动注销）
- **THEN** Router 从一致性哈希环中移除 process_id=2001 的虚拟节点
- **AND** 原本映射到 process_id=2001 的 player_id 范围自动重新分配到相邻虚拟节点对应的进程
- **AND** 新请求自动路由到新分配的目标进程
- **AND** 数据迁移由上层微服务处理（Router 不负责数据迁移）

#### Scenario: 直接路由模式

- **WHEN** 微服务 A 需要向特定微服务 B 进程（process_id=3001）发送消息
- **THEN** 微服务 A 在消息中指定目标 process_id=3001
- **AND** Router 直接查询服务注册表获取 process_id=3001 的地址
- **AND** Router 将消息转发到目标地址
- **AND** 若目标进程不在线，返回错误 DESTINATION_UNREACHABLE

---

### Requirement: 跨大区路由（核心能力）

Router MUST 支持全球多区域部署下的跨大区消息路由，组成全球 Router 网格。这是 ChaosEngine 全球同服架构的核心能力，不是可选或延后功能。

跨大区路由 MUST 满足以下要求：
- 全球分多个大区（如亚洲区 asia-east、欧洲区 eu-west、美洲区 us-west），每个大区独立部署完整集群
- 每个大区部署至少一个 Router 实例，大区之间通过 Router 间 TCP 长连接互联，组成全球 Router 网格
- 不同大区的 Router 之间 MUST 建立 TCP 长连接（全互联拓扑，N 个大区共 N*(N-1)/2 条跨区连接）
- 跨区消息 MUST 通过 Router 间连接转发，不经过 Gateway
- 跨区消息 MUST 携带 `source_region` 和 `target_region` 标识
- Router MUST 维护区域路由表：`{region_id → [router_addresses]}`
- 跨区连接断开时 MUST 自动重连（指数退避：1s, 2s, 4s, 最大 30s）

#### Scenario: 全球 Router 网格拓扑

- **WHEN** 全球部署 3 个大区：asia-east（亚洲区）、eu-west（欧洲区）、us-west（美洲区）
- **THEN** 每个大区独立部署完整集群（Gateway + Router 集群 + 微服务 + DBProxy + MongoDB）
- **AND** 3 个大区的 Router 之间建立全互联 TCP 长连接（共 3 条跨区连接：asia↔eu, asia↔us, eu↔us）
- **AND** 各区域 Router 维护全球区域路由表：`{asia-east: [r1.asia:9101], eu-west: [r1.eu:9101], us-west: [r1.us:9101]}`
- **AND** 跨区消息格式包含 `source_region` 和 `target_region`：`{source_region: "asia-east", target_region: "us-west", player_id: 42, body: {...}}`
- **AND** 任意大区的玩家可与其他大区的玩家进行跨服交互（PVP、聊天、交易等）
- **AND** 新增大区（如 sa-east 南美区）时，只需与现有大区建立 TCP 连接即可加入全球网格

#### Scenario: 跨区域消息转发

- **WHEN** us-west 区域的玩家 A（player_id=42）向 ap-southeast 区域的玩家 B（player_id=99）发起 PVP 挑战
- **THEN** us-west Router 识别目标 player_id=99 属于 ap-southeast 区域
- **AND** us-west Router 将消息通过 Router 间 TCP 连接转发到 ap-southeast Router
- **AND** ap-southeast Router 根据 player_id=99 一致性哈希路由到本地 PVP 服务进程
- **AND** 响应沿原路径返回：PVP 进程 → ap-southeast Router → us-west Router → Gateway → 玩家 A

#### Scenario: 跨区域连接断开

- **WHEN** us-west 与 ap-southeast 之间的 Router 间 TCP 连接因网络故障断开
- **THEN** 双方 Router 检测到连接断开（心跳超时 3s）
- **AND** 双方 Router 将对方区域标记为 UNREACHABLE
- **AND** 发往对方区域的消息暂存到本地转发队列（最大 10000 条）
- **AND** 连接恢复后，转发队列中的消息按序发送
- **AND** 队列满后，新消息返回错误 REGION_UNREACHABLE

#### Scenario: 区域路由表更新

- **WHEN** ap-southeast 区域新增一个 Router 实例（扩容）
- **THEN** 新 Router 向集群内广播 REGION_UPDATE 消息
- **AND** us-west Router 收到后更新区域路由表：ap-southeast 增加新 Router 地址
- **AND** us-west Router 与新 Router 建立 TCP 连接
- **AND** 跨区域消息可在新旧 Router 间负载均衡

---

### Requirement: 健康检查与故障剔除

Router SHALL 对所有注册的微服务进程执行健康检查，自动剔除故障进程。

健康检查 MUST 满足以下要求：
- Router 每 1 秒向所有注册的微服务进程发送 PING 心跳
- 微服务进程 MUST 在收到 PING 后立即回复 PONG
- 连续 3 次（3 秒）未收到 PONG 响应，判定进程故障
- 故障进程 MUST 从服务注册表中标记为 OFFLINE，从一致性哈希环中移除
- 故障进程恢复后（重新发送 REGISTER），自动重新加入服务注册表和哈希环
- Router 自身故障由 Router 集群内的其他 Router 检测（Router 间心跳）

#### Scenario: 微服务进程故障检测

- **WHEN** 好友服务进程 process_id=2001 崩溃
- **THEN** Router 在 1 秒后发送 PING，无响应
- **AND** 2 秒后再次发送 PING，无响应
- **AND** 3 秒后第三次 PING 无响应，判定 process_id=2001 故障
- **AND** Router 将 process_id=2001 标记为 OFFLINE
- **AND** Router 从一致性哈希环中移除 process_id=2001 的虚拟节点
- **AND** Router 向集群内其他 Router 广播此故障信息
- **AND** 记录 WARN 日志：`"Service FRIEND/2001 unhealthy, removed from ring"`

#### Scenario: 故障进程恢复

- **WHEN** 好友服务进程 process_id=2001 恢复运行并重新向 Router 发送 REGISTER
- **THEN** Router 将 process_id=2001 标记为 ONLINE
- **AND** Router 将 process_id=2001 的虚拟节点重新加入一致性哈希环
- **AND** Router 向集群内其他 Router 广播此恢复信息
- **AND** 记录 INFO 日志：`"Service FRIEND/2001 recovered, added to ring"`

#### Scenario: Router 间健康检查

- **WHEN** Router 集群中有 3 个 Router 实例（R1, R2, R3）
- **THEN** 每个 Router 向其他 Router 发送心跳（间隔 1s）
- **AND** R2 连续 3s 未收到 R3 的心跳响应，判定 R3 故障
- **AND** R2 将 R3 从集群成员列表中移除
- **AND** R2 接管 R3 负责的区域路由（若 R3 是跨区域连接的对端）
- **AND** 记录 ERROR 日志：`"Router R3 unreachable, removed from cluster"`

---

### Requirement: 路由表同步（Router 集群内广播）

Router SHALL 在集群内同步服务注册表和路由信息，确保所有 Router 实例拥有一致的路由视图。

路由表同步 MUST 满足以下要求：
- Router 集群内所有实例 MUST 维护一致的服务注册表
- 注册表变更（REGISTER/DEREGISTER/故障剔除）MUST 立即广播到集群内所有其他 Router
- 广播消息 MUST 包含变更类型、服务信息、时间戳和序列号
- Router 启动时 MUST 从集群中其他 Router 同步完整路由表（全量同步）
- 广播消息 MUST 使用 TCP 可靠传输，确保不丢失
- 网络分区恢复后 MUST 自动合并路由表（以最新时间戳为准）

#### Scenario: 注册信息广播

- **WHEN** Router R1 收到好友服务进程 process_id=2001 的 REGISTER 请求
- **THEN** R1 更新本地服务注册表
- **AND** R1 向 R2 和 R3 广播 REGISTER_NOTIFY：`{action: "REGISTER", service_type: "FRIEND", process_id: 2001, address: "192.168.1.10:9100", seq: 42, ts: 1718500000}`
- **AND** R2 和 R3 收到后更新各自的服务注册表
- **AND** R2 和 R3 各自更新一致性哈希环

#### Scenario: 新 Router 加入集群

- **WHEN** 新 Router R4 启动并连接到集群中任一现有 Router（如 R1）
- **THEN** R4 向 R1 发送 SYNC_REQUEST 请求全量路由表
- **AND** R1 返回当前完整的服务注册表和一致性哈希环状态
- **AND** R4 应用全量数据，建立本地服务注册表和哈希环
- **AND** R4 向所有其他 Router 建立连接，加入集群
- **AND** 记录 INFO 日志：`"Router R4 joined cluster, synced N services"`

#### Scenario: 网络分区恢复后合并

- **WHEN** 集群发生网络分区（R1,R2 与 R3,R4 隔离），分区恢复后
- **THEN** 双方 Router 交换各自的路由表版本（基于最新序列号）
- **AND** 以序列号较高的变更记录为准合并
- **AND** 冲突时（同一 process_id 的不同状态），以最新时间戳为准
- **AND** 合并完成后所有 Router 拥有一致的路由表
- **AND** 记录 WARN 日志：`"Network partition healed, merged N divergent records"`

---

### Requirement: Router 进程生命周期

Router SHALL 作为独立进程运行，支持标准的启动、运行、关闭流程。

进程生命周期 MUST 满足以下要求：
- 通过 `chaos_router` 可执行文件启动，加载 Lua 配置和脚本
- 启动时绑定配置的 TCP 端口（默认 9100 用于服务注册/路由，9101 用于集群内部通信）
- 启动时连接集群中的其他 Router（从配置文件读取集群成员列表）
- 运行时输出结构化日志（JSON 格式）
- 支持 SIGHUP 信号重载配置（不中断现有连接和路由表）
- 支持 SIGTERM 信号优雅关闭（通知集群、排空路由、关闭连接）

#### Scenario: 正常启动

- **WHEN** 执行 `./chaos_router -c config.lua`
- **THEN** Router 加载配置文件，绑定 TCP:9100（服务端口）和 TCP:9101（集群端口）
- **AND** 连接配置的集群成员（如 192.168.1.20:9101, 192.168.1.21:9101）
- **AND** 从集群同步完整路由表
- **AND** 输出启动日志，包含监听地址、集群成员和已同步的服务数量
- **AND** 进入事件循环，等待服务注册和路由请求

#### Scenario: 优雅关闭

- **WHEN** Router 进程收到 SIGTERM 信号
- **THEN** Router 向集群广播 LEAVE 消息，通知即将离开
- **AND** 停止接受新的服务注册和路由请求
- **AND** 等待进行中的路由请求完成（最长 10s）
- **AND** 关闭所有微服务连接和集群连接
- **AND** 记录关闭日志并退出
