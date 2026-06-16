# add-gateway

Gateway 网络接入层：TCP/KCP/WebSocket 多协议支持，连接管理，消息路由

> 来源: add-gateway | 状态: 规划中 | 变更类型: ADDED

---

## Why

客户端需要统一的网络接入点来连接游戏服务器。当前 ChaosEngine 缺乏独立的网络接入层，存在以下问题：

1. **无统一接入点**：客户端直接连接 Game 进程，暴露内部拓扑，不利于负载均衡和水平扩展。
2. **协议单一**：仅支持 TCP，无法适应移动端弱网场景（需要 KCP）和 Web 端接入（需要 WebSocket）。
3. **连接管理缺失**：无连接池、心跳管理、断线重连、流量控制等基础能力，每个 Game 进程需自行实现。
4. **消息路由空白**：客户端消息无法根据路由规则转发到正确的后端服务（Game/DBProxy/Admin），缺乏服务发现能力。

引入 Gateway 进程后，可实现：
- 客户端通过 Gateway 统一接入，Gateway 负责协议适配和消息路由，后端服务对客户端透明
- 支持 TCP（可靠传输）、KCP（弱网优化）、WebSocket（Web/H5 接入）三种协议
- 连接生命周期管理（建立、心跳、断线检测、重连、关闭）
- 消息路由：根据消息类型将请求转发到 Game 进程、DBProxy 或 Admin 服务
- 纯 C 网络层（KCP 集成）+ Lua Gateway 进程，保持与引擎现有技术栈一致

## What Changes

### 新增模块

1. **C 网络层 KCP 集成（`ce_kcp`，C）**：`src_c/network/ce_kcp.h/c` — 集成 KCP 协议库，提供可靠的 UDP 传输通道。封装 KCP 的创建、更新（`ikcp_update`）、输入（`ikcp_input`）、发送/接收回调。支持 KCP 参数配置（nodelay、interval、resend、nc）。与现有 TCP socket 抽象层统一接口。

2. **Gateway 进程（Lua）**：`src_lua/gateway/` — 独立 Lua 进程，基于 Lua 5.4 + LuaSocket。实现多协议监听（TCP/KCP/WebSocket）、连接管理、消息路由。进程由 `chaos_gateway` 可执行文件启动。

### 架构决策

| 决策 | 选择 | 理由 |
|------|------|------|
| Gateway 语言 | Lua 5.4 | 与引擎现有 Lua 生态一致（`src_lua/admin/`、`src_lua/dbproxy/`），开发效率高，热更新友好 |
| KCP 实现 | C 扩展（集成 kcp-c） | KCP 是纯 C 实现，性能关键路径用 C 保证吞吐，通过 Lua C API 暴露给 Gateway |
| 协议支持 | TCP + KCP + WebSocket | TCP 覆盖 PC/内网场景，KCP 覆盖移动弱网，WebSocket 覆盖 Web/H5 |
| 连接模型 | 每连接一协程 | Lua 协程轻量（~1KB），支持数万并发连接，代码线性编写避免回调地狱 |
| 消息路由 | 消息头路由表 | 消息头包含 msg_type，Gateway 查表转发到对应后端，支持动态更新路由规则 |
| 后端服务发现 | 静态配置 + 健康检查 | 初期静态配置后端地址列表，Gateway 主动健康检查，故障节点自动剔除 |
| 编译策略 | CMake option + `#ifdef` 条件编译 | `CHAOS_HAS_GATEWAY` 控制 KCP C 扩展编译，非 Server 构建零开销 |

## Impact

### 受影响的代码

| 文件/目录 | 影响类型 | 说明 |
|-----------|----------|------|
| `src_c/network/ce_kcp.h` | **新增** | KCP 协议封装接口（创建、更新、收发、配置） |
| `src_c/network/ce_kcp.c` | **新增** | KCP 协议封装实现（集成 kcp-c 库） |
| `src_lua/gateway/` | **新增** | Lua Gateway 进程（init.lua, server.lua, connection.lua, router.lua, config.lua） |
| `src_c/runtime/ce_gateway_main.c` | **新增** | Gateway 进程入口（嵌入 Lua VM，启动 Gateway 服务） |
| `src_c/network/CMakeLists.txt` | 修改 | 添加 KCP 源文件和 `CHAOS_HAS_GATEWAY` 条件编译 |
| `src_c/CMakeLists.txt` | 修改 | 添加 `CHAOS_HAS_GATEWAY` 选项 |

### 不受影响的模块

- ECS、AOI、Cell、Memory、Log、Render 核心逻辑不变
- Game 进程、DBProxy 进程、Admin 进程不受影响（Gateway 是新增独立进程）
- Client/Headless 编译目标不受影响
- 不带 `CHAOS_HAS_GATEWAY` 编译时，现有构建完全不变

### MVP 范围

**Phase 1：TCP 接入** — TCP 服务器监听、连接 accept、简单的消息收发和转发。Gateway 监听 TCP 端口，接受客户端连接，解析消息头，根据 msg_type 转发到后端 Game 进程。包含基础的连接管理（建立/关闭）。

**Phase 2：KCP 集成** — C 层 KCP 协议封装（`ce_kcp.h/c`），Lua 侧 KCP 服务器监听。支持移动端弱网场景，KCP 参数可配置（nodelay、interval、resend、nc）。KCP 连接与 TCP 连接统一抽象为 Connection 对象。

**Phase 3：WebSocket 支持** — WebSocket 握手（HTTP Upgrade）、帧编解码（RFC 6455）、文本/二进制消息支持。允许 Web/H5 客户端通过 WebSocket 接入 Gateway，消息路由与 TCP/KCP 统一。

**延后到后续版本**：
- 连接数上限的弹性伸缩（多 Gateway 实例 + 负载均衡）
- 消息压缩和加密（TLS/DTLS）
- 全功能服务发现（集成 etcd/Consul）
- 流量控制和限流
- 消息优先级队列
- 连接迁移（客户端无感切换到其他 Gateway 实例）
