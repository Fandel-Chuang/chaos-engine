# Microservice — Game 进程按功能模块拆分（v0.3 Delta）

> 来源: add-router-cluster | 状态: 草案 | 变更类型: ADDED

## 概述

定义 ChaosEngine 中 Game 进程按功能模块拆分的规范。所有 Game 进程使用同一个 `chaos_game` 二进制，通过加载不同的 Lua 业务脚本（如 `friend.lua`、`pvp.lua`、`trade.lua`）实现不同功能。不同 Game 进程之间通过 Router 进行服务注册和 Game↔Game 通信（调用 `ce_rpc_call()`），Router 提供消息路由能力。**Gateway 直连 Game（核心逻辑进程），不经过 Router**；Game 进程间（如核心 Game↔好友 Game、PVP Game↔交易 Game）的通信通过 Router 中转。每个 Game 进程连接独立的 DBProxy 实例实现数据隔离，支持独立扩缩容。**Game 进程需感知区域归属**：每个 Game 进程明确知道自己所属的大区（如 asia-east），跨区交互通过 Router 全球网格完成。

---

## ADDED Requirements

### Requirement: Game 进程按功能模块定义

系统 SHALL 支持按功能模块将 Game 逻辑拆分为独立的 Game 进程实例，每个功能模块有明确的服务类型标识和职责边界。

Game 进程定义 MUST 满足以下要求：
- 所有 Game 进程使用同一个 `chaos_game` 二进制，通过加载不同的 Lua 业务脚本实现不同功能
- 每种功能模块 MUST 有唯一的 `service_type` 枚举值
- Game 进程 MUST 在启动时向 Router 注册（service_type + process_id + address）
- Game 进程 MUST 在关闭时向 Router 注销
- 同一功能模块可运行多个 Game 进程实例（水平扩展）
- Game 进程间通信 MUST 通过 Router 中转，调用 `ce_rpc_call()` 发送消息

#### 功能模块类型枚举

| service_type | 枚举值 | 职责 | Lua 业务脚本 | 数据范围 |
|-------------|--------|------|-------------|---------|
| GAME | 0x01 | 核心游戏逻辑（玩家移动、AOI、Cell 管理） | game_core.lua | 玩家位置、AOI 状态、Cell 负载 |
| FRIEND | 0x02 | 好友系统（添加/删除好友、好友列表、在线状态） | friend.lua | 好友关系、好友请求 |
| PVP | 0x03 | 玩家对战（匹配、战斗逻辑、结算） | pvp.lua | 战斗记录、段位、排名 |
| PVE | 0x04 | PVE 内容（副本、Boss 战、任务） | pve.lua | 副本进度、Boss 状态、任务数据 |
| TRADE | 0x05 | 交易系统（玩家间交易、拍卖行） | trade.lua | 交易记录、拍卖物品 |
| GUILD | 0x06 | 公会系统（创建/加入公会、公会管理） | guild.lua | 公会信息、成员列表、公会日志 |
| CHAT | 0x07 | 聊天系统（世界/公会/私聊频道） | chat.lua | 聊天记录（可选持久化） |
| MAIL | 0x08 | 邮件系统（系统邮件、玩家邮件、附件） | mail.lua | 邮件数据、附件物品 |

#### Scenario: 好友 Game 进程启动

- **WHEN** 好友 Game 进程（service_type=FRIEND, process_id=2001）启动
- **THEN** 进程加载 `friend.lua` 业务脚本（好友系统的 Lua 业务逻辑）
- **AND** 进程向 Router 发送 REGISTER：`{service_type: "FRIEND", process_id: 2001, address: "192.168.1.10:9100"}`
- **AND** 进程连接独立的 DBProxy 实例（用于好友数据持久化）
- **AND** 进程进入事件循环，等待路由消息

#### Scenario: 同一功能模块多实例

