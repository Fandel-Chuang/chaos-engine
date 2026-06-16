# Tasks: Render Vulkan 完整化 + Server AOI/Cell

> 按依赖顺序排列，每个 task 可在一个 session 内完成。标注编译验证点（🔨）。

---

## 1. Render 头文件补全

- [ ] 1.1 从 `ce_rhi_vulkan.c` 提取 `CeRhiDeviceVk` 结构体到 `ce_rhi_vulkan.h`，包含所有字段（Instance、Device、Queue、Swapchain、CommandPool、同步原语、Pipeline、Window、调试等）
- [ ] 1.2 从 `ce_rhi_vulkan.c` 提取 `CeRhiBufferVk` 结构体到 `ce_rhi_vulkan.h`
- [ ] 1.3 新增 `CeRhiTextureVk` 结构体定义（VkImage + VkDeviceMemory + VkImageView + format + 尺寸）
- [ ] 1.4 新增 `CeRhiShaderVk` 结构体定义（VkShaderModule + stage）
- [ ] 1.5 新增 `CeRhiPipelineVk` 结构体定义（VkPipeline + VkPipelineLayout）
- [ ] 1.6 添加 `static inline` 辅助转换函数：`rhi_vk_device()`、`rhi_vk_buffer()`、`rhi_vk_texture()`、`rhi_vk_shader()`、`rhi_vk_pipeline()`
- [ ] 1.7 添加常量定义：`CE_VK_MAX_FRAMES_IN_FLIGHT`、`CE_VK_MAX_DESCRIPTOR_SETS`
- [ ] 1.8 声明 `rhi_vk_find_memory_type()` 函数
- [ ] 1.9 修改 `ce_rhi_vulkan.c`：移除结构体定义，改为 `#include` 新头文件，使用 `rhi_vk_device()` 等辅助函数替换直接访问
- [ ] 1.10 🔨 编译验证：Client/Server/Headless 三种模式均通过

---

## 2. Render Stub 实现 — 纹理

- [ ] 2.1 实现 `rhi_vk_find_memory_type()`：遍历 `VkPhysicalDeviceMemoryProperties`，匹配 type_filter 和 propertyFlags
- [ ] 2.2 实现 `rhi_create_texture()`：创建 VkImage（2D、SRGB、TRANSFER_DST | SAMPLED）、分配 VkDeviceMemory（DEVICE_LOCAL）、绑定内存、创建 VkImageView
- [ ] 2.3 实现纹理格式映射函数：`CE_TEXTURE_RGBA8` → `VK_FORMAT_R8G8B8A8_SRGB`，`CE_TEXTURE_RGBA32F` → `VK_FORMAT_R32G32B32A32_SFLOAT`
- [ ] 2.4 实现 staging buffer 纹理数据上传：创建 staging buffer（HOST_VISIBLE | HOST_COHERENT）、map/memcpy/unmap、通过 `vkCmdCopyBufferToImage` 上传、转换 layout 到 `SHADER_READ_ONLY_OPTIMAL`
- [ ] 2.5 实现 `rhi_destroy_texture()`：销毁 VkImageView → VkImage → 释放 VkDeviceMemory
- [ ] 2.6 🔨 编译验证 + Vulkan validation layer 验证（无 error/warning）

---

## 3. Render Stub 实现 — 着色器

- [ ] 3.1 实现 `rhi_create_shader()`：从 SPIR-V 二进制数据创建 VkShaderModule（约定：数据前 4 字节为 codeSize，source 指向 SPIR-V 魔数）
- [ ] 3.2 实现 `rhi_destroy_shader()`：销毁 VkShaderModule
- [ ] 3.3 🔨 编译验证 + validation layer 验证

---

## 4. Render Stub 实现 — 管线

- [ ] 4.1 实现 `rhi_create_pipeline()`：
  - 从 `CePipelineDesc` 提取顶点/片段着色器
  - 构建 `VkPipelineShaderStageCreateInfo` 数组
  - 构建 `VkVertexInputBindingDescription`（使用 desc->vertex_stride）
  - 构建 `VkVertexInputAttributeDescription` 数组（使用 desc->attr_count，默认 float3 格式）
  - 配置 `VkPipelineVertexInputStateCreateInfo`
  - 配置 `VkPipelineInputAssemblyStateCreateInfo`（Triangle List）
  - 配置 `VkPipelineViewportStateCreateInfo`（动态 viewport + scissor）
  - 配置 `VkPipelineRasterizationStateCreateInfo`（Fill、Cull Back、CCW）
  - 配置 `VkPipelineMultisampleStateCreateInfo`（1 sample）
  - 配置 `VkPipelineColorBlendStateCreateInfo`（无混合，或根据 desc 配置）
  - 创建 VkPipelineLayout（包含 Push Constant range：vertex stage, 0~64 bytes）
  - 创建 VkPipeline
- [ ] 4.2 实现 `rhi_destroy_pipeline()`：销毁 VkPipeline → VkPipelineLayout
- [ ] 4.3 🔨 编译验证 + validation layer 验证

---

## 5. Render Stub 实现 — Uniform

