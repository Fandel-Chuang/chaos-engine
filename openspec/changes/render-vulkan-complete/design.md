# Design: Render Vulkan 完整化 + Server AOI/Cell

## Context

ChaosEngine v0.1 完成了引擎基础骨架：Core 数学/内存、ECS Archetype、Network 帧同步、Render RHI 接口 + OpenGL 后端、Plugin 框架。v0.2 聚焦于两个核心短板：

1. **Render Vulkan 后端不完整**：`ce_rhi_vulkan.h` 仅 11 行，内部结构体全部隐藏在 `.c` 中，纹理/着色器/管线接口全部是 stub。
2. **服务端空间管理缺失**：`src_c/server/` 目录不存在，MMO 必需的 AOI 和 Cell 子系统未实现。

## Goals / Non-Goals

**Goals:**

1. **Render**：将 `ce_rhi_vulkan.h` 补全到 ~200 行，暴露内部结构体；实现纹理/着色器/管线/Uniform 的 stub 函数。
2. **Server AOI**：实现十字链表 AOI，支持 enter/leave/move 事件和周围实体查询。
3. **Server Cell**：实现 Cell 大地图管理，支持网格划分、动态分裂合并、跨 Cell 迁移和查询。
4. **编译隔离**：Server 模块仅在 `CE_BUILD_SERVER` 下编译，不影响 Client/Headless。
5. **单元测试**：AOI 和 Cell 有完整的单元测试覆盖。

**Non-Goals:**

- Vulkan Descriptor Set 管理（延后到 v0.3）
- 多进程 Cell 分配（当前仅单进程，进程 ID 字段预留）
- Editor 3D 视口（延后到 v0.3）
- Lua 战斗逻辑层（延后到 v0.3）
- 跨进程 Cell 迁移协议

## Decisions

### Decision 1: 头文件结构 — 内部结构体从 .c 提取到 .h

**选择**：将 `CeRhiDeviceVk` 等结构体从 `ce_rhi_vulkan.c` 移到 `ce_rhi_vulkan.h`，同时提供 `static inline` 辅助转换函数。

**理由**：
- 当前结构体定义在 `.c` 中，其他 render 模块内部文件无法引用（如未来的 `ce_rhi_vulkan_pipeline.c`）。
- 使用 `static inline` 转换函数而非宏，提供类型安全。
- 保持 `ce_rhi.h` 的不透明句柄设计不变——只有 render 模块内部文件 include `ce_rhi_vulkan.h`。

**替代方案**：
- ❌ 保持结构体在 `.c` 中：阻碍模块拆分，未来 pipeline/texture 分离到独立文件时无法共享类型。
- ❌ 使用 `void*` + 偏移量访问：不安全，易出错。

### Decision 2: AOI 算法 — 十字链表 vs 九宫格

**选择**：十字链表（Cross-Linked List）。

**理由**：
- 十字链表在实体密集区域查询效率高：O(N_aoi)，N_aoi 是 AOI 半径内的实体数，通常远小于总实体数。
- 移动操作仅需在链表中调整位置，不需要重新计算网格归属。
- 链表遍历顺序固定（按坐标排序），支持帧同步回放所需的确定性。
- 经典 MMO 方案（KBEngine、Skynet 均有类似实现），经过大规模验证。

**替代方案**：
- ❌ 九宫格（Grid-based）：实现更简单，但实体密集时一个格子内实体过多，查询退化为 O(N_cell)。且跨格子边界查询需要处理多个格子。
- ❌ 四叉树：实现复杂，动态插入删除开销大，对 AOI 场景过度设计。

**十字链表结构**：

```
X 轴链表（按 X 坐标升序）:
  [head] ↔ [A(x=10)] ↔ [B(x=30)] ↔ [C(x=80)] ↔ [D(x=120)] ↔ [tail]

Y 轴链表（按 Y 坐标升序）:
  [head] ↔ [C(y=20)] ↔ [A(y=40)] ↔ [B(y=60)] ↔ [D(y=90)] ↔ [tail]

查询 B(x=30, y=60) 周围实体（AOI 半径=50）:
  X 轴向左: A (|30-10|=20 ≤ 50 ✓)
  X 轴向右: C (|80-30|=50 ≤ 50 ✓) → D (|120-30|=90 > 50 ✗ 停止)
  Y 轴向上: C (已找到，跳过)
  Y 轴向下: D (|90-60|=30 ≤ 50 ✓)
  结果: {A, C, D}
```

