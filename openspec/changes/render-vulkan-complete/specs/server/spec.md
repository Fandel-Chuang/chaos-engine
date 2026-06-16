# Server — AOI 十字链表 + Cell 大地图管理（v0.2 Delta）

> 来源: chaos-engine-spec-v0.2.md | 状态: 草案 | 变更类型: ADDED

## 概述

v0.1 没有服务端空间管理模块。v0.2 新增 `src_c/server/` 目录，实现 AOI 十字链表和 Cell 大地图管理两个子系统。AOI 负责实体视野管理（进入/离开/移动事件），Cell 负责大地图网格划分和动态负载均衡（分裂/合并）。两个子系统协同工作：每个 Cell 内嵌独立的 AOI 实例，Cell 管理器处理跨 Cell 的实体迁移和边界查询。

---

## ADDED Requirements

### Requirement: 服务端类型定义

`ce_server_types.h` SHALL 定义服务端模块共享的基础类型，包括实体 ID、Cell ID、AOI 节点、AOI 事件、Cell 结构、Cell 管理器等。

类型定义 MUST 满足：
- `CeServerEntityId` 为 `uint32_t`，与 ECS 的 `CeEntity` 对应
- `CeCellId` 为 `uint32_t`，支持最多 42 亿个 Cell
- `CeAoiNode` 包含 X/Y 轴双向链表指针、位置、所属 Cell、实体半径
- `CeAoiEvent` 包含事件类型（ENTER/LEAVE/MOVE）、主体实体、客体实体、位置
- `CeCell` 包含边界、状态、实体计数、分裂/合并阈值、进程 ID、邻居列表、内嵌 AOI 句柄
- `CeCellManager` 包含 Cell 数组、世界边界、默认 Cell 尺寸、自增 ID

#### Scenario: 类型在服务端模块间共享

- **WHEN** `ce_aoi.c` 和 `ce_cell.c` 都 `#include "server/ce_server_types.h"`
- **THEN** 两个模块使用相同的 `CeAoiNode`、`CeCell` 等类型定义
- **AND** 编译无类型冲突

#### Scenario: 实体 ID 与 ECS 对应

- **WHEN** 服务端从 ECS 获取实体 ID
- **THEN** `CeServerEntityId` 可以直接赋值为 ECS 的 `CeEntity`
- **AND** 无需类型转换（两者均为 `uint32_t`）

---

### Requirement: AOI 十字链表初始化与关闭

`ce_aoi_init` SHALL 初始化 AOI 系统，设置 AOI 视野半径和事件回调函数。

`ce_aoi_shutdown` SHALL 释放所有 AOI 节点，清理链表结构。

AOI 系统 MUST 为单例模式（全局唯一实例），所有操作在服务端主循环中执行，无需锁。

#### Scenario: 初始化 AOI 系统

- **WHEN** 调用 `ce_aoi_init(50.0f, my_callback, NULL)`
- **THEN** AOI 系统初始化成功，视野半径设置为 50.0 世界单位
- **AND** 事件回调设置为 `my_callback`
- **AND** 内部链表头节点初始化完成

#### Scenario: 关闭 AOI 系统

- **WHEN** 在已有 100 个实体的 AOI 系统上调用 `ce_aoi_shutdown()`
- **THEN** 所有 100 个节点被释放
- **AND** 无内存泄漏
- **AND** 再次调用 `ce_aoi_init` 可以重新初始化

---

### Requirement: 实体进入 AOI

`ce_aoi_enter` SHALL 将实体添加到 AOI 十字链表中，按 X 坐标升序插入 X 轴链表，按 Y 坐标升序插入 Y 轴链表。

插入完成后 MUST 遍历周围实体（AOI 半径内），对每个周围实体触发 `CE_AOI_ENTER` 事件（双向：新实体看到周围实体，周围实体看到新实体）。

#### Scenario: 插入第一个实体

- **WHEN** 空 AOI 系统中调用 `ce_aoi_enter(1, 100.0f, 200.0f, 1.0f)`
- **THEN** 实体 1 成为 X 轴和 Y 轴链表的唯一节点
- **AND** 无 AOI 事件触发（周围无其他实体）
- **AND** 返回 `CE_SUCCESS`

#### Scenario: 插入第二个实体在视野内

- **WHEN** AOI 中已有实体 1 在 (100, 200)，调用 `ce_aoi_enter(2, 120.0f, 210.0f, 1.0f)` 且 AOI 半径为 50
- **THEN** 实体 2 插入 X 轴链表（在实体 1 右侧）和 Y 轴链表（在实体 1 上方）
- **AND** 触发 CE_AOI_ENTER 事件：subject=2, object=1
- **AND** 触发 CE_AOI_ENTER 事件：subject=1, object=2