- [ ] 5.1 实现 `rhi_set_uniform_mat4()`：通过 `vkCmdPushConstants` 传递 `CeMat4` 到 `VK_SHADER_STAGE_VERTEX_BIT`
- [ ] 5.2 确保 PipelineLayout 创建时已注册 Push Constant range（在 task 4.1 中完成）
- [ ] 5.3 🔨 编译验证 + validation layer 验证

---

## 6. Server 类型定义与构建骨架

- [ ] 6.1 创建 `src_c/server/` 目录
- [ ] 6.2 创建 `ce_server_types.h`：
  - `CeServerEntityId`（uint32_t）
  - `CeCellId`（uint32_t）
  - `CeAoiNode`（entity_id, x, y, x_prev/next, y_prev/next, cell_id, radius）
  - `CeAoiEventType` 枚举（ENTER/LEAVE/MOVE）
  - `CeAoiEvent`（type, subject, object, x, y）
  - `CeAoiEventCallback` 函数指针类型
  - `CeCellState` 枚举（ACTIVE/SPLITTING/MERGING/MIGRATING）
  - `CeCellBounds`（min_x, min_y, max_x, max_y）
  - `CeCell`（id, bounds, state, entity_count, max/min_entities, process_id, neighbors[8], neighbor_count, aoi）
  - `CeCellManager`（cells, cell_count, cell_capacity, default_cell_width/height, world bounds, next_cell_id）
- [ ] 6.3 创建 `src_c/server/CMakeLists.txt`：条件编译 `CE_BUILD_SERVER`，创建 `engine_server` 静态库
- [ ] 6.4 修改根 `CMakeLists.txt`：添加 `CE_BUILD_SERVER_CELL` 选项，`add_subdirectory(src_c/server)`
- [ ] 6.5 修改 `chaos_server` 目标：链接 `engine_server`
- [ ] 6.6 🔨 编译验证：Server 模式编译通过，Client/Headless 模式不受影响

---

## 7. AOI 十字链表实现

- [ ] 7.1 创建 `ce_aoi.h`：声明所有接口函数（init/shutdown/enter/leave/move/query_nearby/count_nearby/distance_sq/entity_count/debug_print）
- [ ] 7.2 实现内部数据结构：哨兵头节点（X 轴 head、Y 轴 head）、实体节点哈希表（用于 O(1) 查找实体节点）
- [ ] 7.3 实现 `ce_aoi_init()`：初始化哨兵节点、设置 AOI 半径和回调
- [ ] 7.4 实现 `ce_aoi_shutdown()`：遍历释放所有节点
- [ ] 7.5 实现 `ce_aoi_enter()`：
  - 创建 CeAoiNode
  - 沿 X 轴链表找到插入位置（保持 X 坐标升序），插入
  - 沿 Y 轴链表找到插入位置（保持 Y 坐标升序），插入
  - 遍历周围实体，触发 CE_AOI_ENTER 事件（双向）
- [ ] 7.6 实现 `ce_aoi_leave()`：
  - 查找实体节点
  - 遍历周围实体，触发 CE_AOI_LEAVE 事件（双向）
  - 从 X/Y 轴链表移除
  - 释放节点
- [ ] 7.7 实现 `ce_aoi_move()`：
  - 记录移动前周围实体集合 S_old
  - 从链表移除 → 更新坐标 → 重新插入链表
  - 查询移动后周围实体集合 S_new
  - 计算差集：S_new - S_old → ENTER，S_old - S_new → LEAVE，交集 → MOVE
- [ ] 7.8 实现 `ce_aoi_query_nearby()`：沿 X 轴左右遍历 + Y 轴上下遍历，距离 > aoi_radius 停止，去重
- [ ] 7.9 实现 `ce_aoi_count_nearby()`：同 query_nearby 但不填充 buffer
- [ ] 7.10 实现 `ce_aoi_distance_sq()`：返回两点间欧氏距离平方（避免 sqrt）
- [ ] 7.11 实现 `ce_aoi_entity_count()` 和 `ce_aoi_debug_print()`
- [ ] 7.12 🔨 编译验证

---

## 8. AOI 单元测试

- [ ] 8.1 创建 `tests/test_aoi.c`：测试框架初始化
- [ ] 8.2 测试 `ce_aoi_enter`：插入 1 个实体、插入多个实体、重复插入同一 ID
- [ ] 8.3 测试 `ce_aoi_leave`：移除存在的实体、移除不存在的实体、移除后链表完整性
- [ ] 8.4 测试 `ce_aoi_move`：视野内移动（MOVE 事件）、移出视野（LEAVE 事件）、移入视野（ENTER 事件）
- [ ] 8.5 测试 `ce_aoi_query_nearby`：空 AOI、单个实体、多个实体、buffer 容量不足
- [ ] 8.6 测试 `ce_aoi_count_nearby`：验证计数与 query_nearby 返回数量一致
- [ ] 8.7 测试链表完整性：插入/删除/移动后，X 轴和 Y 轴链表保持有序且无断裂
- [ ] 8.8 压力测试：1000 实体随机移动 10000 次，验证无内存泄漏、链表完整、查询结果与暴力遍历一致
- [ ] 8.9 🔨 编译 + 运行测试，全部通过

