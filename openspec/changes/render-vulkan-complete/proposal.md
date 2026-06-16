# render-vulkan-complete

v0.2: Vulkan RHI 完整化 + Server AOI/Cell 空间子系统

---

## Why

v0.1 完成了 ChaosEngine 的基础骨架：Core 数学/内存模块、ECS Archetype 系统、Network 帧同步协议、Render RHI 接口定义 + OpenGL 3.3 后端、Plugin 插件框架、Script Lua 绑定。但 Render 模块的 Vulkan 后端仅 1008 行实现代码，头文件 `ce_rhi_vulkan.h` 只有 11 行（仅 include 两个头），内部结构体全部隐藏在 `.c` 文件中，纹理/着色器/管线接口全部是 stub（返回 NULL 或空函数）。这导致：

1. **Vulkan 后端不可用**：任何需要 Vulkan 渲染的模块（Editor 3D 视口、未来 Metal 后端参考）都无法工作。
2. **服务端空间管理缺失**：MMO 类游戏需要 AOI（Area of Interest）和 Cell 大地图管理，v0.1 完全没有服务端模块。
3. **架构不完整**：`src_c/server/` 目录不存在，`CE_BUILD_SERVER` 模式下无空间子系统可链接。

v0.2 的目标是补齐这两块核心短板，使 ChaosEngine 具备"可渲染的 Vulkan 客户端"和"可管理空间的服务端"。

## What Changes

### Render 模块：Vulkan RHI 完整化

- **头文件补全**：`ce_rhi_vulkan.h` 从 11 行扩展到 ~200 行，暴露 `CeRhiDeviceVk`、`CeRhiBufferVk`、`CeRhiTextureVk`、`CeRhiShaderVk`、`CeRhiPipelineVk` 等内部结构体，以及 `rhi_vk_device()` 等 inline 辅助转换函数。
- **Stub 实现补全**：`rhi_create_texture`、`rhi_create_shader`、`rhi_create_pipeline`、`rhi_set_uniform_mat4` 从 stub 变为完整实现，包含对应的 destroy 函数。
- **文件变更**：仅修改 `src_c/render/ce_rhi_vulkan.h` 和 `src_c/render/ce_rhi_vulkan.c`，不改变公开接口 `ce_rhi.h`。

### Server 模块：AOI + Cell 空间子系统

- **新增 `src_c/server/` 目录**：包含 `ce_aoi.h/c`（AOI 十字链表）、`ce_cell.h/c`（Cell 大地图管理）、`ce_server_types.h`（共享类型定义）、`CMakeLists.txt`。
- **AOI 十字链表**：每个实体同时挂在 X 轴和 Y 轴的有序链表上，移动时在链表中调整位置，查询周围实体沿两个轴向遍历。支持 enter/leave/move 事件回调。
- **Cell 大地图管理**：将世界划分为网格 Cell，支持动态分裂（实体数 > 阈值）和合并（实体数 < 阈值），支持跨 Cell 实体迁移和跨 Cell 边界查询。
- **编译条件**：仅在 `CE_BUILD_SERVER` 模式下编译，不影响 Client/Headless 构建。

## Capabilities

### New Capabilities

- `server-aoi`: AOI 十字链表子系统，提供实体进出视野检测和周围实体查询
- `server-cell`: Cell 大地图管理，提供 Cell 网格划分、动态分裂合并、跨 Cell 实体迁移

### Modified Capabilities

- `render`: Vulkan RHI 内部头文件补全 + 纹理/着色器/管线 stub 实现（接口不变，实现从 stub 变为完整）

## Impact

### 受影响的代码

| 文件/目录 | 影响类型 | 说明 |
|-----------|----------|------|
| `src_c/render/ce_rhi_vulkan.h` | 重写 | 11 → ~200 行，暴露内部结构体 |
| `src_c/render/ce_rhi_vulkan.c` | 修改 | 使用新头文件结构体；补全 stub 函数 |
| `src_c/server/` | 新增 | 6 个新文件（aoi/cell/types/cmake） |
| `CMakeLists.txt`（根） | 修改 | 新增 `CE_BUILD_SERVER_CELL` 选项 |
| `src_c/server/CMakeLists.txt` | 新增 | 服务端模块构建脚本 |

### 不受影响的模块

- `ce_rhi.h` 公开接口不变
- OpenGL 后端完全不受影响
- ECS、Network、Core、Plugin、Script 模块不受影响
- Client/Headless 编译目标不受影响

### MVP 范围

**Render MVP**：
- `ce_rhi_vulkan.h` 头文件补全，编译通过
- `rhi_create_texture` / `rhi_destroy_texture` 完整实现
- `rhi_create_shader` / `rhi_destroy_shader` 完整实现
- `rhi_create_pipeline` / `rhi_destroy_pipeline` 完整实现
- `rhi_set_uniform_mat4` 通过 Push Constants 实现

**Server MVP**：
- `ce_server_types.h` 类型定义
- `ce_aoi.h/c` 十字链表完整实现（enter/leave/move/query）
- `ce_cell.h/c` Cell 管理完整实现（find/split/merge/migrate/query）
- 单元测试覆盖核心路径
- `chaos_server` 链接 `engine_server`

**延后到 v0.3**：
- Editor 模块（3D 视口、资产浏览器）
- Lua 战斗逻辑层
- Cell 跨进程迁移（当前仅单进程）
- Vulkan 后端的 Descriptor Set 管理