#### Scenario: 插入实体在视野外

- **WHEN** AOI 中已有实体 1 在 (100, 200)，调用 `ce_aoi_enter(2, 500.0f, 500.0f, 1.0f)` 且 AOI 半径为 50
- **THEN** 实体 2 正确插入链表
- **AND** 无 AOI 事件触发（距离超出 AOI 半径）

---

### Requirement: 实体离开 AOI

`ce_aoi_leave` SHALL 从 X 轴和 Y 轴链表中移除实体节点，并释放节点内存。

移除前 MUST 遍历周围实体，对每个周围实体触发 `CE_AOI_LEAVE` 事件（双向）。

#### Scenario: 移除中间节点

- **WHEN** X 轴链表为 A ↔ B ↔ C，调用 `ce_aoi_leave(B)`
- **THEN** X 轴链表变为 A ↔ C
- **AND** Y 轴链表中 B 也被移除
- **AND** 触发 CE_AOI_LEAVE 事件通知 A 和 C

#### Scenario: 移除不存在的实体

- **WHEN** 调用 `ce_aoi_leave(999)` 但实体 999 不在 AOI 中
- **THEN** 函数静默返回，无崩溃
- **AND** 链表结构不受影响

---

### Requirement: 实体移动

`ce_aoi_move` SHALL 更新实体在十字链表中的位置，并自动检测视野变化事件。

移动算法 MUST：
1. 记录移动前的周围实体集合 S_old
2. 从 X/Y 轴链表中移除节点
3. 更新坐标
4. 重新插入 X/Y 轴链表
5. 查询移动后的周围实体集合 S_new
6. S_new - S_old → 触发 CE_AOI_ENTER
7. S_old - S_new → 触发 CE_AOI_LEAVE
8. S_old ∩ S_new → 触发 CE_AOI_MOVE

#### Scenario: 实体在视野内移动

- **WHEN** 实体 A 和 B 互相在视野内，A 从 (100, 200) 移动到 (110, 205)
- **THEN** A 在链表中的位置更新
- **AND** 触发 CE_AOI_MOVE 事件通知 B
- **AND** 无 CE_AOI_ENTER 或 CE_AOI_LEAVE 事件（两者仍在视野内）

#### Scenario: 实体移出视野

- **WHEN** 实体 A 和 B 互相在视野内，A 移动到远离 B 超出 AOI 半径的位置
- **THEN** 触发 CE_AOI_LEAVE 事件（A 离开 B 的视野，B 离开 A 的视野）
- **AND** 无 CE_AOI_MOVE 事件

#### Scenario: 实体移入视野

- **WHEN** 实体 A 和 B 不在彼此视野内，A 移动到 B 的 AOI 半径内
- **THEN** 触发 CE_AOI_ENTER 事件（A 进入 B 的视野，B 进入 A 的视野）

---

### Requirement: 周围实体查询

`ce_aoi_query_nearby` SHALL 返回指定实体 AOI 半径内的所有实体 ID。

查询算法 MUST 沿 X 轴向左/向右遍历，沿 Y 轴向上/向下遍历，距离超过 AOI 半径时停止。结果 MUST 去重（同一实体可能在 X 和 Y 遍历中都出现）。

`ce_aoi_count_nearby` SHALL 返回周围实体数量（不填充 buffer，仅计数）。

#### Scenario: 查询周围实体

- **WHEN** AOI 中有实体 A(100,100), B(120,100), C(200,200)，AOI 半径 50，查询 A 的周围实体
- **THEN** 返回 {B}（C 距离 > 50）
- **AND** 结果数量为 1

#### Scenario: 查询结果不包含自身

- **WHEN** 查询实体 A 的周围实体
- **THEN** 结果中不包含 A 自身

#### Scenario: buffer 容量不足

- **WHEN** 周围有 10 个实体但 buffer 容量 max_count=5
- **THEN** 返回 5（实际写入数量）
- **AND** 函数返回值表示实际周围实体总数（10）

---

### Requirement: Cell 管理器初始化

`ce_cell_init` SHALL 根据世界尺寸和 Cell 尺寸创建 Cell 网格，初始化所有 Cell 的边界、邻居关系和内嵌 AOI。

Cell 网格 MUST 覆盖整个世界范围（world_min_x/y 到 world_max_x/y）。每个 Cell MUST 初始化独立的 AOI 实例。

`ce_cell_shutdown` SHALL 释放所有 Cell 及其内嵌 AOI。

#### Scenario: 创建 4x4 Cell 网格

