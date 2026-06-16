# DBProxy — 数据库代理层（v0.2 Delta）

> 来源: add-game-cluster-save | 状态: 草案 | 变更类型: ADDED

## 概述

新增 DBProxy 模块，作为 Game 进程与 MongoDB 之间的数据库代理层，同时承担 Game A 内存状态的实时备份存储职责。采用主备架构：主 DBProxy 部署在远程机器，本机部署备用 DBProxy。Game A 通过 TCP 二进制协议将内存状态实时同步到 DBProxy(主)，DBProxy 在自身内存中维护状态镜像（实时备份）。Game A 挂了时，玩家掉线，DBProxy(主) 自动将备份数据存档到 MongoDB，玩家重新登录时从 MongoDB 恢复。DBProxy(主) 挂了时，Game A 切到本机 DBProxy(备)。DBProxy 使用 mongoc 驱动将持久化数据以 BSON 文档格式存储到 MongoDB。DBProxy 以独立的 Lua 进程运行，支持热更新。

---

## ADDED Requirements

### Requirement: DBProxy 主备架构

DBProxy 模块 SHALL 采用主备架构，主 DBProxy 跨机器部署，备用 DBProxy 与本机 Game 进程同机部署。

架构 MUST 满足以下要求：
- 每个 Game 进程所在机器 MUST 部署一个备用 DBProxy 进程
- 主 DBProxy MUST 部署在独立的数据库服务器上
- Game 进程优先连接主 DBProxy，主不可用时自动切换到备用 DBProxy
- 主备 DBProxy 均连接同一个 MongoDB 集群，数据最终一致

#### Scenario: 正常运行时连接主 DBProxy

- **WHEN** 主 DBProxy 正常运行且可达
- **THEN** Game 进程通过 TCP 连接到主 DBProxy
- **AND** 所有数据库读写请求发送到主 DBProxy
- **AND** 备用 DBProxy 处于待命状态，不处理请求

#### Scenario: 跨机器主 DBProxy 部署

- **WHEN** 数据库服务器（192.168.1.100）上运行主 DBProxy，Game 服务器（192.168.1.10）上运行 Game 进程和备用 DBProxy
- **THEN** Game 进程配置主 DBProxy 地址为 192.168.1.100:9000
- **AND** Game 进程配置备用 DBProxy 地址为 127.0.0.1:9000
- **AND** 主备 DBProxy 均连接 MongoDB 集群（192.168.1.200:27017）

---

### Requirement: Game 内存状态实时备份

DBProxy SHALL 在自身内存中维护 Game 进程的状态镜像，作为 Game 进程的实时备份数据存储。

内存状态备份 MUST 满足以下要求：
- DBProxy(主) MUST 在内存中维护一份 Game A 状态的完整镜像（实时备份数据）
- Game A 每帧通过 TCP 向 DBProxy 推送增量状态更新（操作指令或脏数据）
- DBProxy 按帧序号顺序应用变更，保持内存镜像与 Game A 状态一致
- 同步方向为单向：Game → DBProxy 推送，DBProxy 不主动拉取
- Game A 挂了时，玩家掉线，DBProxy(主) 将内存中的备份数据存档到 MongoDB，玩家重新登录时从 MongoDB 恢复
- DBProxy(主) 故障时，Game A 切换到本机备用 DBProxy

#### Scenario: Game A 推送增量状态到 DBProxy

- **WHEN** Game A 每帧收集 dirty entities 并序列化为 SyncFrame
- **THEN** Game A 通过 TCP 将 SyncFrame 发送给 DBProxy(主)
- **AND** DBProxy 解析 SyncFrame，按帧序号顺序应用到内存镜像
- **AND** DBProxy 内存镜像中的实体状态与 Game A 保持一致

#### Scenario: Game A 挂了 → DBProxy 存档备份数据

- **WHEN** DBProxy(主) 检测到 Game A 心跳超时（3s 无心跳）
- **THEN** DBProxy(主) 判定 Game A 故障
- **AND** 所有连接到 Game A 的玩家掉线
- **AND** DBProxy 将当前内存镜像序列化为二进制快照
- **AND** DBProxy 通过 MongoDB 将快照持久化存档
- **AND** 记录 INFO 日志："Game A 故障，备份数据已存档到 MongoDB"
- **AND** 玩家重新登录时从 MongoDB 恢复数据

#### Scenario: DBProxy(主) 故障时 Game A 切到备