### Decision 3: Cell 分裂策略 — 四等分

**选择**：Cell 实体数超过 `max_entities`（默认 150）时，将 Cell 四等分为 4 个子 Cell。

**理由**：
- 四等分是 BigWorld 的经典策略，简单且有效。
- 每次分裂产生固定 4 个子 Cell，邻居关系更新规则明确。
- 配合合并策略（实体数 < `min_entities` 时合并），形成动态平衡。

**替代方案**：
- ❌ 二等分：分裂次数过多，Cell 数量增长快。
- ❌ 动态 K 等分：复杂度高，邻居关系难以维护。

**分裂流程**：

```
原 Cell: [0,0] → [100,100]（实体数 151 > 150）

分裂为:
  Cell A: [0,0]   → [50,50]   (实体 40)
  Cell B: [50,0]  → [100,50]  (实体 35)
  Cell C: [0,50]  → [50,100]  (实体 38)
  Cell D: [50,50] → [100,100] (实体 38)

邻居更新:
  A 的邻居: B(右), C(下), D(右下) + 原 Cell 的外部邻居中与 A 相邻的
  B 的邻居: A(左), D(下) + 原 Cell 的外部邻居中与 B 相邻的
  ...
```

### Decision 4: 跨 Cell 查询 — 按需查询邻居

**选择**：仅当实体距离 Cell 边界 < AOI 半径时，才查询邻居 Cell。

**理由**：
- 大多数实体位于 Cell 内部，不需要跨 Cell 查询，避免不必要的开销。
- 边界检测开销极小（4 次浮点比较）。

**边界检测逻辑**：

```c
bool needs_neighbor_query(CeAoiNode* node, CeCell* cell, float aoi_radius) {
    return (node->x - cell->bounds.min_x < aoi_radius) ||  // 靠近左边界
           (cell->bounds.max_x - node->x < aoi_radius) ||  // 靠近右边界
           (node->y - cell->bounds.min_y < aoi_radius) ||  // 靠近下边界
           (cell->bounds.max_y - node->y < aoi_radius);    // 靠近上边界
}
```

### Decision 5: Uniform 传递 — Push Constants

**选择**：使用 Vulkan Push Constants 传递 `CeMat4` 矩阵。

**理由**：
- Push Constants 是最简单的 Uniform 传递方式，无需 Descriptor Set 管理。
- `sizeof(CeMat4)` = 64 字节，在 Vulkan 最小保证的 128 字节限制内。
- v0.2 阶段避免引入 Descriptor Set 复杂度（延后到 v0.3）。

**替代方案**：
- ❌ Uniform Buffer：需要创建 Buffer、分配内存、绑定 Descriptor Set，v0.2 阶段过度设计。
- ❌ Specialization Constants：仅适用于编译期常量，不适用于每帧变化的 MVP 矩阵。

### Decision 6: 单线程设计

**选择**：AOI 和 Cell 所有操作在服务端主循环中执行，无需锁。

**理由**：
- v0.2 服务端为单线程模型（类似 Skynet 的 Actor 模型，每个 Cell 在单线程中处理）。
- 避免锁竞争，简化实现和调试。
- 未来多线程扩展时，可以按 Cell 粒度加锁（每个 Cell 独立锁）。

## Architecture

### 模块依赖图

```
                        ┌──────────────────┐
                        │  chaos_server    │
                        │  (可执行文件)     │
                        └────────┬─────────┘
                                 │ 链接
                  ┌──────────────┼──────────────┐
                  │              │              │
           ┌──────▼──────┐ ┌────▼─────┐ ┌──────▼──────┐
           │ engine_server│ │engine_ecs│ │engine_network│
           │ (AOI + Cell) │ └──────────┘ └─────────────┘
           └──────┬───────┘
                  │ 依赖
         ┌────────┼────────┐
         │        │        │
    ┌────▼───┐ ┌─▼──┐ ┌───▼──────┐
    │ce_math │ │ce_ │ │ce_types  │
    │        │ │mem │ │(public)  │
    └────────┘ └────┘ └──────────┘
```