- **WHEN** 调用 `ce_cell_init(400, 400, 100, 100, 150, 20)`
- **THEN** 创建 16 个 Cell（4 行 × 4 列）
- **AND** 每个 Cell 的边界正确（如 Cell(0,0) 覆盖 [0,0] 到 [100,100]）
- **AND** 每个 Cell 的内嵌 AOI 已初始化

#### Scenario: 邻居关系正确

- **WHEN** 创建 3x3 Cell 网格
- **THEN** 中心 Cell(1,1) 有 8 个邻居
- **AND** 角落 Cell(0,0) 有 3 个邻居
- **AND** 边缘 Cell(1,0) 有 5 个邻居

---

### Requirement: 坐标到 Cell 映射

`ce_cell_find_by_position` SHALL 根据世界坐标返回对应的 Cell ID。

映射公式 MUST 为：`cell_x = floor((x - world_min_x) / cell_width)`，`cell_y = floor((y - world_min_y) / cell_height)`。

#### Scenario: 坐标映射到正确 Cell

- **WHEN** 世界范围 [0,400]×[0,400]，Cell 尺寸 100×100，查询坐标 (150, 250)
- **THEN** 返回 Cell(1, 2) 的 ID
- **AND** `ce_cell_get(id)->bounds` 覆盖 [100,200]×[200,300]

#### Scenario: 边界坐标

- **WHEN** 查询坐标恰好位于 Cell 边界 (100, 100)
- **THEN** 返回 Cell(1, 1) 的 ID（floor 语义，边界属于右侧/上方 Cell）

---

### Requirement: 实体进入/离开世界

`ce_cell_enter_entity` SHALL 根据实体坐标自动分配到对应 Cell，并在该 Cell 的 AOI 中注册实体。

`ce_cell_leave_entity` SHALL 从对应 Cell 的 AOI 中移除实体。

#### Scenario: 实体进入世界

- **WHEN** 调用 `ce_cell_enter_entity(1, 150.0f, 250.0f, 1.0f)`
- **THEN** 实体 1 被分配到坐标 (150, 250) 对应的 Cell
- **AND** 该 Cell 的 entity_count 增加 1
- **AND** 实体在该 Cell 的 AOI 中注册

#### Scenario: 实体离开世界

- **WHEN** 调用 `ce_cell_leave_entity(1)`
- **THEN** 实体 1 从其所属 Cell 的 AOI 中移除
- **AND** 该 Cell 的 entity_count 减少 1

---

### Requirement: 跨 Cell 实体迁移

`ce_cell_move_entity` SHALL 处理实体移动，当实体跨越 Cell 边界时自动迁移到新 Cell。

迁移流程 MUST：
1. 计算新坐标对应的 Cell ID
2. 如果 Cell ID 不变：仅调用 `ce_aoi_move` 更新位置
3. 如果 Cell ID 改变：从旧 Cell AOI 移除 → 添加到新 Cell AOI → 更新实体的 cell_id

#### Scenario: Cell 内移动

- **WHEN** 实体在 Cell(1,1) 内从 (150,150) 移动到 (180,180)
- **THEN** 实体仍属于 Cell(1,1)
- **AND** 仅调用 AOI 移动更新链表位置
- **AND** 无跨 Cell 迁移开销

#### Scenario: 跨 Cell 移动

- **WHEN** 实体在 Cell(1,1) 内从 (150,150) 移动到 (250,150)（进入 Cell(2,1)）
- **THEN** 实体从 Cell(1,1) 的 AOI 中移除
- **AND** 实体添加到 Cell(2,1) 的 AOI 中
- **AND** 实体的 cell_id 更新为 Cell(2,1) 的 ID
- **AND** Cell(1,1) 的 entity_count 减 1，Cell(2,1) 的 entity_count 加 1

---

### Requirement: Cell 动态分裂

`ce_cell_split` SHALL 将实体数超过 `max_entities` 阈值的 Cell 分裂为 4 个等大的子 Cell。

分裂流程 MUST：
1. 将原 Cell 边界四等分
2. 创建 4 个新 Cell（或复用槽位）
3. 遍历原 Cell 所有实体，按坐标重新分配到子 Cell
4. 更新邻居关系（子 Cell 之间 + 与外部 Cell）
5. 标记原 Cell 为 CE_CELL_MERGING 状态
6. 触发 CE_AOI_LEAVE + CE_AOI_ENTER 事件

`ce_cell_update` SHALL 每帧检查所有 Cell，对超过阈值的 Cell 自动触发分裂。

#### Scenario: 自动触发分裂

