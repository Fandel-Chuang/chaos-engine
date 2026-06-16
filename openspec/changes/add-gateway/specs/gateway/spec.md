# Gateway — 网络接入层（v0.2 Delta）

> 来源: add-gateway | 状态: 草案 | 变更类型: ADDED

## 概述

新增 Gateway 进程，作为客户端统一的网络接入点。支持 TCP、KCP、WebSocket 三种协议，提供连接管理和消息路由能力。客户端通过 Gateway 接入后，Gateway 根据消息类型将请求转发到对应的后端服务（Game/DBProxy/Admin），后端拓扑对客户端透明。

---

## ADDED Requirements

### Requirement: 多协议接入

Gateway SHALL 同时支持 TCP、KCP、WebSocket 三种协议的客户端接入。

多协议接入 MUST 满足以下要求：
- TCP 服务器监听可配置端口（默认 9000），支持标准 TCP socket 连接
- KCP 服务器监听可配置端口（默认 9001），基于 UDP 传输，集成 KCP 协议保证可靠性和弱网优化
- WebSocket 服务器监听可配置端口（默认 9002），支持 HTTP Upgrade 握手和 RFC 6455 帧协议
- 三种协议的连接 MUST 统一抽象为 Connection 对象，上层路由逻辑不感知底层协议差异
- 每种协议可独立启用/禁用

#### Scenario: TCP 客户端连接

- **WHEN** 客户端通过 TCP 连接到 Gateway 的 TCP 端口
- **THEN** Gateway 接受连接，创建 Connection 对象
- **AND** Connection 标记协议类型为 TCP
- **AND** 客户端可以发送和接收消息

#### Scenario: KCP 客户端连接（弱网环境）

- **WHEN** 移动端客户端通过 KCP（UDP）连接到 Gateway 的 KCP 端口
- **THEN** Gateway 通过 KCP 协议建立可靠连接
- **AND** 在高丢包率（>10%）环境下消息仍能可靠送达
- **AND** KCP 参数（nodelay、interval、resend、nc）可配置

#### Scenario: WebSocket 客户端连接

- **WHEN** Web/H5 客户端发起 WebSocket 连接请求
- **THEN** Gateway 完成 HTTP Upgrade 握手，升级为 WebSocket 连接
- **AND** 支持文本帧（opcode=0x1）和二进制帧（opcode=0x2）
- **AND** 支持 ping/pong 心跳帧（opcode=0x9/0xA）

---

### Requirement: 连接管理

Gateway SHALL 管理客户端连接的完整生命周期。

连接管理 MUST 满足以下要求：
- 连接建立时分配唯一 connection_id，记录协议类型、客户端地址、连接时间
- 支持心跳检测：可配置心跳间隔（默认 30s），超时（默认 90s）未收到心跳则断开连接
- 支持连接数上限（默认 10000），达到上限时拒绝新连接并返回错误
- 连接关闭时清理资源，通知后端服务该客户端已断开
- 支持优雅关闭：收到 SIGTERM 时停止接受新连接，等待现有连接处理完毕后退出

#### Scenario: 心跳保活

- **WHEN** 客户端在心跳间隔内未发送任何消息
- **THEN** Gateway 主动发送 PING 消息
- **AND** 客户端应在超时时间内回复 PONG
- **AND** 超时未回复则判定连接断开，关闭连接并通知后端

#### Scenario: 连接数达到上限

- **WHEN** 当前活跃连接数已达到配置上限（10000）
- **THEN** Gateway 拒绝新的连接请求
- **AND** 向客户端返回 "server busy" 错误消息
- **AND** 记录 WARN 级别日志

#### Scenario: 客户端主动断开

- **WHEN** 客户端主动关闭连接（TCP FIN / WebSocket Close / KCP 超时）
- **THEN** Gateway 清理该连接的 Connection 对象
- **AND** 通知后端服务该 client_id 已断开
- **AND** 记录 INFO 级别日志，包含连接时长和收发字节数

---

### Requirement: 消息路由

Gateway SHALL 根据消息头中的消息类型将客户端消息路由到正确的后端服务。

