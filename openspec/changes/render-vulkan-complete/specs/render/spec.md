# Render — Vulkan RHI 完整化（v0.2 Delta）

> 来源: chaos-engine-spec-v0.2.md | 状态: 草案 | 变更类型: MODIFIED

## 概述

v0.1 的 Render 模块定义了完整的 RHI 接口（`ce_rhi.h`，25 个函数），但 Vulkan 后端仅 1008 行实现代码，头文件 `ce_rhi_vulkan.h` 只有 11 行，内部结构体全部隐藏在 `.c` 文件中，纹理/着色器/管线接口全部是 stub。v0.2 将头文件补全到 ~200 行并实现所有 stub 函数，使 Vulkan 后端具备完整的纹理、着色器、管线创建能力。

---

## MODIFIED Requirements

### Requirement: Vulkan RHI 内部头文件完整化

`ce_rhi_vulkan.h` SHALL 从 11 行扩展到 ~200 行，暴露所有 Vulkan 内部结构体，使 render 模块内部其他文件可以引用这些结构体。

头文件 MUST 包含以下结构体定义：
- `CeRhiDeviceVk`：Vulkan 设备结构（Instance、Device、Queue、Swapchain、CommandPool、同步原语、Pipeline 等）
- `CeRhiBufferVk`：Vulkan 缓冲结构（VkBuffer + VkDeviceMemory + size）
- `CeRhiTextureVk`：Vulkan 纹理结构（VkImage + VkDeviceMemory + VkImageView + format + 尺寸）
- `CeRhiShaderVk`：Vulkan 着色器结构（VkShaderModule + stage）
- `CeRhiPipelineVk`：Vulkan 管线结构（VkPipeline + VkPipelineLayout）

头文件 MUST 提供以下 inline 辅助函数用于从不透明句柄转换为内部类型：
- `rhi_vk_device(CeRhiDevice*)` → `CeRhiDeviceVk*`
- `rhi_vk_buffer(CeRhiBuffer*)` → `CeRhiBufferVk*`
- `rhi_vk_texture(CeRhiTexture*)` → `CeRhiTextureVk*`
- `rhi_vk_shader(CeRhiShader*)` → `CeRhiShaderVk*`
- `rhi_vk_pipeline(CeRhiPipeline*)` → `CeRhiPipelineVk*`

头文件 MUST 声明内部辅助函数 `rhi_vk_find_memory_type()` 用于查询 Vulkan 内存类型索引。

#### Scenario: 头文件包含所有内部结构体

- **WHEN** 检查 `ce_rhi_vulkan.h` 的内容
- **THEN** 文件包含 `CeRhiDeviceVk`、`CeRhiBufferVk`、`CeRhiTextureVk`、`CeRhiShaderVk`、`CeRhiPipelineVk` 五个结构体的完整定义
- **AND** 每个结构体包含对应的 Vulkan 句柄和元数据字段

#### Scenario: inline 辅助函数可用

- **WHEN** render 模块内部代码需要访问 Vulkan 设备句柄
- **THEN** 可以通过 `rhi_vk_device(device)->device` 获取 `VkDevice` 句柄
- **AND** 转换是零开销的（static inline 指针强转）

#### Scenario: 编译兼容性

- **WHEN** 修改 `ce_rhi_vulkan.c` 使其 `#include` 新头文件并使用其中定义的结构体
- **THEN** 编译通过，无重复定义错误
- **AND** Client/Server/Headless 三种编译模式均通过

---

### Requirement: 纹理创建与销毁

`rhi_create_texture` SHALL 从返回 NULL 的 stub 变为完整实现，能够创建 VkImage、分配 VkDeviceMemory、创建 VkImageView，并支持通过 staging buffer 上传纹理数据。

`rhi_destroy_texture` SHALL 正确销毁 VkImageView、VkImage、释放 VkDeviceMemory。

纹理创建 MUST 支持以下格式映射：
- `CE_TEXTURE_RGBA8` → `VK_FORMAT_R8G8B8A8_SRGB`
- `CE_TEXTURE_RGBA32F` → `VK_FORMAT_R32G32B32A32_SFLOAT`

#### Scenario: 创建 RGBA8 纹理

- **WHEN** 调用 `rhi_create_texture(device, &desc)` 其中 `desc.format = CE_TEXTURE_RGBA8, desc.width = 256, desc.height = 256`
- **THEN** 返回非 NULL 的 `CeRhiTexture*` 句柄
- **AND** 内部创建了有效的 VkImage、VkDeviceMemory、VkImageView
- **AND** VkImage 的 usage 包含 `VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`

#### Scenario: 创建带数据的纹理

- **WHEN** 调用 `rhi_create_texture` 且 `desc.data` 不为 NULL
- **THEN** 纹理数据通过 staging buffer 上传到 GPU
- **AND** VkImage 的 layout 最终为 `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`

#### Scenario: 销毁纹理

- **WHEN** 调用 `rhi_destroy_texture(device, texture)`
- **THEN** VkImageView、VkImage、VkDeviceMemory 被正确销毁
- **AND** 无 Vulkan validation layer 报错
- **AND** 无内存泄漏

---

