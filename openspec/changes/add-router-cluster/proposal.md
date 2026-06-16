# add-router-cluster

Router 集群：服务发现 + 服务器间消息路由 + 微服务拆分

> 来源: add-router-cluster | 状态: 规划中 | 变更类型: ADDED

---

## Why

ChaosEngine v0.3 已具备 Gateway 网络接入层和 DBProxy 数据持久化层，但服务端内部通信和微服务拆分仍存在以下核心问题：

1. **无服务发现机制**：Gateway 通过静态配置转发消息到 Game 进程，无法动态感知 Game 进程的上线/下线/扩缩容。新增 Game 进程需要手动修改 Gateway 配置并重启，运维成本高。
2. **无服务器间消息路由**：不同 Game 进程之间（如好友服务需要查询另一个玩家的在线状态）无法直接通信。当前所有 Game 逻辑耦合在单一进程中，无法按功能模块拆分。
3. **微服务拆分缺乏基础设施**：随着游戏功能增长（好友、PVP、PVE、交易、公会等），单一 Game 进程成为性能瓶颈和单点故障。需要将不同功能模块拆分为独立微服务进程，但缺乏统一的服务间通信层。
4. **全球同服多区部署是核心架构需求**：ChaosEngine 定位为全球同服游戏引擎，必须原生支持多区域部署（亚洲区/欧洲区/美洲区等）。每个大区独立部署完整集群，大区之间通过 Router 跨区路由进行交互（如跨服 PVP、全球聊天、跨区交易）。跨区域消息转发不是延后功能，而是 Phase 1 就必须具备的核心能力。
5. **Gateway 与 Router 网络逻辑重复**：Gateway 和 Router 都需要 TCP 连接管理、心跳检测、二进制协议编解码等基础网络能力。若不抽离共享库，将导致代码重复和维护负担。

引入 Router 集群后，可实现：
- Router 作为服务注册中心，Game 进程启动时自动注册，Gateway 从 Router 查询路由表
- 服务器间消息通过 Router 中转，支持按 player_id 一致性哈希路由到目标微服务进程
- 微服务按功能模块拆分（好友/PVP/PVE/交易等独立 Game 进程），通过 Router 通信
- 全球同服：全球分多个大区（亚洲区/欧洲区/美洲区），每个大区独立部署完整集群（Gateway + Router 集群 + 微服务 + DBProxy + MongoDB）。大区之间通过 Router 跨区 TCP 长连接互联，组成全球 Router 网格，支持跨区消息路由（跨服 PVP、全球聊天、跨区交易等）
- 共享网络库 `ce_net_base`（C）抽离 Gateway 和 Router 的公共网络逻辑，避免代码重复

## What Changes

### 新增模块

1. **共享网络库（`ce_net_base`，C）**：`src_c/network/ce_net_base.h/c` — 从 Gateway 和 Router 中抽离公共网络逻辑。包含 TCP 连接管理（建立/关闭/重连）、二进制协议编解码（长度前缀 + 消息类型 + 变长编码）、心跳检测（可配置间隔和超时）、连接池管理。Gateway 和 Router 均链接此库，消除代码重复。

2. **Router 进程（Lua）**：`src_lua/router/` — 独立 Lua 进程，基于 Lua 5.4 + LuaSocket。实现服务注册与发现、消息路由（一致性哈希）、Router 集群内部通信（广播路由表）、跨区域转发。进程由 `chaos_router` 可执行文件启动。

3. **微服务拆分规范**：定义微服务类型枚举（好友/PVP/PVE/交易/公会等），每个微服务为独立 Game 进程，通过 Router 注册和通信。每个微服务连接独立的 DBProxy 实例实现数据隔离。

### 架构决策

