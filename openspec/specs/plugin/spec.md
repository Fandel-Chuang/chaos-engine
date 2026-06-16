# Plugin — 插件系统（状态机、描述符、注册表）

> 来源: chaos-engine-spec-v0.1.md | 状态: 已实现

## Requirements

Plugin 模块是 ChaosEngine 的插件系统，提供插件状态机、插件描述符和插件注册表，支持引擎功能的模块化组织和生命周期管理。Plugin 模块依赖 Core 模块。

---

### Requirement: 插件状态机

插件系统 SHALL 实现完整的插件状态机，定义插件从加载到卸载的全部生命周期状态。

状态转换 MUST 遵循以下状态机：

```
UNLOADED  →  LOADING  →  LOADED  →  RUNNING
                ↓           ↓           ↓
              ERROR      UNLOADING   PAUSED
                             ↓           ↓
                          UNLOADED   RUNNING
```

状态转换规则：
- `UNLOADED`：初始状态，调用 `load()` 进入 `LOADING`
- `LOADING`：加载中，加载完成进入 `LOADED`，失败进入 `ERROR`
- `LOADED`：已加载，调用 `init()` 进入 `RUNNING`，调用 `unload()` 进入 `UNLOADING`
- `RUNNING`：运行中，调用 `pause()` 进入 `PAUSED`，发生错误进入 `ERROR`
- `PAUSED`：已暂停，调用 `resume()` 回到 `RUNNING`
- `ERROR`：错误状态，调用 `unload()` 进入 `UNLOADING`
- `UNLOADING`：卸载中，完成后回到 `UNLOADED`

#### Scenario: 插件正常生命周期

- **WHEN** 加载插件 "render"，依次经历 LOADING → LOADED → RUNNING
- **THEN** 每个状态转换通过日志系统记录（含插件名、状态转换和耗时）
- **AND** 插件在 RUNNING 状态下正常执行每帧更新

#### Scenario: 插件加载失败

- **WHEN** 插件 "render" 在 LOADING 阶段发生错误
- **THEN** 插件状态转换为 ERROR
- **AND** 错误信息通过日志系统输出
- **AND** 不影响其他已加载插件的正常运行

#### Scenario: 插件暂停与恢复

- **WHEN** 对 RUNNING 状态的插件调用 `pause()`
- **THEN** 插件进入 PAUSED 状态，不再接收帧更新回调
- **AND** 调用 `resume()` 后插件恢复 RUNNING 状态，继续接收帧更新

---

### Requirement: 插件描述符

每个插件 MUST 提供 `CePluginDesc` 描述符结构，包含以下字段：
- `name`：插件名称（唯一标识）
- `version`：插件版本号
- `init`：初始化回调函数
- `update`：帧更新回调函数
- `shutdown`：关闭回调函数
- `is_core`：是否为核心插件（核心插件不可卸载）

插件描述符 MUST 在插件注册时提供，系统据此管理插件生命周期。

#### Scenario: 核心插件保护

- **WHEN** 尝试卸载标记为 `is_core = true` 的插件
- **THEN** 卸载操作被拒绝
- **AND** 日志输出警告信息

#### Scenario: 插件版本标识

- **WHEN** 查询已注册插件列表
- **THEN** 每个插件返回其名称和版本号
- **AND** 版本号格式符合语义化版本规范

---

### Requirement: 插件注册表

插件系统 SHALL 维护全局插件注册表（`ce_plugin_registry`），管理所有已注册的插件。

注册表 MUST 支持以下操作：
- 注册插件（`ce_plugin_register`）
- 注销插件（`ce_plugin_unregister`）
- 按名称查找插件
- 遍历所有已注册插件
- 按依赖顺序加载插件

插件名称 MUST 在注册表中唯一，重复注册同名插件应返回错误。

#### Scenario: 插件注册与查找

- **WHEN** 注册插件 "render" 后按名称查找
- **THEN** 返回该插件的描述符和当前状态
- **AND** 查找不存在的插件名返回空

#### Scenario: 重复注册检测

- **WHEN** 尝试注册两个同名为 "render" 的插件
- **THEN** 第二次注册返回错误
- **AND** 第一个插件不受影响

---

### Requirement: 插件依赖管理

插件系统 SHALL 支持插件间依赖声明和按依赖顺序加载。

当加载插件 A 时，如果 A 依赖插件 B，系统 MUST 先确保 B 已加载并处于 RUNNING 状态，再加载 A。如果依赖的插件加载失败，依赖方 MUST 也标记为加载失败。

#### Scenario: 依赖顺序加载

- **WHEN** 插件 "game" 依赖插件 "render"，按任意顺序注册两个插件
- **THEN** 加载时系统自动先加载 "render"，待其进入 RUNNING 后再加载 "game"
- **AND** 加载顺序通过日志记录

#### Scenario: 依赖失败传播

- **WHEN** 插件 "game" 依赖插件 "render"，但 "render" 加载失败
- **THEN** 插件 "game" 也标记为加载失败
- **AND** 错误日志明确指示失败原因（依赖插件加载失败）
