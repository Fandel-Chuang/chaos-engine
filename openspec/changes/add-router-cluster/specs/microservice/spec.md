# Microservice — 微服务拆分规范（v0.3 Delta）

> 来源: add-router-cluster | 状态: 草案 | 变更类型: ADDED

## 概述

定义 ChaosEngine 微服务拆分规范。将单体 Game 进程按功能模块拆分为独立的微服务进程（好友/PVP/PVE/交易/公会等），每个微服务为独立的 Game 进程，通过 Router 进行服务注册和服务间通信。每个微服务连接独立的 DBProxy 实例实现数据隔离，支持独立扩缩容。

---

## ADDED Requirements

### Requirement: 微服务定义

系统 SHALL 支持按功能模块将 Game 逻辑拆分为独立的微服务进程，每种微服务有明确的服务类型标识和职责边界。

微服务定义 MUST 满足以下要求：
- 每种微服务 MUST 有唯一的 `service_type` 枚举值
- 微服务 MUST 作为独立的 Game 进程运行（继承 Game 进程的 ECS/AOI/Cell 等核心能力）
- 微服务进程 MUST 在启动时向 Router 注册（service_type + process_id + address）
- 微服务进程 MUST 在关闭时向 Router 注销
- 同一微服务类型可运行多个进程实例（水平扩展）

#### 服务类型枚举

| service_type | 枚举值 | 职责 | 数据范围 |
|-------------|--------|------|---------|
| GAME | 0x01 | 核心游戏逻辑（玩家移动、AOI、Cell 管理） | 玩家位置、AOI 状态、Cell 负载 |
| FRIEND | 0x02 | 好友系统（添加/删除好友、好友列表、在线状态） | 好友关系、好友请求 |
| PVP | 0x03 | 玩家对战（匹配、战斗逻辑、结算） | 战斗记录、段位、排名 |
| PVE | 0x04 | PVE 内容（副本、Boss 战、任务） | 副本进度、Boss 状态、任务数据 |
| TRADE | 0x05 | 交易系统（玩家间交易、拍卖行） | 交易记录、拍卖物品 |
| GUILD | 0x06 | 公会系统（创建/加入公会、公会管理） | 公会信息、成员列表、公会日志 |
| CHAT | 0x07 | 聊天系统（世界/公会/私聊频道） | 聊天记录（可选持久化） |
| MAIL | 0x08 | 邮件系统（系统邮件、玩家邮件、附件） | 邮件数据、附件物品 |

#### Scenario: 好友服务进程启动

- **WHEN** 好友服务进程（service_type=FRIEND, process_id=2001）启动
- **THEN** 进程加载好友系统的 Lua 业务逻辑
- **AND** 进程向 Router 发送 REGISTER：`{service_type: "FRIEND", process_id: 2001, address: "192.168.1.10:9100"}`
- **AND** 进程连接独立的 DBProxy 实例（用于好友数据持久化）
- **AND** 进程进入事件循环，等待路由消息

#### Scenario: 同一微服务多实例

- **WHEN** 好友服务需要扩容，启动第二个进程（process_id=2002）
- **THEN** process_id=2002 向 Router 注册，加入一致性哈希环
- **AND** 部分 player_id 的好友请求自动路由到 process_id=2002
- **AND** 两个进程独立运行，通过 Router 通信（如查询对方管理的玩家在线状态）

---

### Requirement: 服务间通信

微服务之间 SHALL 通过 Router 中转进行通信，不建立直接的 TCP 连接。

服务间通信 MUST 满足以下要求：
- 微服务 A 向微服务 B 发送消息时，MUST 将消息发送到 Router，由 Router 转发
- 消息 MUST 包含：源 service_type、源 process_id、目标 service_type、目标路由键（player_id 或 process_id）、消息体
- Router MUST 根据目标路由键将消息转发到正确的目标进程
- 请求-响应模式：微服务 A 发送请求后等待微服务 B 的响应，Router 负责将响应路由回微服务 A
- 通信协议 MUST 使用与 Gateway 一致的二进制协议（由 ce_net_base 提供）
- 超时请求 MUST 返回错误（默认超时 5s）

#### Scenario: 好友服务查询玩家在线状态

- **WHEN** 好友服务（FRIEND/2001）需要查询 player_id=99 的在线状态
- **THEN** FRIEND/2001 构造消息：`{src: FRIEND/2001, dst_service: GAME, dst_key: player_id=99, body: {action: "query_online_status"}}`
- **AND** 消息发送到 Router
- **AND** Router 根据 player_id=99 一致性哈希找到 GAME 进程 process_id=1001
- **AND** Router 将消息转发到 GAME/1001
- **AND** GAME/1001 查询玩家在线状态，构造响应消息
- **AND** 响应经 Router 路由回 FRIEND/2001

#### Scenario: PVP 服务请求交易服务验证物品

- **WHEN** PVP 服务（PVP/3001）需要在战斗结束后发放奖励，需验证物品合法性
- **THEN** PVP/3001 发送消息到 Router：`{src: PVP/3001, dst_service: TRADE, dst_key: player_id=42, body: {action: "verify_items", items: [...]}}`
- **AND** Router 将消息路由到 TRADE 服务
- **AND** TRADE 服务验证物品并返回结果
- **AND** PVP/3001 根据验证结果发放奖励

#### Scenario: 目标服务不可达

- **WHEN** 微服务 A 发送消息到微服务 B，但微服务 B 所有进程均 OFFLINE
- **THEN** Router 返回错误响应：`{error: "SERVICE_UNAVAILABLE", service: "TRADE"}`
- **AND** 微服务 A 记录 WARN 日志并执行降级逻辑（如暂存请求、稍后重试）
- **AND** 不阻塞微服务 A 的正常运行

---

