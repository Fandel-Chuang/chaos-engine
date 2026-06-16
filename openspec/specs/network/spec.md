# Network — 网络层（Socket 抽象、属性复制、帧同步、状态同步）

> 来源: chaos-engine-spec-v0.1.md | 状态: 已实现

## Requirements

Network 模块是 ChaosEngine 的网络通信层，提供 Socket 抽象、属性复制系统、帧同步（Lockstep）和状态同步（State Sync）两种网络模式。Network 模块依赖 Core 和 ECS 模块，是实现客户端/服务器同构架构的关键模块。

---

### Requirement: Socket 传输层抽象

Network 模块 SHALL 提供统一的 Socket 抽象层（`ce_socket`），封装底层传输协议。

首版 MUST 支持以下传输协议：
- **TCP**：用于登录、聊天、物品交易等可靠性优先的场景
- **UDP**：用于战斗、移动等低延迟场景

后续版本 SHALL 集成 KCP（可靠 UDP）和 WebSocket 支持。

Socket 抽象层 MUST 提供统一的发送/接收接口，上层代码无需关心底层协议类型。

#### Scenario: TCP 可靠传输

- **WHEN** 客户端通过 TCP Socket 发送一条登录请求消息
- **THEN** 服务端完整收到该消息，无丢包、无乱序
- **AND** 连接断开时双方收到断开通知

#### Scenario: UDP 低延迟传输

- **WHEN** 客户端通过 UDP Socket 高频发送移动输入（每帧一次）
- **THEN** 消息延迟显著低于 TCP 模式
- **AND** 允许少量丢包，不影响后续消息接收

---

### Requirement: 属性复制系统

Network 模块 SHALL 实现属性复制系统（借鉴 UE Replication），支持将服务端组件属性自动同步到客户端。

属性复制 MUST 支持以下功能：
- 标记需要复制的组件属性（通过 `CeReplicatedProp` 描述偏移和大小）
- 注册复制组件（`ce_replication_register_component`）
- 每帧自动比较脏数据并复制（`ce_replication_tick`）
- 支持复制条件（`CeReplicateCondition`），控制复制频率和触发条件

#### Scenario: 属性自动复制

- **WHEN** 服务端修改实体的 Health 组件（已注册为复制组件）
- **THEN** 下一帧 `ce_replication_tick` 自动检测到脏数据
- **AND** 变更的属性值通过网络发送给相关客户端
- **AND** 客户端收到后自动更新对应实体的 Health 组件

#### Scenario: 复制条件过滤

- **WHEN** 配置 Health 组件的复制条件为 "仅当 HP 变化时复制"
- **THEN** 连续多帧 HP 未变化时不产生复制流量
- **AND** HP 变化时立即触发复制

---

### Requirement: 帧同步（Lockstep）

Network 模块 SHALL 实现帧同步协议，适用于 MOBA、RTS、格斗等低延迟少人场景（<10 人）。

帧同步流程 MUST 为：
1. 服务器收集所有客户端输入
2. 服务器广播 Frame N 的输入包给所有客户端
3. 客户端收到后执行 Frame N 的逻辑
4. 客户端缓存 N 帧状态，用于回滚/追帧

帧同步 MUST 保证确定性：
- 浮点数统一使用 IEEE 754，禁用快速数学优化
- 随机数使用确定性种子 PRNG
- 实体遍历顺序固定（按 ID 排序）

#### Scenario: 帧同步确定性

- **WHEN** 两个客户端在相同初始状态下收到相同的输入序列
- **THEN** 两个客户端执行 N 帧后的游戏状态完全一致
- **AND** 可通过 checksum 比对验证状态一致性

#### Scenario: 帧缓存与追帧

- **WHEN** 客户端因网络波动落后服务器 5 帧
- **THEN** 客户端利用缓存的帧数据快速追帧
- **AND** 追帧完成后游戏状态与服务器一致

---

### Requirement: 状态同步（State Sync）

Network 模块 SHALL 实现状态同步协议，适用于 MMO、开放世界等大规模多人场景（>100 人）。

状态同步 MUST 满足以下特性：
- 服务器为权威端，执行游戏逻辑并验证客户端行为
- 客户端通过插值/预测平滑表现服务器状态
- 结合属性复制系统自动同步实体状态
- 支持网络优先级和相关性，减少不必要的数据同步

#### Scenario: 服务器权威验证

- **WHEN** 客户端上报一个异常的移动位置（如瞬移）
- **THEN** 服务器检测到异常并拒绝该位置更新
- **AND** 服务器将正确位置强制同步给该客户端

#### Scenario: 客户端插值

- **WHEN** 客户端收到服务端的位置更新（非每帧发送）
- **THEN** 客户端在两个已知位置之间进行平滑插值
- **AND** 实体移动表现流畅，无明显跳变

---

### Requirement: 客户端/服务器同构网络模式切换

Network 模块 SHALL 支持通过编译宏在帧同步和状态同步之间切换。

客户端模式（`CE_BUILD_CLIENT`）MUST 默认使用帧同步（`CE_NET_LOCKSTEP`），服务器模式（`CE_BUILD_SERVER`）MUST 默认使用状态同步（`CE_NET_STATESYNC`）。

Lua 脚本层 MUST 可通过全局变量（如 `CHAOS_IS_SERVER`、`NET_MODE`）判断当前运行环境，实现同构逻辑分支。

#### Scenario: 编译模式决定网络模式

- **WHEN** 以 Client 模式编译
- **THEN** 网络模块以帧同步模式初始化
- **AND** Lua 全局变量 `CHAOS_IS_SERVER` 为 false

#### Scenario: 同构逻辑分支

- **WHEN** shared/combat.lua 中的伤害计算函数检测到 `CHAOS_IS_SERVER == true`
- **THEN** 服务端直接应用伤害并广播结果
- **AND** 客户端仅播放表现效果，等待服务端确认
