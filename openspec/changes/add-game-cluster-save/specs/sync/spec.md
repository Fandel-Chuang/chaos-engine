# Sync — Game↔DBProxy TCP 实时同步（v0.2 Delta）

> 来源: add-game-cluster-save | 状态: 草案 | 变更类型: ADDED

## 概述

新增 Sync 模块，实现 Game A 与 DBProxy 之间的 TCP 实时状态同步。采用 Game A → DBProxy(主) 单向推送模型：Game A 将每帧的 ECS 状态变更（操作指令或脏数据）通过 TCP 推送给 DBProxy(主)，DBProxy 在自身内存中维护 Game A 的状态镜像（实时备份）。当 Game A 故障时，玩家掉线，DBProxy(主) 把内存中的备份数据存档到 MongoDB，玩家重新登录时从 MongoDB 恢复数据。当 DBProxy(主) 故障时，Game A 切到本机 DBProxy(备)。同步协议为单向推送，DBProxy 不主动拉取 Game 状态。

---

## ADDED Requirements

### Requirement: Game↔DBProxy 连接管理

Sync 模块 SHALL 支持 Game 进程与 DBProxy 之间的 TCP 连接管理，Game 作为客户端主动连接 DBProxy。

每个 Game 进程 MUST 维护以下连接状态：
- Game A 建立到 DBProxy(主) 的出站 TCP 连接，用于推送状态同步数据
- 连接配置 MUST 包含 DBProxy(主) 的 IP 地址和端口号，以及本机备用 DBProxy 地址

连接建立后，双方 MUST 交换握手消息确认角色（Game/DBProxy）和协议版本。

#### Scenario: Game A 连接 DBProxy(主) 并开始同步

- **WHEN** Game A 启动并配置 DBProxy(主) 地址为远程机器 IP:Port
- **THEN** Game A 通过 TCP 连接到 DBProxy(主)
- **AND** 双方完成握手，Game A 声明角色为 MASTER
- **AND** 连接状态变为 CONNECTED
- **AND** Game A 开始每帧向 DBProxy 推送增量状态更新

#### Scenario: 连接断开重连

- **WHEN** Game 与 DBProxy 之间的 TCP 连接因网络故障断开
- **THEN** Game 自动尝试重连，采用指数退避策略（初始 1s，最大 30s）
- **AND** 优先重连 DBProxy(主)，主不可用时切换到本机备用 DBProxy
- **AND** 重连成功后重新握手，恢复增量同步

---

### Requirement: 状态增量同步（Game → DBProxy 单向推送）

Sync 模块 SHALL 将 Game A 上的 ECS 状态变更实时推送到 DBProxy，DBProxy 在内存中维护状态镜像。同步方向为单向：Game → DBProxy，DBProxy 不主动拉取。

状态同步 MUST 满足以下要求：
- Game A 每帧收集所有 ECS dirty entities 或操作指令，打包为同步帧（SyncFrame），单向推送给 DBProxy(主)
- 同步帧 MUST 包含帧序号（单调递增）、时间戳和变更数据列表
- 每条变更数据 MUST 包含实体 ID、组件类型和变更后的数据
- DBProxy 收到同步帧后更新内存中的状态镜像
- DBProxy 按帧序号顺序应用变更，确保状态一致性

#### Scenario: 正常增量同步

- **WHEN** Game A 上实体 42 的位置变更为 (100, 200, 0)
- **THEN** Game A 将变更打包进当前帧的 SyncFrame
- **AND** SyncFrame 通过 TCP 发送给 DBProxy(主)
- **AND** DBProxy 收到后更新内存中实体 42 的位置为 (100, 200, 0)

#### Scenario: 多实体变更同步

- **WHEN** 同一帧内实体 A 移动、实体 B 属性变更、实体 C 销毁
- **THEN** Game A 将所有变更打包进同一个 SyncFrame
- **AND** DBProxy 收到后按序应用所有变更到内存镜像
- **AND** DBProxy 内存镜像与 Game A 状态保持一致

#### Scenario: 帧序号保证顺序

