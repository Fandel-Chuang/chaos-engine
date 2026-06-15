/*
 * ChaosEngine Vulkan RHI 内部头文件
 * 
 * 暴露 Vulkan 后端内部结构体，供 render 模块内部使用。
 * 外部模块应仅通过 ce_rhi.h 的不透明句柄访问。
 */

#ifndef CE_RHI_VULKAN_H
#define CE_RHI_VULKAN_H

#include "render/ce_rhi.h"
#include "core/ce_platform.h"
#include <vulkan/vulkan.h>

/* ---- 常量 ---- */

#define CE_VK_MAX_FRAMES_IN_FLIGHT 2
#define CE_VK_MAX_DESCRIPTOR_SETS  64

/* ---- 设备结构 ---- */

typedef struct CeRhiDevice {
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
    uint32_t         current_image_index;  /* 当前帧的交换链图像索引 */

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

/* ---- 缓冲结构 ---- */

typedef struct CeRhiBuffer {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    size_t         size;
} CeRhiBufferVk;

/* ---- 纹理结构 ---- */

typedef struct CeRhiTexture {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkFormat       format;
    int            width;
    int            height;
} CeRhiTextureVk;

/* ---- 着色器结构 ---- */

typedef struct CeRhiShader {
    VkShaderModule module;
    CeShaderStage  stage;
} CeRhiShaderVk;

/* ---- 管线结构 ---- */

typedef struct CeRhiPipeline {
    VkPipeline       pipeline;
    VkPipelineLayout layout;
} CeRhiPipelineVk;

/* ---- 内联类型转换（从不透明句柄到 Vulkan 内部结构） ---- */

static inline CeRhiDeviceVk* rhi_vk_device(CeRhiDevice* d) {
    return (CeRhiDeviceVk*)d;
}

static inline CeRhiBufferVk* rhi_vk_buffer(CeRhiBuffer* b) {
    return (CeRhiBufferVk*)b;
}

static inline CeRhiTextureVk* rhi_vk_texture(CeRhiTexture* t) {
    return (CeRhiTextureVk*)t;
}

static inline CeRhiShaderVk* rhi_vk_shader(CeRhiShader* s) {
    return (CeRhiShaderVk*)s;
}

static inline CeRhiPipelineVk* rhi_vk_pipeline(CeRhiPipeline* p) {
    return (CeRhiPipelineVk*)p;
}

/* ---- 内部辅助函数声明 ---- */

/** 查找合适的内存类型索引 */
uint32_t rhi_vk_find_memory_type(CeRhiDeviceVk* dev, uint32_t type_filter,
                                  VkMemoryPropertyFlags props);

/** 创建着色器模块（从 SPIR-V 二进制） */
VkShaderModule rhi_vk_create_shader_module(CeRhiDeviceVk* dev,
                                            const uint32_t* code, size_t size);

#endif /* CE_RHI_VULKAN_H */