- **WHEN** 好友功能需要扩容，启动第二个 Game 进程（process_id=2002，同样加载 `friend.lua`）
- **THEN** process_id=2002 向 Router 注册，加入一致性哈希环
- **AND** 部分 player_id 的好友请求自动路由到 process_id=2002
- **AND** 两个进程独立运行，通过 Router 通信（如查询对方管理的玩家在线状态）

---

### Requirement: 区域归属感知

每个 Game 进程 MUST 明确感知自己所属的大区，并在注册和通信中携带区域信息。

区域归属 MUST 满足以下要求：
- Game 进程启动时 MUST 从配置中读取所属大区标识（如 `region: "asia-east"`）
- 向 Router 注册时 MUST 携带区域信息：`{service_type, process_id, address, region}`
- Game 进程发送跨区消息时 MUST 通过 Router 全球网格路由，消息包含 `source_region` 和 `target_region`
- 同区 Game 进程间通信 MUST 优先使用本区 Router 集群，不经过跨区连接
- Game 进程 MUST 能处理来自其他大区的请求（如跨服 PVP 挑战），并根据 `source_region` 做出差异化处理

#### Scenario: Game 进程感知区域归属

- **WHEN** asia-east 大区的好友 Game 进程（FRIEND/2001）启动
- **THEN** 进程从配置读取 `region: "asia-east"`
- **AND** 向 Router 注册时携带区域信息：`{service_type: "FRIEND", process_id: 2001, address: "10.0.1.10:9100", region: "asia-east"}`
- **AND** Router 将区域信息纳入服务注册表
- **AND** Game 进程日志中标注区域标识

#### Scenario: 同区通信优先

- **WHEN** asia-east 的 FRIEND Game 进程需要查询 asia-east 的 GAME 进程
- **THEN** 消息通过本区 Router 集群路由，不经过跨区连接
- **AND** 消息不携带跨区标识（source_region == target_region，Router 识别为同区消息）
- **AND** 延迟为同区内网延迟（<1ms）

#### Scenario: 跨区通信

- **WHEN** asia-east 的 PVP Game 进程收到 us-west 玩家的跨服挑战请求
- **THEN** PVP Game 进程从消息中识别 `source_region: "us-west"`
- **AND** PVP Game 进程根据 source_region 执行跨区延迟补偿逻辑
- **AND** 响应消息携带 `target_region: "us-west"`，经 Router 全球网格返回

---

### Requirement: Game 进程间通信（Game↔Game）

不同 Game 进程之间 SHALL 通过 Router 中转进行通信，不建立直接的 TCP 连接。**Gateway 直连 Game（核心逻辑进程），不经过 Router**。Game 进程只需调用 `ce_rpc_call()` 即可发送消息，Router 负责服务发现和消息路由。

进程间通信 MUST 满足以下要求：
- Game 进程 A 向 Game 进程 B 发送消息时，MUST 调用 `ce_rpc_call()` 将消息发送到 Router，由 Router 转发
- 消息 MUST 包含：源 service_type、源 process_id、目标 service_type、目标路由键（player_id 或 process_id）、消息体
- Router MUST 根据目标路由键将消息转发到正确的目标进程
- 请求-响应模式：Game 进程 A 调用 `ce_rpc_call()` 发送请求后等待 Game 进程 B 的响应，Router 负责将响应路由回 Game 进程 A
- 通信协议 MUST 使用与 Gateway 一致的二进制协议（由 ce_net_base 提供）
- 超时请求 MUST 返回错误（默认超时 5s）

#### Scenario: 好友 Game 进程查询玩家在线状态

- **WHEN** 好友 Game 进程（FRIEND/2001）需要查询 player_id=99 的在线状态
- **THEN** FRIEND/2001 调用 `ce_rpc_call()` 构造消息：`{src: FRIEND/2001, dst_service: GAME, dst_key: player_id=99, body: {action: "query_online_status"}}`
- **AND** 消息发送到 Router
- **AND** Router 根据 player_id=99 一致性哈希找到 GAME 进程 process_id=1001
- **AND** Router 将消息转发到 GAME/1001
- **AND** GAME/1001 查询玩家在线状态，构造响应消息
- **AND** 响应经 Router 路由回 FRIEND/2001