### 文件结构

```
src_c/
├── render/
│   ├── ce_rhi.h              # 公开 RHI 接口（不变）
│   ├── ce_rhi_vulkan.h       # 【重写】11 → ~200 行
│   └── ce_rhi_vulkan.c       # 【修改】使用新头文件 + 补全 stub
│
├── server/                   # 【新增目录】
│   ├── ce_server_types.h     # 共享类型定义
│   ├── ce_aoi.h              # AOI 接口
│   ├── ce_aoi.c              # AOI 十字链表实现
│   ├── ce_cell.h             # Cell 接口
│   ├── ce_cell.c             # Cell 管理实现
│   └── CMakeLists.txt        # 服务端构建
│
tests/
├── test_aoi.c                # 【新增】AOI 单元测试
└── test_cell.c               # 【新增】Cell 单元测试
```

### Cell 内嵌 AOI 设计

每个 Cell 有独立的 AOI 实例，Cell 管理器负责跨 Cell 协调：

```
CeCellManager
  ├── cells[0] → CeCell
  │     ├── bounds: [0,0] → [100,100]
  │     ├── entity_count: 85
  │     └── aoi → CeAoiSystem (独立实例)
  │           ├── X 轴链表: [E1] ↔ [E2] ↔ [E3] ↔ ...
  │           └── Y 轴链表: [E2] ↔ [E1] ↔ [E5] ↔ ...
  │
  ├── cells[1] → CeCell
  │     ├── bounds: [100,0] → [200,100]
  │     ├── entity_count: 120
  │     └── aoi → CeAoiSystem (独立实例)
  │
  └── ...
```

## Risks / Trade-offs

### Risk 1: Vulkan 后端未经完整测试

- **风险**：补全的纹理/着色器/管线函数可能包含 Vulkan 使用错误，validation layer 报错。
- **缓解**：每个函数实现后立即用 validation layer 验证；参考现有 1008 行 Vulkan 代码中的模式。
- **影响**：中等。不影响 OpenGL 后端，仅 Vulkan 后端受影响。

### Risk 2: 十字链表在极端稀疏场景退化

- **风险**：实体极度稀疏分布时，十字链表遍历可能退化为 O(N_total)（沿链表遍历很远才遇到实体）。
- **缓解**：配合 Cell 网格，每个 Cell 内的实体数有限（分裂阈值 150），链表长度有上界。
- **影响**：低。实际 MMO 场景实体分布不会极端稀疏。

### Risk 3: Cell 分裂/合并导致 AOI 事件风暴

- **风险**：Cell 分裂时，所有实体触发 CE_AOI_LEAVE + CE_AOI_ENTER，可能导致大量网络消息。
- **缓解**：分裂/合并不是每帧触发（有阈值保护）；可以在 `ce_cell_update` 中限制每帧最多处理 1 次分裂/合并。
- **影响**：中等。需要在集成阶段测试实际消息量。

### Risk 4: 跨 Cell 查询遗漏

- **风险**：实体在 Cell 角落时，可能同时靠近 2-3 个边界，查询逻辑可能遗漏某个邻居 Cell。
- **缓解**：边界检测覆盖 4 个方向，查询所有满足条件的邻居 Cell；单元测试覆盖角落场景。
- **影响**：中等。遗漏会导致实体"看不到"应该看到的其他实体。

### Trade-off: 单线程 vs 多线程

- **选择**：v0.2 单线程。
- **代价**：无法利用多核并行处理多个 Cell。
- **收益**：实现简单，无锁竞争，确定性保证。
- **未来**：按 Cell 粒度加锁扩展为多线程（每个 Cell 独立锁，跨 Cell 操作按 ID 顺序加锁避免死锁）。