消息路由 MUST 满足以下要求：
- 消息头包含 msg_type 字段（2 字节），Gateway 根据路由表查找目标后端
- 路由表支持静态配置（配置文件）和动态更新（Admin 命令）
- 支持后端服务列表配置，Gateway 主动健康检查，故障节点自动剔除
- 后端响应消息 MUST 能正确路由回原始客户端
- 未知 msg_type 的消息 MUST 记录 WARN 日志并丢弃

#### Scenario: 游戏消息路由到 Game 进程

- **WHEN** 客户端发送 msg_type=0x0100（玩家移动）的消息
- **THEN** Gateway 查询路由表，找到 msg_type 对应的后端为 Game 进程
- **AND** 将消息转发到 Game 进程的地址
- **AND** Game 进程的响应通过 Gateway 返回给原始客户端

#### Scenario: 数据库查询路由到 DBProxy

- **WHEN** 客户端发送 msg_type=0x0200（查询玩家数据）的消息
- **THEN** Gateway 查询路由表，找到 msg_type 对应的后端为 DBProxy
- **AND** 将消息转发到 DBProxy 的地址
- **AND** DBProxy 的响应通过 Gateway 返回给原始客户端

#### Scenario: 后端服务不可用

- **WHEN** 路由目标后端服务健康检查失败（连续 3 次）
- **THEN** Gateway 将该后端标记为不可用
- **AND** 发往该后端的消息返回错误给客户端（错误码：BACKEND_UNAVAILABLE）
- **AND** 后端恢复后自动重新加入可用列表

---

### Requirement: KCP 协议集成

Gateway SHALL 通过 C 扩展集成 KCP 协议，提供可靠的 UDP 传输通道。

KCP 集成 MUST 满足以下要求：
- C 层封装 KCP 库（`ce_kcp.h/c`），提供创建、更新、输入、发送回调接口
- KCP 参数可配置：nodelay（0/1）、interval（ms）、resend（快速重传次数）、nc（是否关闭拥塞控制）
- KCP 的 `ikcp_update` 调用 MUST 集成到 Gateway 主循环中，按 interval 定时驱动
- KCP 连接 MUST 与 TCP 连接共享统一的 Connection 抽象
- 默认 KCP 参数：nodelay=1, interval=10ms, resend=2, nc=1（流模式）

#### Scenario: KCP 弱网传输

- **WHEN** 网络丢包率达到 20%
- **THEN** KCP 连接仍能可靠传输消息
- **AND** 相比 TCP 在同等丢包率下延迟降低 30%-50%
- **AND** 消息不乱序、不丢失

#### Scenario: KCP 参数调优

- **WHEN** 管理员通过配置文件修改 KCP 参数（如 interval=20ms）
- **THEN** Gateway 重启后使用新参数创建 KCP 连接
- **AND** 新参数对所有新建 KCP 连接生效

---

### Requirement: 进程生命周期

Gateway SHALL 作为独立进程运行，支持标准的启动、运行、关闭流程。

进程生命周期 MUST 满足以下要求：
- 通过 `chaos_gateway` 可执行文件启动，加载 Lua 配置和脚本
- 启动时绑定配置的 TCP/KCP/WebSocket 端口，端口冲突时退出并报错
- 运行时输出结构化日志（JSON 格式），包含时间戳、日志级别、模块名
- 支持 SIGHUP 信号重载配置（不中断现有连接）
- 支持 SIGTERM 信号优雅关闭（停止接受新连接，等待现有连接处理完毕）

#### Scenario: 正常启动

- **WHEN** 执行 `./chaos_gateway -c config.lua`
- **THEN** Gateway 加载配置文件，绑定 TCP:9000、KCP:9001、WebSocket:9002 端口
- **AND** 输出启动日志，包含监听地址和协议
- **AND** 进入事件循环，等待客户端连接

#### Scenario: 端口冲突

- **WHEN** TCP 端口 9000 已被其他进程占用
- **THEN** Gateway 启动失败，输出 ERROR 日志 "port 9000 already in use"
- **AND** 进程退出码为非零

#### Scenario: 优雅关闭

- **WHEN** Gateway 进程收到 SIGTERM 信号
- **THEN** 停止接受新的客户端连接
- **AND** 向所有现有连接发送关闭通知
- **AND** 等待最多 30 秒让现有连接处理完毕
- **AND** 超时后强制关闭剩余连接并退出
