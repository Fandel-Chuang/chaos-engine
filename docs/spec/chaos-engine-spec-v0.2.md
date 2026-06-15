# ChaosEngine 设计规格说明书 v0.2

> **状态：** 草案 | **日期：** 2026-06-15 | **作者：** zhongfangdao
>
> **v0.2 主题：** Render 模块完整化 + 服务端空间子系统（AOI + Cell）

---

## 目录

1. [版本变更摘要](#1-版本变更摘要)
2. [Render 模块：Vulkan RHI 完整化](#2-render-模块vulkan-rhi-完整化)
3. [服务端空间子系统：AOI + Cell](#3-服务端空间子系统aoi--cell)
4. [AOI 十字链表设计](#4-aoi-十字链表设计)
5. [Cell 大地图管理](#5-cell-大地图管理)
6. [模块依赖与接口](#6-模块依赖与接口)
7. [构建系统变更](#7-构建系统变更)
8. [验收标准](#8-验收标准)
9. [实现路线图](#9-实现路线图)

---

## 1. 版本变更摘要

### v0.1 → v0.2 变更

| 模块 | v0.1 状态 | v0.2 目标 |
|------|-----------|-----------|
| **Render** | Vulkan 后端 1008 行但头文件仅 11 行；纹理/着色器/管线 stub | 头文件补全，内部结构体暴露；纹理/着色器/管线 stub 实现 |
| **Server** | 不存在 | 新增 `src_c/server/` 模块：AOI 十字链表 + Cell 大地图管理 |
| **Editor** | 2 个空壳模块 | 延后到 v0.3 |
| **Lua 层** | 仅 hello.lua 占位 | 延后到 v0.3（战斗核心已移到 C） |

### 架构决策

- **战斗核心从 Lua 移到 C**：AOI 十字链表和 Cell 管理用纯 C 实现，Lua 仅做胶水层
- **服务端专属模块**：`src_c/server/` 仅在 `CE_BUILD_SERVER` 模式下编译
- **Render 头文件补全**：内部结构体从 `.c` 提取到 `ce_rhi_vulkan.h`，其他模块可引用

---

## 2. Render 模块：Vulkan RHI 完整化

### 2.1 现状分析

| 文件 | 行数 | 问题 |
|------|------|------|
| `ce_rhi.h` | 119 | ✅ 接口完整，25 个函数 |
| `ce_rhi_vulkan.h` | 11 | ❌ 仅 include 两个头，无内部结构体 |
| `ce_rhi_vulkan.c` | 1008 | ⚠️ 所有结构体定义在 .c 中，外部不可见 |

### 2.2 目标

将 `ce_rhi_vulkan.h` 从 11 行补全到 ~200 行，暴露内部结构体，同时补全纹理/着色器/管线 stub 实现。

### 2.3 头文件补全：ce_rhi_vulkan.h

```c
/*
 * ChaosEngine Vulkan RHI 内部头文件
 * 暴露 Vulkan 内部结构体，供 render 模块内部使用
 */

#ifndef CE_RHI_VULKAN_H
#define CE_RHI_VULKAN_H

#include "render/ce_rhi.h"
#include <vulkan/vulkan.h>

/* ---- 常量 ---- */

#define CE_VK_MAX_FRAMES_IN_FLIGHT 2
#define CE_VK_MAX_DESCRIPTOR_SETS  64

/* ---- 设备结构 ---- */

typedef struct CeRhiDeviceVk {
    /* Vulkan 实例 */
    VkInstance       instance;
    VkPhysicalDevice physical_device;
    VkDevice         device;
    VkQueue          graphics_queue;
    VkQueue          present_queue;
    uint32_t         graphics_family;
    uint32_t         present_family;

    /* 表面 & 交换链 */
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkFormat         swapchain_format;
    VkExtent2D       swapchain_extent;
    uint32_t         image_count;
    VkImage*         swapchain_images;
    VkImageView*     swapchain_views;
    VkFramebuffer*   framebuffers;

    /* 渲染通道 */
    VkRenderPass     render_pass;

    /* 命令 */
    VkCommandPool    command_pool;
    VkCommandBuffer* command_buffers;

    /* 同步 */
    VkSemaphore*     image_available;
    VkSemaphore*     render_finished;
    VkFence*         in_flight_fences;
    uint32_t         current_frame;
    uint32_t         current_image_index;

    /* 管线 */
    VkPipelineLayout pipeline_layout;
    VkPipeline       graphics_pipeline;

    /* 窗口 */
    CeWindow*        window;
    int              width;
    int              height;
    CeBool           should_close;

    /* 调试 */
    VkDebugUtilsMessengerEXT debug_messenger;
} CeRhiDeviceVk;

/* ---- 资源结构 ---- */

typedef struct CeRhiBufferVk {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    size_t         size;
} CeRhiBufferVk;

typedef struct CeRhiTextureVk {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkFormat       format;
    int            width;
    int            height;
} CeRhiTextureVk;

typedef struct CeRhiShaderVk {
    VkShaderModule module;
    CeShaderStage  stage;
} CeRhiShaderVk;

typedef struct CeRhiPipelineVk {
    VkPipeline       pipeline;
    VkPipelineLayout layout;
} CeRhiPipelineVk;

/* ---- 内部辅助函数 ---- */

/** 从不透明句柄获取 Vulkan 设备 */
static inline CeRhiDeviceVk* rhi_vk_device(CeRhiDevice* d) {
    return (CeRhiDeviceVk*)d;
}

/** 从不透明句柄获取 Vulkan 缓冲 */
static inline CeRhiBufferVk* rhi_vk_buffer(CeRhiBuffer* b) {
    return (CeRhiBufferVk*)b;
}

/** 从不透明句柄获取 Vulkan 纹理 */
static inline CeRhiTextureVk* rhi_vk_texture(CeRhiTexture* t) {
    return (CeRhiTextureVk*)t;
}

/** 从不透明句柄获取 Vulkan 着色器 */
static inline CeRhiShaderVk* rhi_vk_shader(CeRhiShader* s) {
    return (CeRhiShaderVk*)s;
}

/** 从不透明句柄获取 Vulkan 管线 */
static inline CeRhiPipelineVk* rhi_vk_pipeline(CeRhiPipeline* p) {
    return (CeRhiPipelineVk*)p;
}

/* ---- 内存类型查询 ---- */

uint32_t rhi_vk_find_memory_type(CeRhiDeviceVk* dev, uint32_t type_filter,
                                  VkMemoryPropertyFlags props);

#endif /* CE_RHI_VULKAN_H */
```

### 2.4 Stub 实现补全

当前 stub 函数：

| 函数 | 现状 | v0.2 实现 |
|------|------|-----------|
| `rhi_create_texture` | 返回 NULL | 创建 VkImage + VkDeviceMemory + VkImageView |
| `rhi_destroy_texture` | 空 | 销毁 VkImage/View/Memory |
| `rhi_create_shader` | 返回 NULL | 从 SPIR-V 源码创建 VkShaderModule |
| `rhi_destroy_shader` | 空 | 销毁 VkShaderModule |
| `rhi_create_pipeline` | 返回 NULL | 根据 CePipelineDesc 创建完整 VkPipeline |
| `rhi_destroy_pipeline` | 空 | 销毁 VkPipeline + VkPipelineLayout |
| `rhi_set_uniform_mat4` | 空 | 通过 Push Constants 或 Descriptor Set 传递矩阵 |

#### 2.4.1 rhi_create_texture 实现

```c
CeRhiTexture* rhi_create_texture(CeRhiDevice* device, const CeTextureDesc* desc) {
    CeRhiDeviceVk* dev = rhi_vk_device(device);
    CeRhiTextureVk* tex = calloc(1, sizeof(CeRhiTextureVk));

    VkFormat vk_format = VK_FORMAT_R8G8B8A8_SRGB;  // 根据 desc->format 映射
    tex->format = vk_format;
    tex->width  = desc->width;
    tex->height = desc->height;

    // 1. 创建 VkImage
    VkImageCreateInfo image_info = {0};
    image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType     = VK_IMAGE_TYPE_2D;
    image_info.format        = vk_format;
    image_info.extent        = (VkExtent3D){desc->width, desc->height, 1};
    image_info.mipLevels     = 1;
    image_info.arrayLayers   = 1;
    image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(dev->device, &image_info, NULL, &tex->image);

    // 2. 分配内存
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(dev->device, tex->image, &mem_req);
    VkMemoryAllocateInfo alloc_info = {0};
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_req.size;
    alloc_info.memoryTypeIndex = rhi_vk_find_memory_type(dev, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev->device, &alloc_info, NULL, &tex->memory);
    vkBindImageMemory(dev->device, tex->image, tex->memory, 0);

    // 3. 创建 ImageView
    VkImageViewCreateInfo view_info = {0};
    view_info.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image      = tex->image;
    view_info.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format     = vk_format;
    view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount     = 1;
    view_info.subresourceRange.layerCount     = 1;
    vkCreateImageView(dev->device, &view_info, NULL, &tex->view);

    // 4. 上传数据（如果有）
    if (desc->data) {
        // 通过 staging buffer 上传
        rhi_vk_upload_texture_data(dev, tex, desc->data, desc->width * desc->height * 4);
    }

    return (CeRhiTexture*)tex;
}
```

#### 2.4.2 rhi_create_shader 实现

```c
CeRhiShader* rhi_create_shader(CeRhiDevice* device, CeShaderStage stage,
                                const char* source) {
    CeRhiDeviceVk* dev = rhi_vk_device(device);
    CeRhiShaderVk* shader = calloc(1, sizeof(CeRhiShaderVk));
    shader->stage = stage;

    // source 是 SPIR-V 二进制数据（以字节数组形式传入）
    // 约定：source 前 4 字节是 SPIR-V 魔数 0x07230203
    VkShaderModuleCreateInfo info = {0};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = *(uint32_t*)(source - 4);  // 调用方在数据前放置大小
    info.pCode    = (const uint32_t*)source;

    vkCreateShaderModule(dev->device, &info, NULL, &shader->module);
    return (CeRhiShader*)shader;
}
```

#### 2.4.3 rhi_create_pipeline 实现

```c
CeRhiPipeline* rhi_create_pipeline(CeRhiDevice* device, const CePipelineDesc* desc) {
    CeRhiDeviceVk* dev = rhi_vk_device(device);
    CeRhiPipelineVk* pipeline = calloc(1, sizeof(CeRhiPipelineVk));

    CeRhiShaderVk* vs = rhi_vk_shader(desc->vertex_shader);
    CeRhiShaderVk* fs = rhi_vk_shader(desc->fragment_shader);

    VkPipelineShaderStageCreateInfo stages[2] = { /* vs + fs */ };

    // 顶点输入布局
    VkVertexInputBindingDescription binding = {
        .binding   = 0,
        .stride    = desc->vertex_stride,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription* attrs = malloc(
        sizeof(VkVertexInputAttributeDescription) * desc->attr_count);
    for (uint32_t i = 0; i < desc->attr_count; i++) {
        attrs[i] = (VkVertexInputAttributeDescription){
            .location = i,
            .binding  = 0,
            .format   = VK_FORMAT_R32G32B32_SFLOAT,  // 默认 float3
            .offset   = i * sizeof(float) * 3
        };
    }

    // ... 创建完整 VkPipeline（参考现有 create_graphics_pipeline）

    return (CeRhiPipeline*)pipeline;
}
```

#### 2.4.4 rhi_set_uniform_mat4 实现

使用 Push Constants 传递矩阵（最简单、无需 Descriptor Set）：

```c
void rhi_set_uniform_mat4(CeRhiDevice* device, const char* name, const CeMat4* mat) {
    CeRhiDeviceVk* dev = rhi_vk_device(device);
    // 使用 Push Constants，限制 128 字节（足够一个 mat4）
    vkCmdPushConstants(dev->command_buffers[dev->current_frame],
                       dev->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(CeMat4), mat);
}
```

### 2.5 文件变更清单

| 文件 | 操作 | 变更 |
|------|------|------|
| `src_c/render/ce_rhi_vulkan.h` | 重写 | 11 → ~200 行，暴露内部结构体 |
| `src_c/render/ce_rhi_vulkan.c` | 修改 | 使用新头文件的结构体；补全 stub 函数 |
| `src_c/render/ce_rhi.h` | 不变 | 接口已完整 |

---

## 3. 服务端空间子系统：AOI + Cell

### 3.1 模块定位

```
src_c/server/              # 【服务端专属】仅在 CE_BUILD_SERVER 编译
├── ce_aoi.h               # AOI 十字链表接口
├── ce_aoi.c               # AOI 十字链表实现
├── ce_cell.h              # Cell 大地图管理接口
├── ce_cell.c              # Cell 分裂/合并/进程分配
├── ce_server_types.h      # 服务端共享类型（EntityId, CellId, AOI 事件等）
└── CMakeLists.txt         # 服务端模块构建
```

### 3.2 设计原则

- **纯 C99**，无外部依赖（仅依赖 `core/` 和 `public_api/`）
- **单线程安全**：所有操作在服务端主循环中执行，无需锁
- **确定性**：十字链表遍历顺序固定（按 ID 排序），支持帧同步回放
- **可配置**：Cell 大小、AOI 半径、分裂阈值均可运行时调整

### 3.3 与现有模块的关系

```
                    ┌──────────────┐
                    │   server/    │  (新增)
                    │ AOI + Cell   │
                    └──────┬───────┘
                           │ 依赖
              ┌────────────┼────────────┐
              │            │            │
        ┌─────▼─────┐ ┌───▼────┐ ┌─────▼─────┐
        │   core/   │ │  ecs/  │ │ network/  │
        │ 数学/内存  │ │ 实体   │ │ 消息收发   │
        └───────────┘ └────────┘ └───────────┘
```

---

## 4. AOI 十字链表设计

### 4.1 核心思想

十字链表（Cross-Linked List）是 MMO 中经典的 AOI 算法，每个实体同时挂在 X 轴和 Y 轴的有序链表上。移动时只需在链表中调整位置，查询周围实体时沿两个轴向遍历。

```
X 轴链表:  [A] ←→ [B] ←→ [C] ←→ [D] ←→ [E]
Y 轴链表:  [C] ←→ [A] ←→ [E] ←→ [B] ←→ [D]

查询 B 周围的实体：
  沿 X 轴向左遍历 → A（在 AOI 半径内 ✓）
  沿 X 轴向右遍历 → C（在 AOI 半径内 ✓）→ D（超出半径 ✗，停止）
  沿 Y 轴向上遍历 → A（已找到，跳过）
  沿 Y 轴向下遍历 → D（超出半径 ✗，停止）

结果：{A, C}
```

### 4.2 数据结构

```c
/* ---- ce_server_types.h ---- */

/** 服务端实体 ID（对应 ECS 的 CeEntity） */
typedef uint32_t CeServerEntityId;

/** Cell ID */
typedef uint32_t CeCellId;

/** AOI 实体节点 */
typedef struct CeAoiNode {
    CeServerEntityId entity_id;

    /* 位置（2D 地图坐标，服务端 AOI 通常用 2D） */
    float x;
    float y;

    /* 十字链表指针 */
    struct CeAoiNode* x_prev;
    struct CeAoiNode* x_next;
    struct CeAoiNode* y_prev;
    struct CeAoiNode* y_next;

    /* 所属 Cell */
    CeCellId cell_id;

    /* 实体半径（用于 AOI 距离判断） */
    float radius;
} CeAoiNode;

/** AOI 事件类型 */
typedef enum CeAoiEventType {
    CE_AOI_ENTER = 0,   /* 实体进入视野 */
    CE_AOI_LEAVE = 1,   /* 实体离开视野 */
    CE_AOI_MOVE  = 2,   /* 实体在视野内移动 */
} CeAoiEventType;

/** AOI 事件 */
typedef struct CeAoiEvent {
    CeAoiEventType    type;
    CeServerEntityId  subject;    /* 谁触发了事件 */
    CeServerEntityId  object;     /* 与谁相关 */
    float             x, y;       /* 当前位置 */
} CeAoiEvent;

/** AOI 事件回调 */
typedef void (*CeAoiEventCallback)(const CeAoiEvent* event, void* user_data);
```

### 4.3 接口定义：ce_aoi.h

```c
/*
 * ChaosEngine AOI 十字链表
 * 纯 C99，单线程安全
 */

#ifndef CE_AOI_H
#define CE_AOI_H

#include "server/ce_server_types.h"
#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 ---- */

/** 初始化 AOI 系统 */
void ce_aoi_init(float aoi_radius, CeAoiEventCallback callback, void* user_data);

/** 关闭 AOI 系统，释放所有节点 */
void ce_aoi_shutdown(void);

/* ---- 实体管理 ---- */

/** 添加实体到 AOI */
CeResult ce_aoi_enter(CeServerEntityId entity_id, float x, float y, float radius);

/** 从 AOI 移除实体 */
void ce_aoi_leave(CeServerEntityId entity_id);

/** 移动实体（自动检测进出视野事件） */
CeResult ce_aoi_move(CeServerEntityId entity_id, float new_x, float new_y);

/* ---- 查询 ---- */

/** 获取实体周围的实体列表（结果写入 buffer，返回数量） */
int ce_aoi_query_nearby(CeServerEntityId entity_id, CeServerEntityId* buffer, int max_count);

/** 获取实体周围的实体数量 */
int ce_aoi_count_nearby(CeServerEntityId entity_id);

/** 获取两点间距离（平方，避免 sqrt） */
float ce_aoi_distance_sq(float x1, float y1, float x2, float y2);

/* ---- 调试 ---- */

/** 获取 AOI 系统中的实体总数 */
int ce_aoi_entity_count(void);

/** 打印 AOI 链表结构（调试用） */
void ce_aoi_debug_print(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_AOI_H */
```

### 4.4 核心算法

#### 4.4.1 插入实体

```
ce_aoi_enter(id, x, y, radius):
    1. 创建 CeAoiNode
    2. 沿 X 轴链表找到插入位置（保持 X 坐标升序）
    3. 插入 X 轴链表
    4. 沿 Y 轴链表找到插入位置（保持 Y 坐标升序）
    5. 插入 Y 轴链表
    6. 遍历周围实体，触发 CE_AOI_ENTER 事件（双向）
```

#### 4.4.2 移动实体

```
ce_aoi_move(id, new_x, new_y):
    1. 找到实体节点
    2. 记录移动前的周围实体集合 S_old
    3. 从 X/Y 轴链表中移除（保持链表完整）
    4. 更新坐标
    5. 重新插入 X/Y 轴链表
    6. 查询移动后的周围实体集合 S_new
    7. S_new - S_old → 触发 CE_AOI_ENTER 事件
    8. S_old - S_new → 触发 CE_AOI_LEAVE 事件
    9. S_old ∩ S_new → 触发 CE_AOI_MOVE 事件
```

#### 4.4.3 查询周围实体

```
ce_aoi_query_nearby(id, buffer, max):
    1. 找到实体节点 node
    2. 沿 X 轴向左遍历，距离 > aoi_radius 时停止
    3. 沿 X 轴向右遍历，距离 > aoi_radius 时停止
    4. 沿 Y 轴向上遍历，距离 > aoi_radius 时停止
    5. 沿 Y 轴向下遍历，距离 > aoi_radius 时停止
    6. 去重（一个实体可能在 X 和 Y 遍历中都出现）
    7. 返回结果
```

### 4.5 复杂度分析

| 操作 | 时间复杂度 | 说明 |
|------|-----------|------|
| 插入 | O(N_aoi) | N_aoi = AOI 半径内的实体数，通常 << 总实体数 |
| 移动 | O(N_aoi) | 同上 |
| 删除 | O(1) | 仅修改链表指针 |
| 查询周围 | O(N_aoi) | 同上 |

> 对比：暴力遍历 O(N_total)，十字链表在实体密集时优势明显。

---

## 5. Cell 大地图管理

### 5.1 核心思想

借鉴 BigWorld 的 Cell 架构：将大地图划分为多个 Cell，每个 Cell 可分配给不同进程。Cell 根据负载（实体数量）动态分裂和合并。

```
┌─────────────────────────────────────────────────────┐
│                    Game World                        │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
│  │  Cell A  │  │  Cell B  │  │  Cell C  │          │
│  │ (0,0)    │  │ (1,0)    │  │ (2,0)    │          │
│  │ Process1 │  │ Process2 │  │ Process1 │          │
│  │ 实体: 85 │  │ 实体: 120│  │ 实体: 45 │          │
│  └──────────┘  └──────────┘  └──────────┘          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
│  │  Cell D  │  │  Cell E  │  │  Cell F  │          │
│  │ (0,1)    │  │ (1,1)    │  │ (2,1)    │          │
│  │ Process3 │  │ Process2 │  │ Process3 │          │
│  │ 实体: 200│  │ 实体: 95 │  │ 实体: 60 │          │
│  └──────────┘  └──────────┘  └──────────┘          │
│                                                     │
│  Cell D 实体数 200 > 分裂阈值 150 → 分裂为 4 个子 Cell │
└─────────────────────────────────────────────────────┘
```

### 5.2 数据结构

```c
/* ---- ce_server_types.h (续) ---- */

/** Cell 状态 */
typedef enum CeCellState {
    CE_CELL_ACTIVE = 0,    /* 正常运行 */
    CE_CELL_SPLITTING,     /* 分裂中 */
    CE_CELL_MERGING,       /* 合并中 */
    CE_CELL_MIGRATING,     /* 迁移中 */
} CeCellState;

/** Cell 边界 */
typedef struct CeCellBounds {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} CeCellBounds;

/** Cell 结构 */
typedef struct CeCell {
    CeCellId     id;
    CeCellBounds bounds;
    CeCellState  state;

    /* 负载指标 */
    int          entity_count;    /* 当前实体数 */
    int          max_entities;    /* 分裂阈值（默认 150） */
    int          min_entities;    /* 合并阈值（默认 20） */

    /* 进程分配 */
    int          process_id;      /* 分配的进程 ID（-1 = 未分配） */

    /* 邻居 Cell（用于跨 Cell 实体迁移） */
    CeCellId     neighbors[8];    /* 最多 8 个邻居（3x3 网格 - 自己） */
    int          neighbor_count;

    /* AOI 实例（每个 Cell 有独立的 AOI） */
    void*        aoi;             /* 内部 AOI 系统句柄 */
} CeCell;

/** Cell 管理器 */
typedef struct CeCellManager {
    CeCell*  cells;               /* Cell 数组 */
    int      cell_count;          /* 当前 Cell 数量 */
    int      cell_capacity;       /* Cell 数组容量 */

    float    default_cell_width;  /* 默认 Cell 宽度（世界单位） */
    float    default_cell_height; /* 默认 Cell 高度 */

    float    world_min_x;         /* 世界边界 */
    float    world_min_y;
    float    world_max_x;
    float    world_max_y;

    int      next_cell_id;        /* 自增 Cell ID */
} CeCellManager;
```

### 5.3 接口定义：ce_cell.h

```c
/*
 * ChaosEngine Cell 大地图管理
 * 纯 C99，单线程安全
 */

#ifndef CE_CELL_H
#define CE_CELL_H

#include "server/ce_server_types.h"
#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 ---- */

/** 初始化 Cell 管理器 */
CeResult ce_cell_init(float world_width, float world_height,
                       float cell_width, float cell_height,
                       int max_entities_per_cell, int min_entities_per_cell);

/** 关闭 Cell 管理器 */
void ce_cell_shutdown(void);

/* ---- Cell 操作 ---- */

/** 根据坐标查找 Cell */
CeCellId ce_cell_find_by_position(float x, float y);

/** 获取 Cell 信息 */
const CeCell* ce_cell_get(CeCellId cell_id);

/** 获取 Cell 数量 */
int ce_cell_count(void);

/* ---- 实体管理 ---- */

/** 实体进入世界（自动分配到对应 Cell） */
CeResult ce_cell_enter_entity(CeServerEntityId entity_id, float x, float y, float radius);

/** 实体离开世界 */
void ce_cell_leave_entity(CeServerEntityId entity_id);

/** 实体移动（可能触发跨 Cell 迁移） */
CeResult ce_cell_move_entity(CeServerEntityId entity_id, float new_x, float new_y);

/* ---- 动态管理 ---- */

/** 检查并执行 Cell 分裂/合并（每帧调用） */
void ce_cell_update(void);

/** 手动分裂指定 Cell */
CeResult ce_cell_split(CeCellId cell_id);

/** 手动合并两个相邻 Cell */
CeResult ce_cell_merge(CeCellId cell_a, CeCellId cell_b);

/* ---- 进程分配 ---- */

/** 将 Cell 分配给指定进程 */
void ce_cell_assign_process(CeCellId cell_id, int process_id);

/** 获取 Cell 的进程 ID */
int ce_cell_get_process(CeCellId cell_id);

/* ---- 跨 Cell 查询 ---- */

/** 查询实体周围的实体（自动处理跨 Cell 边界） */
int ce_cell_query_nearby(CeServerEntityId entity_id,
                          CeServerEntityId* buffer, int max_count);

/* ---- 调试 ---- */

/** 打印 Cell 网格状态 */
void ce_cell_debug_print(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_CELL_H */
```

### 5.4 核心算法

#### 5.4.1 Cell 查找

```
ce_cell_find_by_position(x, y):
    cell_x = floor((x - world_min_x) / cell_width)
    cell_y = floor((y - world_min_y) / cell_height)
    cell_index = cell_y * cells_per_row + cell_x
    return cells[cell_index].id
```

#### 5.4.2 Cell 分裂

```
ce_cell_split(cell_id):
    前提: cell.entity_count > max_entities
    
    1. 将原 Cell 的边界四等分
    2. 创建 4 个新 Cell（或复用现有 Cell 槽位）
    3. 遍历原 Cell 的所有实体，重新分配到 4 个子 Cell
    4. 更新邻居关系（新 Cell 之间 + 与外部 Cell）
    5. 标记原 Cell 为 CE_CELL_MERGING（保留用于回滚）
    6. 触发 CE_AOI_LEAVE + CE_AOI_ENTER 事件（实体视野变化）
```

#### 5.4.3 Cell 合并

```
ce_cell_merge(cell_a, cell_b):
    前提: cell_a.entity_count + cell_b.entity_count < min_entities
          && cell_a 和 cell_b 相邻
    
    1. 创建合并后的 Cell（边界 = cell_a ∪ cell_b）
    2. 将两个 Cell 的所有实体迁移到新 Cell
    3. 更新邻居关系
    4. 释放旧 Cell
    5. 触发 AOI 事件
```

#### 5.4.4 跨 Cell 实体迁移

```
ce_cell_move_entity(entity_id, new_x, new_y):
    1. 计算新坐标对应的 Cell ID
    2. 如果 Cell ID 不变：
       a. 调用 ce_aoi_move() 更新位置
       b. 返回
    3. 如果 Cell ID 改变：
       a. 从旧 Cell 的 AOI 中移除实体（触发 CE_AOI_LEAVE）
       b. 将实体添加到新 Cell 的 AOI（触发 CE_AOI_ENTER）
       c. 更新实体的 cell_id
       d. 如果新 Cell 实体数超过阈值，标记待分裂
```

#### 5.4.5 跨 Cell 查询

```
ce_cell_query_nearby(entity_id, buffer, max):
    1. 获取实体所在 Cell
    2. 在本 Cell 内使用 AOI 查询
    3. 如果实体靠近 Cell 边界（距离边界 < aoi_radius）：
       a. 确定可能包含实体的邻居 Cell
       b. 在邻居 Cell 中查询边界附近的实体
    4. 合并结果，去重
    5. 返回
```

### 5.5 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `cell_width` | 100.0f | Cell 宽度（世界单位） |
| `cell_height` | 100.0f | Cell 高度（世界单位） |
| `max_entities_per_cell` | 150 | 超过此值触发分裂 |
| `min_entities_per_cell` | 20 | 低于此值触发合并 |
| `aoi_radius` | 50.0f | AOI 视野半径（世界单位） |

---

## 6. 模块依赖与接口

### 6.1 依赖关系

```
server/ce_aoi.c:
  └── core/ce_memory.h    (malloc/free)
  └── core/ce_math.h      (距离计算)
  └── public_api/ce_types.h

server/ce_cell.c:
  └── server/ce_aoi.h     (每个 Cell 内嵌 AOI)
  └── core/ce_memory.h
  └── core/ce_math.h
  └── public_api/ce_types.h

server/ce_server_types.h:
  └── public_api/ce_types.h
```

### 6.2 编译条件

```cmake
# src_c/server/CMakeLists.txt
if(CE_BUILD_SERVER)
    add_library(engine_server
        ce_aoi.c
        ce_cell.c
    )
    target_include_directories(engine_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src_c
    )
    target_link_libraries(engine_server PRIVATE engine_core)
endif()
```

### 6.3 与 ECS 的集成点

服务端实体通过 `CeServerEntityId` 与 ECS 的 `CeEntity` 对应：

```c
// 在 ce_server_main.c 中集成
void server_tick(float dt) {
    // 1. ECS 更新（移动系统等）
    ce_ecs_update(dt);

    // 2. 同步 ECS 位置到 AOI/Cell
    CE_QUERY_BEGIN(Transform, NetworkId);
        ce_cell_move_entity(net_id->id, transform->x, transform->z);
    CE_QUERY_END;

    // 3. Cell 动态管理（分裂/合并检查）
    ce_cell_update();

    // 4. 网络广播 AOI 事件
    // ...
}
```

---

## 7. 构建系统变更

### 7.1 新增 CMake 选项

```cmake
# 根 CMakeLists.txt
option(CE_BUILD_SERVER_CELL "Build server Cell/AOI subsystem" ON)
```

### 7.2 新增目录

```
src_c/server/
├── ce_aoi.h
├── ce_aoi.c
├── ce_cell.h
├── ce_cell.c
├── ce_server_types.h
└── CMakeLists.txt
```

### 7.3 编译目标变更

| 目标 | v0.1 | v0.2 |
|------|------|------|
| `chaos_server` | 链接 `engine_core` | 链接 `engine_core` + `engine_server` |
| `chaos_client` | 链接 `engine_core` | 不变 |
| `chaos_headless` | 链接 `engine_core` | 不变 |

---

## 8. 验收标准

### 8.1 Render 模块

- [ ] `ce_rhi_vulkan.h` 包含所有内部结构体（Device/Buffer/Texture/Shader/Pipeline）
- [ ] `rhi_create_texture` 能创建 VkImage + 分配内存 + 创建 ImageView
- [ ] `rhi_create_shader` 能从 SPIR-V 创建 VkShaderModule
- [ ] `rhi_create_pipeline` 能根据 CePipelineDesc 创建完整 VkPipeline
- [ ] `rhi_set_uniform_mat4` 能通过 Push Constants 传递矩阵
- [ ] 所有新增函数有对应的 destroy 函数
- [ ] `cmake --build .` 编译通过（Client/Server/Headless 三种模式）

### 8.2 AOI 十字链表

- [ ] `ce_aoi_enter` 正确插入 X/Y 轴链表
- [ ] `ce_aoi_move` 正确更新链表位置并触发事件
- [ ] `ce_aoi_leave` 正确移除并触发 CE_AOI_LEAVE 事件
- [ ] `ce_aoi_query_nearby` 返回 AOI 半径内的实体
- [ ] 1000 实体随机移动压力测试通过（无内存泄漏、无链表断裂）
- [ ] 单元测试覆盖：插入/删除/移动/查询/边界条件

### 8.3 Cell 管理

- [ ] `ce_cell_init` 正确创建 Cell 网格
- [ ] `ce_cell_find_by_position` 正确映射坐标到 Cell
- [ ] `ce_cell_enter_entity` 正确分配实体到 Cell
- [ ] `ce_cell_move_entity` 正确处理跨 Cell 迁移
- [ ] `ce_cell_split` 正确分裂 Cell（实体重新分配、邻居更新）
- [ ] `ce_cell_merge` 正确合并 Cell
- [ ] `ce_cell_query_nearby` 正确处理跨 Cell 边界查询
- [ ] 单元测试覆盖：Cell 查找/分裂/合并/跨 Cell 迁移

---

## 9. 实现路线图

```
Phase 1: Render 头文件补全（~2h）
  ├─ 1.1 提取内部结构体到 ce_rhi_vulkan.h
  ├─ 1.2 修改 ce_rhi_vulkan.c 使用新头文件
  ├─ 1.3 编译验证
  └─ 1.4 提交: [refactor](render): 提取 Vulkan 内部结构体到头文件

Phase 2: Render stub 实现（~4h）
  ├─ 2.1 rhi_create_texture / rhi_destroy_texture
  ├─ 2.2 rhi_create_shader / rhi_destroy_shader
  ├─ 2.3 rhi_create_pipeline / rhi_destroy_pipeline
  ├─ 2.4 rhi_set_uniform_mat4 (Push Constants)
  ├─ 2.5 编译验证
  └─ 2.6 提交: [feat](render): 补全纹理/着色器/管线/Uniform 实现

Phase 3: 服务端类型定义（~1h）
  ├─ 3.1 ce_server_types.h
  ├─ 3.2 CMakeLists.txt
  └─ 3.3 提交: [feat](server): 添加服务端类型定义和构建骨架

Phase 4: AOI 十字链表（~4h）
  ├─ 4.1 ce_aoi.h 接口
  ├─ 4.2 ce_aoi.c 实现（插入/删除/移动/查询）
  ├─ 4.3 单元测试 test_aoi.c
  ├─ 4.4 编译 + 测试通过
  └─ 4.5 提交: [feat](server): 实现 AOI 十字链表

Phase 5: Cell 管理（~4h）
  ├─ 5.1 ce_cell.h 接口
  ├─ 5.2 ce_cell.c 实现（查找/分裂/合并/迁移）
  ├─ 5.3 单元测试 test_cell.c
  ├─ 5.4 编译 + 测试通过
  └─ 5.5 提交: [feat](server): 实现 Cell 大地图管理

Phase 6: 集成 + 文档（~2h）
  ├─ 6.1 chaos_server 链接 engine_server
  ├─ 6.2 服务端主循环集成 AOI/Cell
  ├─ 6.3 更新 spec 文档
  └─ 6.4 提交: [feat](server): 集成 AOI/Cell 到服务端主循环
```

---

> **下一步：** 请审阅本 Spec，确认后按 Phase 顺序逐步实现 v0.2。