- **WHEN** DBProxy(主) 不可用，Game 切换到本机备用 DBProxy
- **THEN** 备用 DBProxy 承担数据库代理职责
- **AND** Game A 后续数据库操作通过本机 DBProxy(备) 进行
- **AND** 记录 WARN 日志："DBProxy(主) 不可用，切换到本机 DBProxy(备)"
- **AND** 本机 DBProxy(备) 不存储 Game A 内存备份数据，仅提供数据库代理服务

---

### Requirement: 主备切换

DBProxy 模块 SHALL 支持主备自动切换，确保数据库服务在 DBProxy 故障时不中断。

主备切换 MUST 满足以下要求：
- Game 进程检测到主 DBProxy 连接断开或超时后，自动切换到备用 DBProxy
- 主 DBProxy 恢复后，Game 进程自动切回主 DBProxy
- 切换过程 MUST 对 Game 逻辑层透明（仅 DBProxy 客户端层感知）
- 切换期间未完成的请求 MUST 重试（最多 3 次）

#### Scenario: 主 DBProxy 不可用时切到备用

- **WHEN** Game 进程检测到主 DBProxy 连接断开（TCP 连接重置或超时）
- **THEN** DBProxy 客户端自动将后续请求路由到备用 DBProxy
- **AND** 当前正在执行的请求重试（最多 3 次）
- **AND** 记录 WARN 日志："主 DBProxy 不可用，切换到备用 DBProxy"
- **AND** Game 逻辑层不感知切换，存档和查询正常完成

#### Scenario: 主 DBProxy 恢复后切回

- **WHEN** 主 DBProxy 恢复运行，Game 进程检测到主 DBProxy 重新可达
- **THEN** DBProxy 客户端将后续请求路由回主 DBProxy
- **AND** 备用 DBProxy 恢复待命状态
- **AND** 记录 INFO 日志："主 DBProxy 已恢复，切回主 DBProxy"

#### Scenario: 主备同时不可用

- **WHEN** 主 DBProxy 和备用 DBProxy 均不可用
- **THEN** DBProxy 客户端返回错误给 Game 逻辑层
- **AND** 存档请求进入重试队列，等待任一 DBProxy 恢复
- **AND** 记录 ERROR 日志："所有 DBProxy 不可用"
- **AND** Game 进程继续运行，不崩溃

---

### Requirement: Game↔DBProxy 通信协议

Game 进程与 DBProxy 之间的通信 SHALL 使用 TCP + 二进制协议。

通信协议 MUST 满足以下要求：
- 传输层使用 TCP，确保可靠传输
- 消息格式为二进制协议：固定长度头部 + 可变长度载荷
- 消息头部 MUST 包含：消息总长度（4 字节）、操作码（2 字节）、请求 ID（4 字节）、标志位（2 字节）
- 支持的操作码 MUST 包括：CONNECT、DISCONNECT、INSERT、FIND、UPDATE、DELETE、PING
- 请求-响应模式：每个请求 MUST 有对应的响应，通过请求 ID 匹配

#### Scenario: 二进制协议 — INSERT 操作

- **WHEN** Game 进程需要保存玩家数据
- **THEN** DBProxy 客户端构造 INSERT 请求消息：头部（opcode=INSERT, req_id=自动分配）+ 载荷（BSON 文档）
- **AND** 消息通过 TCP 发送给 DBProxy
- **AND** DBProxy 解析头部，识别为 INSERT 操作
- **AND** DBProxy 将 BSON 载荷写入 MongoDB
- **AND** DBProxy 返回 INSERT 响应：头部（opcode=INSERT, req_id=匹配请求）+ 载荷（插入结果状态码）

#### Scenario: 二进制协议 — FIND 操作

- **WHEN** Game 进程需要查询玩家数据
- **THEN** DBProxy 客户端构造 FIND 请求：头部（opcode=FIND）+ 载荷（BSON 查询条件）
- **AND** DBProxy 执行 MongoDB 查询
- **AND** DBProxy 返回 FIND 响应：头部 + 载荷（BSON 结果文档数组）
- **AND** Game 进程通过 req_id 匹配请求和响应

#### Scenario: 心跳 PING/PONG

- **WHEN** Game 进程需要检测 DBProxy 连接健康状态
- **THEN** DBProxy 客户端发送 PING 请求（opcode=PING，载荷为空）
- **AND** DBProxy 收到后立即返回 PONG 响应
- **AND** 客户端记录 RTT 用于监控
- **AND** PING 间隔为 10s，超时 5s 判定连接故障