---

## 9. Cell 大地图管理实现

- [ ] 9.1 创建 `ce_cell.h`：声明所有接口函数（init/shutdown/find_by_position/get/count/enter_entity/leave_entity/move_entity/update/split/merge/assign_process/get_process/query_nearby/debug_print）
- [ ] 9.2 实现 `ce_cell_init()`：
  - 计算 Cell 网格行列数
  - 分配 Cell 数组
  - 初始化每个 Cell 的边界、状态、阈值、内嵌 AOI
  - 计算邻居关系
- [ ] 9.3 实现 `ce_cell_shutdown()`：释放所有 Cell 及其内嵌 AOI
- [ ] 9.4 实现 `ce_cell_find_by_position()`：坐标 → Cell 索引映射（floor 公式）
- [ ] 9.5 实现 `ce_cell_get()` 和 `ce_cell_count()`
- [ ] 9.6 实现 `ce_cell_enter_entity()`：查找 Cell → 调用 `ce_aoi_enter` → entity_count++
- [ ] 9.7 实现 `ce_cell_leave_entity()`：查找实体所在 Cell → 调用 `ce_aoi_leave` → entity_count--
- [ ] 9.8 实现 `ce_cell_move_entity()`：
  - 计算新坐标 Cell ID
  - 同 Cell：调用 `ce_aoi_move`
  - 跨 Cell：旧 Cell AOI leave → 新 Cell AOI enter → 更新 cell_id
- [ ] 9.9 实现 `ce_cell_split()`：
  - 验证 entity_count > max_entities
  - 四等分边界
  - 创建 4 个子 Cell（分配新 ID，初始化 AOI）
  - 遍历原 Cell 实体，按坐标重新分配到子 Cell
  - 更新邻居关系（子 Cell 之间 + 与外部 Cell）
  - 标记原 Cell 为 CE_CELL_MERGING
- [ ] 9.10 实现 `ce_cell_merge()`：
  - 验证相邻 + 总实体数 < min_entities
  - 创建合并 Cell（边界 = 并集）
  - 迁移所有实体
  - 更新邻居关系
  - 释放旧 Cell
- [ ] 9.11 实现 `ce_cell_update()`：遍历所有 ACTIVE Cell，entity_count > max → split，相邻 Cell 总实体数 < min → merge
- [ ] 9.12 实现 `ce_cell_query_nearby()`：
  - 本 Cell AOI 查询
  - 边界检测：距离边界 < aoi_radius 时查询邻居 Cell
  - 合并结果去重
- [ ] 9.13 实现 `ce_cell_assign_process()` 和 `ce_cell_get_process()`（当前预留，设置/返回 process_id）
- [ ] 9.14 实现 `ce_cell_debug_print()`
- [ ] 9.15 🔨 编译验证

---

## 10. Cell 单元测试

- [ ] 10.1 创建 `tests/test_cell.c`：测试框架初始化
- [ ] 10.2 测试 `ce_cell_init`：创建不同尺寸的 Cell 网格，验证 Cell 数量和边界
- [ ] 10.3 测试 `ce_cell_find_by_position`：中心坐标、边界坐标、角落坐标、世界外坐标
- [ ] 10.4 测试 `ce_cell_enter_entity` / `ce_cell_leave_entity`：实体计数变化
- [ ] 10.5 测试 `ce_cell_move_entity`：Cell 内移动、跨 Cell 移动、entity_count 变化
- [ ] 10.6 测试 `ce_cell_split`：手动触发分裂，验证子 Cell 数量、实体分配、邻居关系
- [ ] 10.7 测试 `ce_cell_merge`：手动触发合并，验证合并后 Cell 边界和实体
- [ ] 10.8 测试 `ce_cell_update`：自动分裂（填充实体超阈值）、自动合并（减少实体低于阈值）
- [ ] 10.9 测试 `ce_cell_query_nearby`：Cell 中心查询、边界查询、跨 Cell 查询
- [ ] 10.10 测试邻居关系：验证各种位置 Cell 的邻居数量（角落 3、边缘 5、中心 8）
- [ ] 10.11 🔨 编译 + 运行测试，全部通过

---

## 11. 集成与收尾

- [ ] 11.1 在 `chaos_server` 主循环中集成 AOI/Cell：每帧调用 `ce_cell_update()`，ECS 位置同步到 `ce_cell_move_entity()`
- [ ] 11.2 添加集成测试：模拟 500 实体在 4×4 Cell 网格中随机移动，验证 AOI 事件正确性和 Cell 分裂/合并
- [ ] 11.3 更新 `docs/spec/chaos-engine-spec-v0.2.md` 状态为"已实现"
- [ ] 11.4 更新 `README.md` 中的版本状态和功能列表
- [ ] 11.5 🔨 全量编译验证：Client/Server/Headless 三种模式 + 所有单元测试通过
- [ ] 11.6 运行 Valgrind/ASan 内存检查：AOI 和 Cell 测试无内存泄漏