### Requirement: 数据隔离

每个微服务 SHALL 拥有独立的 DBProxy 连接和数据库命名空间，实现数据隔离。

数据隔离 MUST 满足以下要求：
- 每个微服务进程 MUST 连接独立的 DBProxy 实例（或共享 DBProxy 但使用独立数据库/集合）
- 微服务的数据库命名 MUST 遵循规范：`chaos_{game_id}_{service_type}`（如 `chaos_game001_friend`）
- 微服务 MUST NOT 直接访问其他微服务的数据库
- 跨微服务数据查询 MUST 通过 Router 发送消息到目标微服务，由目标微服务查询自身数据库后返回
- 微服务内部可使用 DBProxy 的主备模式（与现有 DBProxy 架构一致）

#### Scenario: 好友服务数据存储

- **WHEN** 好友服务（FRIEND/2001）需要保存好友关系
- **THEN** FRIEND/2001 通过自身 DBProxy 连接写入数据库 `chaos_game001_friend`
- **AND** 数据存储在 `chaos_game001_friend.relationships` 集合中
- **AND** 其他微服务无法直接访问此数据库

#### Scenario: 跨微服务数据查询

- **WHEN** PVP 服务需要查询玩家的公会信息（存储在 GUILD 服务的数据库中）
- **THEN** PVP 服务 MUST NOT 直接查询 GUILD 服务的数据库
- **AND** PVP 服务通过 Router 发送查询消息到 GUILD 服务
- **AND** GUILD 服务查询自身数据库后返回公会信息
- **AND** PVP 服务使用返回的数据

#### Scenario: 微服务共享 DBProxy 实例

- **WHEN** 多个微服务部署在同一台机器上，共享一个 DBProxy 进程
- **THEN** 各微服务连接到同一 DBProxy 的不同端口或使用不同的数据库名称
- **AND** DBProxy 根据数据库名称隔离不同微服务的数据
- **AND** 微服务 A 的数据库操作不影响微服务 B 的数据

---

### Requirement: 服务扩缩容

微服务 SHALL 支持水平扩缩容，新增或移除进程实例时对系统影响最小。

扩缩容 MUST 满足以下要求：
- 新增微服务进程时，MUST 向 Router 注册并加入一致性哈希环，自动接管部分 player_id 范围
- 移除微服务进程时，MUST 先进入 DRAINING 状态（拒绝新请求，完成进行中请求），然后注销
- 扩缩容过程中 MUST NOT 丢失进行中的请求
- Router 的一致性哈希环 MUST 在扩缩容后自动再均衡
- 数据迁移（如 player_id 范围变更导致的数据重分配）由微服务自身处理，Router 不参与

#### Scenario: 扩容 — 新增好友服务进程

- **WHEN** 好友服务从 2 个进程扩容到 3 个进程（新增 process_id=2003）
- **THEN** process_id=2003 向 Router 注册
- **AND** Router 将 process_id=2003 的虚拟节点加入一致性哈希环
- **AND** 部分 player_id 范围自动从 process_id=2001 和 2002 迁移到 2003
- **AND** 新请求自动路由到 process_id=2003
- **AND** 已在 process_id=2001/2002 上的现有会话不受影响（会话亲和性保持）

#### Scenario: 缩容 — 优雅下线

- **WHEN** 需要将好友服务进程 process_id=2001 下线
- **THEN** process_id=2001 进入 DRAINING 状态，向 Router 发送 DRAIN 通知
- **AND** Router 将 process_id=2001 标记为 DRAINING，停止向其转发新请求
- **AND** process_id=2001 完成所有进行中的请求（最长等待 30s）
- **AND** process_id=2001 向 Router 发送 DEREGISTER
- **AND** Router 从一致性哈希环中移除 process_id=2001
- **AND** 原本路由到 process_id=2001 的 player_id 重新分配到其他进程

#### Scenario: 缩容 — 故障下线

- **WHEN** 好友服务进程 process_id=2001 意外崩溃
- **THEN** Router 通过健康检查检测到故障（3s 心跳超时）
- **AND** Router 将 process_id=2001 标记为 OFFLINE
- **AND** Router 从一致性哈希环中移除 process_id=2001
- **AND** 进行中的请求丢失（客户端需重试）
- **AND** 新请求自动路由到其他可用进程

---

### Requirement: 微服务共享库

系统 SHALL 提供微服务共享 Lua 库，封装服务注册、消息路由等公共逻辑，降低微服务开发成本。

共享库 MUST 满足以下要求：
- 提供 `service_registry.lua`：封装 REGISTER/DEREGISTER/HEARTBEAT 等 Router 通信协议
- 提供 `msg_router.lua`：封装服务间消息的构造、发送、响应处理
- 共享库 MUST 位于 `src_lua/shared/` 目录，所有微服务通过 `require` 引用
- 共享库 MUST 与 ce_net_base C 库配合使用（Lua 层调用 C 扩展的网络能力）

#### Scenario: 微服务使用共享库注册

- **WHEN** 好友服务进程启动
- **THEN** 调用 `service_registry.register("FRIEND", 2001, "192.168.1.10:9100")`
- **AND** 共享库自动完成 TCP 连接、发送 REGISTER 消息、处理响应
- **AND** 共享库自动启动心跳协程，每 1s 向 Router 发送 PONG 响应
- **AND** 进程退出时自动调用 `service_registry.deregister()`

#### Scenario: 微服务使用共享库发送消息

- **WHEN** 好友服务需要查询 GAME 服务的玩家状态
- **THEN** 调用 `msg_router.send({dst_service = "GAME", dst_key = {player_id = 99}, body = {action = "query_status"}})`
- **AND** 共享库自动构造消息、发送到 Router、等待响应
- **AND** 返回响应数据或超时错误