- **WHEN** Cell A 的 entity_count 达到 151（阈值 150），调用 `ce_cell_update()`
- **THEN** Cell A 被分裂为 4 个子 Cell
- **AND** 原 Cell A 标记为 CE_CELL_MERGING
- **AND** 所有实体按坐标重新分配到子 Cell
- **AND** 子 Cell 的邻居关系正确

#### Scenario: 未达阈值不分裂

- **WHEN** Cell A 的 entity_count 为 149（阈值 150），调用 `ce_cell_update()`
- **THEN** Cell A 不被分裂
- **AND** Cell A 状态保持 CE_CELL_ACTIVE

---

### Requirement: Cell 合并

`ce_cell_merge` SHALL 将两个相邻且总实体数低于 `min_entities` 阈值的 Cell 合并为一个 Cell。

合并流程 MUST：
1. 验证两个 Cell 相邻且总实体数 < min_entities
2. 创建合并后的 Cell（边界 = cell_a ∪ cell_b）
3. 将所有实体迁移到新 Cell
4. 更新邻居关系
5. 释放旧 Cell

#### Scenario: 自动触发合并

- **WHEN** Cell A（10 实体）和 Cell B（8 实体）相邻，总实体数 18 < 阈值 20，调用 `ce_cell_update()`
- **THEN** Cell A 和 Cell B 合并为一个新 Cell
- **AND** 新 Cell 包含 18 个实体
- **AND** 旧 Cell A 和 B 被释放

#### Scenario: 超过阈值不合并

- **WHEN** Cell A（15 实体）和 Cell B（10 实体）相邻，总实体数 25 > 阈值 20
- **THEN** 两个 Cell 保持独立
- **AND** 无合并操作

---

### Requirement: 跨 Cell 边界查询

`ce_cell_query_nearby` SHALL 查询实体周围的实体，自动处理跨 Cell 边界的情况。

查询流程 MUST：
1. 在本 Cell 内使用 AOI 查询
2. 如果实体距离 Cell 边界 < AOI 半径，查询邻居 Cell 中边界附近的实体
3. 合并结果并去重

#### Scenario: 实体在 Cell 中心

- **WHEN** 实体位于 Cell 中心（距离所有边界 > AOI 半径），查询周围实体
- **THEN** 仅查询本 Cell 的 AOI
- **AND** 不查询邻居 Cell

#### Scenario: 实体靠近 Cell 边界

- **WHEN** 实体距离 Cell 右边界 10 单位（AOI 半径 50），查询周围实体
- **THEN** 查询本 Cell 的 AOI
- **AND** 同时查询右侧邻居 Cell 的 AOI
- **AND** 结果包含两个 Cell 中 AOI 半径内的实体

---

### Requirement: 编译条件隔离

Server 模块 SHALL 仅在 `CE_BUILD_SERVER` 编译选项开启时编译和链接。

`chaos_server` 目标 MUST 链接 `engine_server` 库（包含 AOI 和 Cell 实现）。

Client 和 Headless 编译目标 MUST NOT 链接 `engine_server`。

#### Scenario: Server 模式编译

- **WHEN** 执行 `cmake -DCE_BUILD_SERVER=ON .. && make chaos_server`
- **THEN** `src_c/server/` 下的源文件被编译
- **AND** `chaos_server` 链接 `engine_server`
- **AND** 编译通过

#### Scenario: Client 模式不编译 Server

- **WHEN** 执行 `cmake -DCE_BUILD_SERVER=OFF .. && make chaos_client`
- **THEN** `src_c/server/` 下的源文件不被编译
- **AND** `chaos_client` 不链接 `engine_server`
- **AND** 编译通过

---

### Requirement: 单元测试覆盖

AOI 和 Cell 模块 MUST 有对应的单元测试文件（`test_aoi.c` 和 `test_cell.c`）。

AOI 测试 MUST 覆盖：插入、删除、移动、查询、边界条件、1000 实体压力测试。

Cell 测试 MUST 覆盖：Cell 查找、分裂、合并、跨 Cell 迁移、跨 Cell 查询。

#### Scenario: AOI 压力测试

- **WHEN** 运行 AOI 单元测试，1000 个实体随机移动 10000 次
- **THEN** 无内存泄漏
- **AND** 无链表断裂（所有实体可通过链表遍历找到）
- **AND** 查询结果与暴力遍历结果一致

#### Scenario: Cell 分裂合并测试

- **WHEN** 运行 Cell 单元测试，创建 Cell 网格并触发多次分裂和合并
- **THEN** 分裂后实体正确分配到子 Cell
- **AND** 合并后实体正确集中到新 Cell
- **AND** 邻居关系始终正确
