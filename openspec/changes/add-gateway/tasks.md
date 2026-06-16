# Tasks: Gateway 网络接入层

> 按依赖顺序排列，Phase 1-3。全部待实现。

---

## Phase 1: TCP 接入

- [ ] 1.1 创建 `src_lua/gateway/` 目录，创建 `init.lua` 入口：加载配置、初始化各子模块、启动事件循环
- [ ] 1.2 创建 `config.lua`：定义 Gateway 配置（TCP 端口 9000、KCP 端口 9001、WebSocket 端口 9002、后端服务地址列表、心跳间隔、连接数上限、日志级别），支持命令行参数和配置文件覆盖
- [ ] 1.3 创建 `server.lua`：实现 TCP 服务器监听（基于 LuaSocket），accept 客户端连接，为每个连接创建 Lua 协程处理
- [ ] 1.4 创建 `connection.lua`：定义 Connection 对象（connection_id、协议类型、客户端地址、连接时间、收发缓冲区），实现 `connection:send()` 和 `connection:recv()` 方法
- [ ] 1.5 创建 `protocol.lua`：定义消息头格式（msg_type 2 字节 + body_length 4 字节大端 + body），实现 `protocol:pack(msg_type, body)` 和 `protocol:unpack(data)` 编解码
- [ ] 1.6 创建 `router.lua`：实现路由表（msg_type → 后端地址映射），支持静态配置加载，实现 `router:route(msg_type)` 查找目标后端，实现 `router:forward(conn, msg)` 转发消息到后端并回传响应
- [ ] 1.7 创建 `heartbeat.lua`：实现心跳检测定时器，每 30s 检查连接活跃度，超时 90s 未收到消息则关闭连接
- [ ] 1.8 创建 `backend.lua`：实现后端服务连接池（到 Game/DBProxy/Admin 的 TCP 长连接），健康检查（定时 PING），故障节点自动剔除和恢复

---

## Phase 2: KCP 集成

- [ ] 2.1 集成 kcp-c 库到 `src_c/network/`，添加 `ikcp.h` 和 `ikcp.c` 源文件
- [ ] 2.2 创建 `src_c/network/ce_kcp.h`：定义 `CeKcpContext` 不透明句柄、`ce_kcp_create/destroy/update/input/send/recv/set_config` 接口
- [ ] 2.3 实现 `ce_kcp.c`：封装 KCP 创建（`ikcp_create`）、更新驱动（`ikcp_update`）、数据输入（`ikcp_input`）、输出回调（发送 UDP 数据包）、接收接口（从 KCP 缓冲区读取可靠数据）、配置参数设置（nodelay/interval/resend/nc）
- [ ] 2.4 创建 Lua C 绑定：将 `ce_kcp` 接口通过 Lua C API 暴露给 Gateway（`luaopen_gateway_kcp`），支持在 Lua 中创建和管理 KCP 连接
- [ ] 2.5 修改 `server.lua`：添加 KCP 服务器监听（UDP socket），为每个 KCP 客户端创建 KCP 控制块，在主循环中定时调用 `ikcp_update` 驱动
- [ ] 2.6 修改 `connection.lua`：扩展 Connection 对象支持 KCP 协议类型，统一 TCP/KCP 连接的 send/recv 接口
- [ ] 2.7 修改 `config.lua`：添加 KCP 参数配置（nodelay=1, interval=10, resend=2, nc=1）
- [ ] 2.8 修改 `src_c/network/CMakeLists.txt`：添加 KCP 源文件编译，添加 `CHAOS_HAS_GATEWAY` 条件编译选项

---

## Phase 3: WebSocket 支持

- [ ] 3.1 创建 `websocket.lua`：实现 WebSocket 握手（解析 HTTP Upgrade 请求、验证 Sec-WebSocket-Key、生成 Accept、返回 101 响应）
- [ ] 3.2 实现 WebSocket 帧编解码：支持文本帧（opcode=0x1）、二进制帧（opcode=0x2）、关闭帧（opcode=0x8）、PING/PONG 帧（opcode=0x9/0xA），实现 mask/unmask 处理
- [ ] 3.3 修改 `server.lua`：添加 WebSocket 服务器监听（TCP 端口 9002），HTTP Upgrade 握手后升级为 WebSocket 连接，创建 Connection 对象（协议类型=WebSocket）
- [ ] 3.4 修改 `connection.lua`：扩展 Connection 对象支持 WebSocket 协议类型，send 方法自动封装 WebSocket 帧，recv 方法自动解析帧
- [ ] 3.5 修改 `heartbeat.lua`：WebSocket 连接使用标准 PING/PONG 帧进行心跳检测
- [ ] 3.6 修改 `config.lua`：添加 WebSocket 相关配置（端口、最大帧大小、是否启用压缩扩展）

---

## Phase 4: 集成与测试

- [ ] 4.1 创建 `src_c/runtime/ce_gateway_main.c`：Gateway 进程入口，嵌入 Lua VM，加载 `init.lua`，启动事件循环
- [ ] 4.2 CMake 集成：在顶层 `CMakeLists.txt` 添加 `CHAOS_HAS_GATEWAY` option，添加 KCP 子目录编译，Gateway 可执行文件链接 KCP 库和 Lua
- [ ] 4.3 编写 TCP 接入测试：启动 Gateway + Game 进程，TCP 客户端连接 → 发送消息 → 验证路由到 Game → 验证响应返回
- [ ] 4.4 编写 KCP 接入测试：启动 Gateway + Game 进程，KCP 客户端连接 → 模拟丢包环境 → 验证消息可靠送达
- [ ] 4.5 编写 WebSocket 接入测试：启动 Gateway + Game 进程，WebSocket 客户端连接 → 握手验证 → 消息收发验证
- [ ] 4.6 编写连接管理测试：心跳超时断开、连接数上限拒绝、客户端主动断开通知后端、优雅关闭
- [ ] 4.7 编写消息路由测试：正确路由到 Game/DBProxy/Admin、未知 msg_type 丢弃、后端不可用返回错误、后端恢复自动重连
- [ ] 4.8 编写性能基准测试：并发连接数（目标 10000）、消息吞吐量（P50/P99 延迟）、KCP vs TCP 弱网对比

---

**总进度：0/22 已完成（0%）**

**全部待实现**：TCP 接入（8 项）、KCP 集成（8 项）、WebSocket 支持（6 项）、集成与测试（8 项，含 Phase 4）。
