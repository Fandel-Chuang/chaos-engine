# ECS — ECS 系统（Archetype 存储、实体生命周期、组件、系统调度、查询）

> 来源: chaos-engine-spec-v0.1.md | 状态: 已实现

## Requirements

ECS 模块是 ChaosEngine 的实体组件系统，采用 Archetype 存储模式，提供实体生命周期管理、组件注册与存储、系统调度管线和实体查询功能。ECS 模块位于引擎内核中层，依赖 Core 模块，被 Render、Network、Plugin、Script 等上层模块依赖。

---

### Requirement: Archetype 存储模型

ECS 系统 SHALL 采用 Archetype（原型）存储模式，将具有相同组件组合的实体归入同一 Archetype。

相同 Archetype 的实体，其组件数据 MUST 连续存储在内存中，以实现：
- 遍历时 Cache-Friendly 数据访问
- SIMD 友好的内存布局
- 高效的批量操作

当实体添加或移除组件时，实体 MUST 在 Archetype 之间迁移，迁移操作应保证数据完整性。

#### Scenario: 相同 Archetype 实体连续存储

- **WHEN** 创建 100 个拥有 Transform + MeshRenderer 组件的实体
- **THEN** 这些实体的 Transform 数据存储在连续内存块中
- **AND** 遍历这些实体时内存访问模式为顺序访问

#### Scenario: 组件变更触发 Archetype 迁移

- **WHEN** 对 Archetype<Transform> 的实体添加 RigidBody 组件
- **THEN** 该实体从原 Archetype 迁移到 Archetype<Transform, RigidBody>
- **AND** 现有组件数据完整保留，新组件初始化为默认值

---

### Requirement: 实体生命周期管理

ECS 系统 SHALL 提供完整的实体生命周期管理，包括：
- 实体创建（`ce_entity_create`）
- 实体销毁（`ce_entity_destroy`）
- 实体 ID 分配与回收

实体 ID MUST 在全局范围内唯一。销毁的实体 ID 可被回收复用，但需保证同一时刻不存在重复 ID。

#### Scenario: 实体创建与销毁

- **WHEN** 创建实体 A，记录其 ID，然后销毁实体 A，再创建实体 B
- **THEN** 实体 A 和实体 B 拥有不同的 ID
- **AND** 实体 A 销毁后其所有组件数据被正确释放

#### Scenario: 实体 ID 唯一性

- **WHEN** 连续创建 10000 个实体
- **THEN** 所有实体的 ID 互不相同
- **AND** 通过 ID 可唯一索引到对应实体

---

### Requirement: 组件注册与存储

ECS 系统 SHALL 支持组件的注册、添加、获取和移除操作。

每个组件类型 MUST 在系统启动时注册，注册信息包括：
- 组件唯一标识符（Component ID）
- 组件数据大小
- 可选的构造函数/析构函数

内置组件（首版）MUST 包括：Transform、Velocity、Health、MeshRenderer、Camera、Light、NetworkId、Script。

#### Scenario: 组件注册

- **WHEN** 注册自定义组件类型 "Mana"（包含 current 和 max 字段）
- **THEN** 系统为该组件分配唯一的 Component ID
- **AND** 后续可通过该 ID 添加/获取/移除 Mana 组件

#### Scenario: 组件操作

- **WHEN** 对实体执行 `ce_entity_add_component(e, COMPONENT_TRANSFORM)`
- **THEN** 实体获得 Transform 组件，可通过 `ce_entity_get_component` 获取并修改
- **AND** 组件数据初始化为零值

---

### Requirement: 系统调度管线

ECS 系统 SHALL 提供基于 Phase 的系统调度管线，按以下顺序执行：

```
CE_PHASE_PRE_UPDATE    (输入采集)
    ↓
CE_PHASE_UPDATE        (逻辑更新)
    ↓
CE_PHASE_POST_UPDATE   (状态同步)
    ↓
CE_PHASE_RENDER        (渲染提交)
    ↓
CE_PHASE_POST_RENDER   (UI/调试)
```

同一 Phase 内的系统 MUST 按注册时的优先级（priority）排序执行。系统注册接口为 `ce_system_register(name, fn, phase, priority)`。

#### Scenario: Phase 顺序执行

- **WHEN** 注册系统 A（PHASE_PRE_UPDATE, priority=0）和系统 B（PHASE_UPDATE, priority=0）
- **THEN** 每帧系统 A 在系统 B 之前执行
- **AND** 系统 A 的输出可作为系统 B 的输入

#### Scenario: 同 Phase 优先级排序

- **WHEN** 注册系统 A（PHASE_UPDATE, priority=10）和系统 B（PHASE_UPDATE, priority=0）
- **THEN** 系统 B（低 priority 值）在系统 A 之前执行

---

### Requirement: 实体查询与迭代

ECS 系统 SHALL 提供高效的实体查询机制，支持按组件组合筛选实体。

查询接口 MUST 支持：
- 查询拥有指定组件组合的所有实体
- 在查询结果上迭代访问组件数据
- 查询结果缓存以提高重复查询性能

查询宏（`CE_QUERY_BEGIN` / `CE_QUERY_END`）MUST 提供类型安全的组件访问。

#### Scenario: 组件组合查询

- **WHEN** 查询所有拥有 Transform + Velocity 组件的实体
- **THEN** 仅返回同时拥有这两个组件的实体
- **AND** 不包含仅有 Transform 或仅有 Velocity 的实体

#### Scenario: 查询迭代

- **WHEN** 在 CE_QUERY_BEGIN(Transform, Velocity) 块内修改 transform->x
- **THEN** 所有匹配实体的 Transform 组件被正确修改
- **AND** 迭代过程中不会遗漏或重复访问实体