- **WHEN** DBProxy 收到帧序号为 N+2 的 SyncFrame，但帧 N+1 尚未到达
- **THEN** DBProxy 缓存帧 N+2，等待帧 N+1 到达
- **AND** 按帧序号顺序应用变更，不跳过任何帧
- **AND** 等待超时（5s）后向 Game A 请求重传缺失帧

---

### Requirement: 心跳检测

Sync 模块 SHALL 通过心跳机制检测 Game↔DBProxy 连接的健康状态。

心跳参数 MUST 为：
- 心跳间隔：500ms（Game 每 500ms 向 DBProxy 发送心跳包）
- 超时阈值：3s（连续 3s 未收到心跳响应判定为故障）
- 心跳包 MUST 包含 Game 当前帧序号，用于 DBProxy 确认同步进度

DBProxy 收到心跳后 MUST 立即回复心跳响应，包含已确认的最新帧序号。

#### Scenario: 正常心跳

- **WHEN** Game A 每 500ms 发送心跳包，DBProxy 正常响应
- **THEN** 连接状态保持 CONNECTED
- **AND** Game A 记录最近一次心跳响应时间
- **AND** 心跳 RTT 被记录用于监控

#### Scenario: 心跳超时判定 Game A 故障 → DBProxy 存档

- **WHEN** DBProxy 连续 3s 未收到 Game A 的心跳包
- **THEN** DBProxy 判定 Game A 故障
- **AND** DBProxy 标记 Game A 状态为 DISCONNECTED
- **AND** 所有连接到 Game A 的玩家掉线
- **AND** DBProxy(主) 将内存中的 Game A 备份数据存档到 MongoDB
- **AND** 玩家重新登录时从 MongoDB 恢复数据
- **AND** 记录 WARN 级别日志

#### Scenario: Game 检测 DBProxy 故障

- **WHEN** Game 连续 3s 未收到 DBProxy 的心跳响应
- **THEN** Game 判定当前 DBProxy 故障
- **AND** Game 尝试切换到备用 DBProxy 地址
- **AND** 记录 ERROR 级别日志

---

### Requirement: 故障处理

Sync 模块 SHALL 支持 Game A 故障时 DBProxy 存档，以及 DBProxy(主) 故障时 Game A 切换。

故障处理 MUST 满足以下要求：
- **Game A 挂了**：玩家掉线 → DBProxy(主) 检测到 Game A 心跳超时后，将内存中的 Game A 备份数据存档到 MongoDB → 玩家重新登录时从 MongoDB 恢复
- **DBProxy(主) 挂了**：Game A 检测到 DBProxy(主) 心跳超时后，切换到本机 DBProxy(备)
- **Game A 恢复**：Game A 恢复后，重新连接 DBProxy(主)，恢复增量同步

#### Scenario: Game A 挂了 → DBProxy 存档

- **WHEN** DBProxy 检测到 Game A 心跳超时（3s 无心跳）
- **THEN** 所有连接到 Game A 的玩家掉线
- **AND** DBProxy(主) 将内存中的 Game A 备份数据序列化
- **AND** DBProxy(主) 通过 MongoDB 将备份数据持久化存档
- **AND** 存档完成后记录 INFO 日志，包含存档数据大小和时间戳
- **AND** 标记 Game A 状态为 DISCONNECTED
- **AND** 玩家重新登录时从 MongoDB 恢复数据

#### Scenario: DBProxy(主) 挂了 → A 切到备

- **WHEN** Game A 检测到 DBProxy(主) 心跳超时（3s 无响应）
- **THEN** Game A 将 DBProxy 连接切换到本机 DBProxy(备)
- **AND** 后续数据库操作通过本机 DBProxy(备) 进行
- **AND** 记录 WARN 日志："DBProxy(主) 不可用，切换到本机 DBProxy(备)"

#### Scenario: Game A 恢复后重连

- **WHEN** Game A 进程恢复运行并重新连接 DBProxy(主)
- **THEN** Game A 重新建立 TCP 连接并完成握手
- **AND** Game A 恢复增量状态同步
- **AND** 记录 INFO 日志："Game A 已恢复，重新连接 DBProxy(主)"