#### Scenario: PVP Game 进程请求交易 Game 进程验证物品

- **WHEN** PVP Game 进程（PVP/3001）需要在战斗结束后发放奖励，需验证物品合法性
- **THEN** PVP/3001 调用 `ce_rpc_call()` 发送消息到 Router：`{src: PVP/3001, dst_service: TRADE, dst_key: player_id=42, body: {action: "verify_items", items: [...]}}`
- **AND** Router 将消息路由到 TRADE Game 进程
- **AND** TRADE Game 进程验证物品并返回结果
- **AND** PVP/3001 根据验证结果发放奖励

#### Scenario: 目标进程不可达

- **WHEN** Game 进程 A 发送消息到 Game 进程 B，但 Game 进程 B 所有实例均 OFFLINE
- **THEN** Router 返回错误响应：`{error: "SERVICE_UNAVAILABLE", service: "TRADE"}`
- **AND** Game 进程 A 记录 WARN 日志并执行降级逻辑（如暂存请求、稍后重试）
- **AND** 不阻塞 Game 进程 A 的正常运行

---

### Requirement: 数据隔离

每个 Game 进程 SHALL 拥有独立的 DBProxy 连接和数据库命名空间，实现数据隔离。

数据隔离 MUST 满足以下要求：
- 每个 Game 进程 MUST 连接独立的 DBProxy 实例（或共享 DBProxy 但使用独立数据库/集合）
- Game 进程的数据库命名 MUST 遵循规范：`chaos_{game_id}_{service_type}`（如 `chaos_game001_friend`）
- Game 进程 MUST NOT 直接访问其他 Game 进程的数据库
- 跨 Game 进程数据查询 MUST 通过 Router 发送消息到目标 Game 进程，由目标 Game 进程查询自身数据库后返回
- Game 进程内部可使用 DBProxy 的主备模式（与现有 DBProxy 架构一致）

#### Scenario: 好友 Game 进程数据存储

- **WHEN** 好友 Game 进程（FRIEND/2001）需要保存好友关系
- **THEN** FRIEND/2001 通过自身 DBProxy 连接写入数据库 `chaos_game001_friend`
- **AND** 数据存储在 `chaos_game001_friend.relationships` 集合中
- **AND** 其他 Game 进程无法直接访问此数据库

#### Scenario: 跨 Game 进程数据查询

- **WHEN** PVP Game 进程需要查询玩家的公会信息（存储在 GUILD Game 进程的数据库中）
- **THEN** PVP Game 进程 MUST NOT 直接查询 GUILD Game 进程的数据库
- **AND** PVP Game 进程通过 Router 发送查询消息到 GUILD Game 进程
- **AND** GUILD Game 进程查询自身数据库后返回公会信息
- **AND** PVP Game 进程使用返回的数据

#### Scenario: Game 进程共享 DBProxy 实例

- **WHEN** 多个 Game 进程部署在同一台机器上，共享一个 DBProxy 进程
- **THEN** 各 Game 进程连接到同一 DBProxy 的不同端口或使用不同的数据库名称
- **AND** DBProxy 根据数据库名称隔离不同 Game 进程的数据
- **AND** Game 进程 A 的数据库操作不影响 Game 进程 B 的数据

---

### Requirement: Game 进程扩缩容

Game 进程 SHALL 支持水平扩缩容，新增或移除进程实例时对系统影响最小。

扩缩容 MUST 满足以下要求：
- 新增 Game 进程时，MUST 向 Router 注册并加入一致性哈希环，自动接管部分 player_id 范围
- 移除 Game 进程时，MUST 先进入 DRAINING 状态（拒绝新请求，完成进行中请求），然后注销
- 扩缩容过程中 MUST NOT 丢失进行中的请求
- Router 的一致性哈希环 MUST 在扩缩容后自动再均衡
- 数据迁移（如 player_id 范围变更导致的数据重分配）由 Game 进程自身处理，Router 不参与