---

### Requirement: DBProxy↔MongoDB 数据存储

DBProxy SHALL 使用 mongoc 驱动连接 MongoDB，以 BSON 文档格式存储游戏数据。

数据存储 MUST 满足以下要求：
- DBProxy 使用 mongoc 客户端库（libmongoc）连接 MongoDB
- 所有游戏数据 MUST 以 BSON 文档格式存储
- 数据库命名规范：`chaos_{game_id}`，集合命名规范：`{collection_type}`（如 players、items、world）
- DBProxy MUST 支持连接池（默认最小 2、最大 10 个连接）
- 写操作 MUST 使用 write concern "majority"，确保数据安全

#### Scenario: 玩家数据 BSON 存储

- **WHEN** Game 进程请求保存玩家数据（player_id=1001, name="Alice", level=50, position={x:100,y:200,z:0}）
- **THEN** DBProxy 将数据构造为 BSON 文档：`{"_id": 1001, "name": "Alice", "level": 50, "position": {"x": 100, "y": 200, "z": 0}}`
- **AND** 执行 MongoDB upsert 操作写入 `chaos_game001.players` 集合
- **AND** write concern 为 "majority"，确保写入多数节点后返回成功

#### Scenario: 连接池管理

- **WHEN** DBProxy 启动时
- **THEN** 初始化 mongoc 连接池，最小连接数 2
- **AND** 高并发时连接池自动扩容，最大连接数 10
- **AND** 空闲连接在 60s 后回收，保持最小连接数

#### Scenario: MongoDB 连接失败处理

- **WHEN** MongoDB 集群不可达
- **THEN** DBProxy 返回错误响应给 Game 进程（错误码 + 错误描述）
- **AND** DBProxy 自动重试 MongoDB 连接（间隔 5s）
- **AND** MongoDB 恢复后 DBProxy 自动恢复服务

---

### Requirement: Lua DBProxy 进程

DBProxy SHALL 以独立的 Lua 进程运行，支持热更新。

Lua DBProxy 进程 MUST 满足以下要求：
- DBProxy 进程基于 Lua 运行时实现，与 Game 进程完全独立
- DBProxy 进程 MUST 支持热更新：接收更新信号后重新加载 Lua 脚本，不中断现有连接
- DBProxy 进程 MUST 支持优雅关闭：收到 SIGTERM 后完成所有进行中的请求再退出
- DBProxy 进程 MUST 记录结构化日志（JSON 格式），包含请求耗时、操作类型和结果状态

#### Scenario: DBProxy 独立进程启动

- **WHEN** 执行 `./chaos_dbproxy --config dbproxy.conf` 启动 DBProxy
- **THEN** DBProxy 进程启动，监听配置的 TCP 端口（默认 9000）
- **AND** 初始化 mongoc 驱动并连接 MongoDB
- **AND** 打印启动日志，包含监听地址、MongoDB 连接状态和进程 PID
- **AND** 进程独立于 Game 进程运行，崩溃不影响 Game

#### Scenario: Lua 热更新

- **WHEN** 运维人员发送 SIGUSR1 信号或调用热更新接口
- **THEN** DBProxy 重新加载 Lua 业务逻辑脚本
- **AND** 现有 TCP 连接不中断
- **AND** 正在执行的请求使用旧版本逻辑完成
- **AND** 新请求使用更新后的逻辑处理
- **AND** 记录 INFO 日志："DBProxy 热更新完成，版本: xxx"

#### Scenario: 优雅关闭

- **WHEN** DBProxy 进程收到 SIGTERM 信号
- **THEN** DBProxy 停止接受新连接
- **AND** 等待所有进行中的请求完成（最长等待 30s）
- **AND** 超时后强制关闭剩余连接
- **AND** 关闭 MongoDB 连接池
- **AND** 记录关闭日志并退出

#### Scenario: 结构化日志

- **WHEN** DBProxy 处理一个 INSERT 请求
- **THEN** 输出 JSON 格式日志：`{"ts":"2026-06-16T10:00:00Z","op":"INSERT","db":"chaos_game001","coll":"players","latency_us":1234,"status":"ok"}`
- **AND** 请求失败时日志包含 `error` 字段和错误码
- **AND** 日志同时输出到 stdout 和文件 `logs/dbproxy/YYYY-MM-DD.log`
