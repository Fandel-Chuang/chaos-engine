# Script — Lua 脚本层（双 VM、同构加载、最小绑定）

> 来源: chaos-engine-spec-v0.1.md | 状态: 已实现

## Requirements

Script 模块是 ChaosEngine 的 Lua 脚本运行时，提供 Lua VM 嵌入、C API 绑定和客户端/服务端同构脚本加载。Script 模块依赖 Core 和 ECS 模块，是实现游戏逻辑脚本化的核心模块。

---

### Requirement: 双 VM 隔离

Script 模块 SHALL 提供两个完全独立的 Lua VM 实例：
- **运行时 Lua VM**：执行游戏逻辑（战斗、技能、AI 等）
- **编辑器 Lua VM**：执行编辑器扩展脚本

两个 VM MUST 完全隔离，互不干扰：
- 各自拥有独立的全局状态
- 各自拥有独立的 GC（垃圾回收）
- 一个 VM 的崩溃不影响另一个 VM

#### Scenario: 运行时 VM 与编辑器 VM 隔离

- **WHEN** 运行时 VM 中执行游戏逻辑脚本
- **THEN** 运行时 VM 中的全局变量和函数对编辑器 VM 不可见
- **AND** 编辑器 VM 中的脚本执行不影响运行时 VM 的状态

#### Scenario: VM 崩溃隔离

- **WHEN** 编辑器 VM 中的扩展脚本触发 Lua 错误
- **THEN** 仅编辑器 VM 受影响，运行时 VM 继续正常运行
- **AND** 游戏逻辑不受编辑器脚本错误影响

---

### Requirement: 同构脚本加载

Script 模块 SHALL 支持客户端和服务端加载同一份 Lua 脚本文件。

`shared/` 目录下的 Lua 文件 MUST 被客户端和服务端共同加载，实现"同一份战斗逻辑代码，编译一次，两处运行"。

脚本 MUST 可通过引擎注入的全局变量（如 `CHAOS_IS_SERVER`、`NET_MODE`）判断当前运行环境，执行环境特定的分支逻辑。

#### Scenario: 同构战斗逻辑加载

- **WHEN** 客户端和服务端分别启动并加载 shared/combat.lua
- **THEN** 双方加载的是同一份脚本文件
- **AND** 脚本中的核心计算逻辑（如伤害公式）在两端执行结果一致

#### Scenario: 环境感知分支

- **WHEN** shared/combat.lua 中的函数检测 `CHAOS_IS_SERVER` 全局变量
- **THEN** 服务端执行权威逻辑分支（应用伤害、广播结果）
- **AND** 客户端执行表现逻辑分支（播放特效、等待确认）

---

### Requirement: 最小 C API 绑定

Script 模块 SHALL 仅暴露必要的 C API 给 Lua，遵循最小绑定原则。

首版 MUST 绑定的 API 包括：
- `entity_create`：创建实体
- `entity_add_comp`：添加组件
- `entity_get_comp`：获取组件
- `log_info` / `log_error`：日志输出
- `network_send`：网络消息发送

绑定 MUST 通过 `ce_lua_register_function` 统一注册，避免分散的绑定逻辑。

#### Scenario: Lua 创建实体

- **WHEN** Lua 脚本调用 `entity_create()` 函数
- **THEN** 引擎创建一个新实体并返回实体 ID 给 Lua
- **AND** Lua 可继续调用 `entity_add_comp` 为该实体添加组件

#### Scenario: 绑定注册

- **WHEN** 引擎初始化 Lua VM
- **THEN** 所有公开的 C API 通过 `ce_lua_register_function` 注册到 Lua 全局表
- **AND** 未注册的 C 函数对 Lua 不可见

---

### Requirement: Lua VM 生命周期管理

Script 模块 SHALL 提供完整的 Lua VM 生命周期管理，包括：
- VM 创建与初始化
- 脚本文件加载与执行
- 错误处理与栈追踪
- VM 销毁与资源释放

VM 初始化时 MUST 注入引擎全局变量（`CHAOS_IS_SERVER`、`NET_MODE` 等），确保脚本在正确的环境中运行。

#### Scenario: 脚本加载错误处理

- **WHEN** 加载包含语法错误的 Lua 脚本
- **THEN** 引擎捕获错误并通过日志系统输出详细错误信息（含文件名和行号）
- **AND** 引擎继续运行，不因脚本错误而崩溃

#### Scenario: VM 资源释放

- **WHEN** 引擎关闭并销毁 Lua VM
- **THEN** VM 占用的所有内存被正确释放
- **AND** 所有 Lua 对象的析构逻辑被正确执行