#### Scenario: 扩容 — 新增好友 Game 进程

- **WHEN** 好友功能从 2 个 Game 进程扩容到 3 个进程（新增 process_id=2003，加载 `friend.lua`）
- **THEN** process_id=2003 向 Router 注册
- **AND** Router 将 process_id=2003 的虚拟节点加入一致性哈希环
- **AND** 部分 player_id 范围自动从 process_id=2001 和 2002 迁移到 2003
- **AND** 新请求自动路由到 process_id=2003
- **AND** 已在 process_id=2001/2002 上的现有会话不受影响（会话亲和性保持）

#### Scenario: 缩容 — 优雅下线

- **WHEN** 需要将好友 Game 进程 process_id=2001 下线
- **THEN** process_id=2001 进入 DRAINING 状态，向 Router 发送 DRAIN 通知
- **AND** Router 将 process_id=2001 标记为 DRAINING，停止向其转发新请求
- **AND** process_id=2001 完成所有进行中的请求（最长等待 30s）
- **AND** process_id=2001 向 Router 发送 DEREGISTER
- **AND** Router 从一致性哈希环中移除 process_id=2001
- **AND** 原本路由到 process_id=2001 的 player_id 重新分配到其他进程

#### Scenario: 缩容 — 故障下线

- **WHEN** 好友 Game 进程 process_id=2001 意外崩溃
- **THEN** Router 通过健康检查检测到故障（3s 心跳超时）
- **AND** Router 将 process_id=2001 标记为 OFFLINE
- **AND** Router 从一致性哈希环中移除 process_id=2001
- **AND** 进行中的请求丢失（客户端需重试）
- **AND** 新请求自动路由到其他可用进程

---

### Requirement: Game 进程配置与 Lua 脚本组织

系统 SHALL 提供清晰的 Game 进程配置方式和 Lua 业务脚本组织规范，使不同功能模块的 Game 进程易于配置和部署。

配置与脚本组织 MUST 满足以下要求：
- 每个功能模块的 Lua 业务脚本位于 `src_lua/services/` 目录下（如 `src_lua/services/friend.lua`、`src_lua/services/pvp.lua`）
- Game 进程启动时通过配置指定加载的 Lua 脚本：`chaos_game -c config_friend.lua`（配置中指定 `service_script = "services/friend.lua"`）
- Game 进程内置 `ce_rpc_call()` 函数，由引擎核心提供，封装与 Router 的通信（注册、消息发送、心跳）
- 业务脚本只需调用 `ce_rpc_call()` 即可与其他 Game 进程通信，无需关心底层网络细节
- Game 进程配置 MUST 包含：`service_type`、`process_id`、`region`、Router 地址、DBProxy 地址、Lua 业务脚本路径

#### Scenario: 好友 Game 进程配置

- **WHEN** 部署好友功能模块
- **THEN** 创建配置文件 `config_friend.lua`：
  ```lua
  return {
      service_type = "FRIEND",
      process_id = 2001,
      region = "asia-east",
      service_script = "services/friend.lua",
      router_address = "192.168.1.20:9100",
      dbproxy_address = "127.0.0.1:9013",
  }
  ```
- **AND** 启动命令：`./chaos_game -c config_friend.lua`
- **AND** Game 进程自动加载 `friend.lua` 业务脚本，向 Router 注册，连接 DBProxy

#### Scenario: Game 进程调用 ce_rpc_call() 发送消息

- **WHEN** 好友 Game 进程（FRIEND/2001）需要查询 GAME 进程的玩家状态
- **THEN** 在 `friend.lua` 中调用 `ce_rpc_call({dst_service = "GAME", dst_key = {player_id = 99}, body = {action = "query_status"}})`
- **AND** `ce_rpc_call()` 自动构造消息、发送到 Router、等待响应
- **AND** 返回响应数据或超时错误
- **AND** 业务脚本无需关心 Router 地址、连接管理、协议编解码等底层细节