| 决策 | 选择 | 理由 |
|------|------|------|
| Router 语言 | Lua 5.4 | 与引擎现有 Lua 生态一致（Gateway、DBProxy、Admin），开发效率高，热更新友好 |
| 共享网络库 | C（ce_net_base） | 网络层性能关键路径用 C 保证吞吐，Gateway 和 Router 共享避免代码重复 |
| 服务发现 | Router 集中注册 + 广播同步 | 无需外部服务（etcd/Consul），Router 集群内广播路由表，降低部署复杂度 |
| 消息路由算法 | 一致性哈希（player_id → 虚拟节点 → 目标进程） | 玩家与微服务进程的亲和性绑定，进程扩缩容时最小化数据迁移 |
| 跨区域通信 | Router 间 TCP 长连接 + 消息转发 | 简单可靠，无需额外的跨区域消息队列；每个区域的 Router 与其他区域 Router 建立全互联 |
| 微服务通信 | 通过 Router 中转 | 微服务之间不直连，所有消息经 Router 路由，集中管控和监控 |
| 数据隔离 | 每个微服务独立 DBProxy 连接 | 避免微服务间的数据库耦合，每个微服务拥有独立的数据库连接和集合命名空间 |
| 编译策略 | CMake option + `#ifdef` 条件编译 | `CHAOS_HAS_ROUTER` 控制 ce_net_base 编译，非 Server 构建零开销 |

## Impact

### 受影响的代码

| 文件/目录 | 影响类型 | 说明 |
|-----------|----------|------|
| `src_c/network/ce_net_base.h` | **新增** | 共享网络库接口（连接管理、协议编解码、心跳、连接池） |
| `src_c/network/ce_net_base.c` | **新增** | 共享网络库实现 |
| `src_lua/router/` | **新增** | Lua Router 进程（init.lua, registry.lua, router.lua, cluster.lua, health.lua, config.lua） |
| `src_c/runtime/ce_router_main.c` | **新增** | Router 进程入口（嵌入 Lua VM，启动 Router 服务） |
| `src_lua/gateway/` | 修改 | Gateway 重构：网络层改用 ce_net_base，路由查询改为从 Router 动态获取 |
| `src_c/runtime/ce_gateway_main.c` | 修改 | 链接 ce_net_base 替代内联网络逻辑 |
| `src_c/network/CMakeLists.txt` | 修改 | 添加 ce_net_base 源文件和 `CHAOS_HAS_ROUTER` 条件编译 |
| `src_c/CMakeLists.txt` | 修改 | 添加 `CHAOS_HAS_ROUTER` 选项 |
| `src_lua/shared/` | **新增** | 微服务共享 Lua 库（service_registry.lua, msg_router.lua） |

### 不受影响的模块

- ECS、AOI、Cell、Memory、Log、Render 核心逻辑不变
- DBProxy 进程不受影响（微服务各自连接独立 DBProxy 实例，DBProxy 内部逻辑不变）
- Admin 进程不受影响
- Client/Headless 编译目标不受影响
- 不带 `CHAOS_HAS_ROUTER` 编译时，现有构建完全不变

### MVP 范围

**Phase 1：ce_net_base 共享网络库 + 跨区路由基础** — 从 Gateway 抽离 TCP 连接管理、二进制协议编解码、心跳检测、连接池等公共逻辑为独立 C 库。Gateway 重构以链接 ce_net_base。同时实现 Router 跨区路由基础：定义跨区消息格式（包含 source_region + target_region）、区域间 Router TCP 长连接互联、全球 Router 网格拓扑。验证 Gateway 功能不受影响，验证跨区消息格式和连接框架。

**Phase 2：Router 基础** — Router 进程启动、服务注册与发现（Game 进程启动时向 Router 注册）、消息路由（player_id → 一致性哈希 → 目标进程）、Router 集群内广播路由表。Gateway 改为从 Router 查询路由表。

**Phase 3：微服务拆分** — 定义微服务类型枚举，以好友服务为示例拆分为独立 Game 进程。好友服务通过 Router 注册，客户端消息经 Gateway → Router → 好友服务。验证服务间通信正确性。

**Phase 4：跨区域路由集成** — 基于 Phase 1 的跨区路由基础，完成全球多区域部署的完整集成。多区域部署（如 us-west、ap-southeast、eu-west），每个大区独立部署完整集群。玩家跨区域交互（如跨服 PVP、全球聊天、跨区交易）的端到端验证。跨区消息完整链路测试。

**Phase 5：集成与测试** — 全链路集成测试（Gateway → Router → 微服务 → DBProxy），异常场景测试（Router 故障、微服务扩缩容、跨区域网络分区），性能基准测试。

**延后到后续版本**：
- Router 多活负载均衡（当前为单 Router per 区域）
- 微服务动态扩缩容的自动再均衡
- 消息优先级队列和流量控制
- 跨区域消息压缩和加密
- 基于 Raft 的 Router 集群一致性（当前为广播同步）
- 微服务网格（Service Mesh）化