### Requirement: 着色器创建与销毁

`rhi_create_shader` SHALL 从返回 NULL 的 stub 变为完整实现，能够从 SPIR-V 二进制数据创建 VkShaderModule。

`rhi_destroy_shader` SHALL 正确销毁 VkShaderModule。

SPIR-V 数据约定：调用方在 SPIR-V 二进制数据前 4 字节放置数据大小（字节数），`source` 指针指向 SPIR-V 魔数 `0x07230203` 起始位置。

#### Scenario: 创建顶点着色器

- **WHEN** 调用 `rhi_create_shader(device, CE_SHADER_VERTEX, spirv_data)`
- **THEN** 返回非 NULL 的 `CeRhiShader*` 句柄
- **AND** 内部创建了有效的 VkShaderModule
- **AND** `shader->stage == CE_SHADER_VERTEX`

#### Scenario: 创建片段着色器

- **WHEN** 调用 `rhi_create_shader(device, CE_SHADER_FRAGMENT, spirv_data)`
- **THEN** 返回非 NULL 的 `CeRhiShader*` 句柄
- **AND** 内部创建了有效的 VkShaderModule

#### Scenario: 销毁着色器

- **WHEN** 调用 `rhi_destroy_shader(device, shader)`
- **THEN** VkShaderModule 被正确销毁
- **AND** 无 Vulkan validation layer 报错

---

### Requirement: 管线创建与销毁

`rhi_create_pipeline` SHALL 从返回 NULL 的 stub 变为完整实现，能够根据 `CePipelineDesc` 创建完整的 VkPipeline（包含 VkPipelineLayout）。

`rhi_destroy_pipeline` SHALL 正确销毁 VkPipeline 和 VkPipelineLayout。

管线创建 MUST 支持：
- 顶点输入布局配置（binding description + attribute descriptions）
- 顶点着色器和片段着色器绑定
- 基本的管线状态（拓扑类型 = Triangle List、多边形模式 = Fill、Cull Mode = Back、Front Face = CCW）
- Viewport 和 Scissor 动态状态

#### Scenario: 创建基本图形管线

- **WHEN** 调用 `rhi_create_pipeline(device, &desc)` 其中 desc 包含有效的顶点着色器和片段着色器
- **THEN** 返回非 NULL 的 `CeRhiPipeline*` 句柄
- **AND** 内部创建了有效的 VkPipeline 和 VkPipelineLayout
- **AND** 管线使用 Triangle List 拓扑

#### Scenario: 自定义顶点布局

- **WHEN** 调用 `rhi_create_pipeline` 且 `desc.vertex_stride = 32, desc.attr_count = 3`
- **THEN** 创建的 VkPipeline 使用指定的顶点步长和 3 个顶点属性
- **AND** 顶点属性按顺序绑定到 location 0, 1, 2

#### Scenario: 销毁管线

- **WHEN** 调用 `rhi_destroy_pipeline(device, pipeline)`
- **THEN** VkPipeline 和 VkPipelineLayout 被正确销毁
- **AND** 无 Vulkan validation layer 报错

---

### Requirement: Uniform 矩阵传递

`rhi_set_uniform_mat4` SHALL 从空函数变为完整实现，通过 Vulkan Push Constants 机制将 4x4 矩阵传递给着色器。

Push Constants 范围 MUST 为 `VK_SHADER_STAGE_VERTEX_BIT`，大小 MUST 为 `sizeof(CeMat4)`（64 字节），在 128 字节的 Push Constants 限制内。

#### Scenario: 传递 MVP 矩阵

- **WHEN** 调用 `rhi_set_uniform_mat4(device, "mvp", &mat)` 其中 mat 是有效的 4x4 矩阵
- **THEN** 矩阵数据通过 `vkCmdPushConstants` 写入当前命令缓冲
- **AND** 着色器可以通过 `layout(push_constant) uniform MVP { mat4 mvp; }` 访问

#### Scenario: 多次调用覆盖

- **WHEN** 在同一帧内先后调用两次 `rhi_set_uniform_mat4` 使用不同矩阵值
- **THEN** 第二次调用的值覆盖第一次
- **AND** 后续绘制调用使用第二次设置的矩阵

---

## ADDED Requirements

### Requirement: Vulkan 内存类型查询

`rhi_vk_find_memory_type` SHALL 根据类型过滤器（`type_filter`）和所需属性（`props`）查找合适的 Vulkan 内存类型索引。

该函数 MUST 遍历 `VkPhysicalDeviceMemoryProperties`，返回第一个同时满足 `type_filter` 位匹配和 `propertyFlags` 包含所需属性的内存类型索引。

#### Scenario: 查找设备本地内存

- **WHEN** 调用 `rhi_vk_find_memory_type(dev, type_filter, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)`
- **THEN** 返回一个有效的内存类型索引
- **AND** 该索引对应的内存堆具有 `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` 属性

#### Scenario: 查找主机可见内存

- **WHEN** 调用 `rhi_vk_find_memory_type(dev, type_filter, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)`
- **THEN** 返回一个有效的内存类型索引
- **AND** 该索引对应的内存堆同时具有 HOST_VISIBLE 和 HOST_COHERENT 属性
