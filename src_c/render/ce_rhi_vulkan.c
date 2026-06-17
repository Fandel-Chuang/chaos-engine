/*
 * ChaosEngine Vulkan RHI 后端
 * 
 * 最小化 Vulkan 封装，提供：
 * - 实例/设备创建
 * - 交换链
 * - 渲染通道 + 帧缓冲
 * - 图形管线（顶点/片元着色器）
 * - 顶点/索引缓冲
 * - 命令缓冲录制
 * - 同步原语（信号量/围栏）
 */

#define VK_USE_PLATFORM_XLIB_KHR
#include "render/ce_rhi_vulkan.h"
#include "core/ce_memory.h"
#include "core/ce_platform.h"
#include "log/ce_log_internal.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 常量 ---- */

#define CE_VK_MAX_FRAMES_IN_FLIGHT 2
#define CE_VK_MAX_DESCRIPTOR_SETS  64

/* ---- 顶点着色器 (SPIR-V, 三角形, glslc 编译) ---- */
static const uint32_t g_vert_shader_spv[] = {
    0x07230203, 0x00010000, 0x000D000B, 0x00000036, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0008000F, 0x00000000, 0x00000004, 0x6E69616D, 0x00000000, 0x00000022, 0x00000026, 0x00000031,
    0x00030003, 0x00000002, 0x000001C2, 0x000A0004, 0x475F4C47, 0x4C474F4F, 0x70635F45, 0x74735F70,
    0x5F656C79, 0x656E696C, 0x7269645F, 0x69746365, 0x00006576, 0x00080004, 0x475F4C47, 0x4C474F4F,
    0x6E695F45, 0x64756C63, 0x69645F65, 0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6E69616D,
    0x00000000, 0x00050005, 0x0000000C, 0x69736F70, 0x6E6F6974, 0x00000073, 0x00040005, 0x00000017,
    0x6F6C6F63, 0x00007372, 0x00060005, 0x00000020, 0x505F6C67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x00000020, 0x00000000, 0x505F6C67, 0x7469736F, 0x006E6F69, 0x00070006, 0x00000020,
    0x00000001, 0x505F6C67, 0x746E696F, 0x657A6953, 0x00000000, 0x00070006, 0x00000020, 0x00000002,
    0x435F6C67, 0x4470696C, 0x61747369, 0x0065636E, 0x00070006, 0x00000020, 0x00000003, 0x435F6C67,
    0x446C6C75, 0x61747369, 0x0065636E, 0x00030005, 0x00000022, 0x00000000, 0x00060005, 0x00000026,
    0x565F6C67, 0x65747265, 0x646E4978, 0x00007865, 0x00050005, 0x00000031, 0x67617266, 0x6F6C6F43,
    0x00000072, 0x00030047, 0x00000020, 0x00000002, 0x00050048, 0x00000020, 0x00000000, 0x0000000B,
    0x00000000, 0x00050048, 0x00000020, 0x00000001, 0x0000000B, 0x00000001, 0x00050048, 0x00000020,
    0x00000002, 0x0000000B, 0x00000003, 0x00050048, 0x00000020, 0x00000003, 0x0000000B, 0x00000004,
    0x00040047, 0x00000026, 0x0000000B, 0x0000002A, 0x00040047, 0x00000031, 0x0000001E, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
    0x0004002B, 0x00000008, 0x00000009, 0x00000003, 0x0004001C, 0x0000000A, 0x00000007, 0x00000009,
    0x00040020, 0x0000000B, 0x00000006, 0x0000000A, 0x0004003B, 0x0000000B, 0x0000000C, 0x00000006,
    0x0004002B, 0x00000006, 0x0000000D, 0x00000000, 0x0004002B, 0x00000006, 0x0000000E, 0xBF000000,
    0x0005002C, 0x00000007, 0x0000000F, 0x0000000D, 0x0000000E, 0x0004002B, 0x00000006, 0x00000010,
    0x3F000000, 0x0005002C, 0x00000007, 0x00000011, 0x00000010, 0x00000010, 0x0005002C, 0x00000007,
    0x00000012, 0x0000000E, 0x00000010, 0x0006002C, 0x0000000A, 0x00000013, 0x0000000F, 0x00000011,
    0x00000012, 0x00040017, 0x00000014, 0x00000006, 0x00000003, 0x0004001C, 0x00000015, 0x00000014,
    0x00000009, 0x00040020, 0x00000016, 0x00000006, 0x00000015, 0x0004003B, 0x00000016, 0x00000017,
    0x00000006, 0x0004002B, 0x00000006, 0x00000018, 0x3F800000, 0x0006002C, 0x00000014, 0x00000019,
    0x00000018, 0x0000000D, 0x0000000D, 0x0006002C, 0x00000014, 0x0000001A, 0x0000000D, 0x00000018,
    0x0000000D, 0x0006002C, 0x00000014, 0x0000001B, 0x0000000D, 0x0000000D, 0x00000018, 0x0006002C,
    0x00000015, 0x0000001C, 0x00000019, 0x0000001A, 0x0000001B, 0x00040017, 0x0000001D, 0x00000006,
    0x00000004, 0x0004002B, 0x00000008, 0x0000001E, 0x00000001, 0x0004001C, 0x0000001F, 0x00000006,
    0x0000001E, 0x0006001E, 0x00000020, 0x0000001D, 0x00000006, 0x0000001F, 0x0000001F, 0x00040020,
    0x00000021, 0x00000003, 0x00000020, 0x0004003B, 0x00000021, 0x00000022, 0x00000003, 0x00040015,
    0x00000023, 0x00000020, 0x00000001, 0x0004002B, 0x00000023, 0x00000024, 0x00000000, 0x00040020,
    0x00000025, 0x00000001, 0x00000023, 0x0004003B, 0x00000025, 0x00000026, 0x00000001, 0x00040020,
    0x00000028, 0x00000006, 0x00000007, 0x00040020, 0x0000002E, 0x00000003, 0x0000001D, 0x00040020,
    0x00000030, 0x00000003, 0x00000014, 0x0004003B, 0x00000030, 0x00000031, 0x00000003, 0x00040020,
    0x00000033, 0x00000006, 0x00000014, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200F8, 0x00000005, 0x0003003E, 0x0000000C, 0x00000013, 0x0003003E, 0x00000017, 0x0000001C,
    0x0004003D, 0x00000023, 0x00000027, 0x00000026, 0x00050041, 0x00000028, 0x00000029, 0x0000000C,
    0x00000027, 0x0004003D, 0x00000007, 0x0000002A, 0x00000029, 0x00050051, 0x00000006, 0x0000002B,
    0x0000002A, 0x00000000, 0x00050051, 0x00000006, 0x0000002C, 0x0000002A, 0x00000001, 0x00070050,
    0x0000001D, 0x0000002D, 0x0000002B, 0x0000002C, 0x0000000D, 0x00000018, 0x00050041, 0x0000002E,
    0x0000002F, 0x00000022, 0x00000024, 0x0003003E, 0x0000002F, 0x0000002D, 0x0004003D, 0x00000023,
    0x00000032, 0x00000026, 0x00050041, 0x00000033, 0x00000034, 0x00000017, 0x00000032, 0x0004003D,
    0x00000014, 0x00000035, 0x00000034, 0x0003003E, 0x00000031, 0x00000035, 0x000100FD, 0x00010038
};

/* ---- 片元着色器 (SPIR-V, glslc 编译) ---- */
static const uint32_t g_frag_shader_spv[] = {
    0x07230203, 0x00010000, 0x000D000B, 0x00000013, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0007000F, 0x00000004, 0x00000004, 0x6E69616D, 0x00000000, 0x00000009, 0x0000000C, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001C2, 0x000A0004, 0x475F4C47, 0x4C474F4F,
    0x70635F45, 0x74735F70, 0x5F656C79, 0x656E696C, 0x7269645F, 0x69746365, 0x00006576, 0x00080004,
    0x475F4C47, 0x4C474F4F, 0x6E695F45, 0x64756C63, 0x69645F65, 0x74636572, 0x00657669, 0x00040005,
    0x00000004, 0x6E69616D, 0x00000000, 0x00050005, 0x00000009, 0x4374756F, 0x726F6C6F, 0x00000000,
    0x00050005, 0x0000000C, 0x67617266, 0x6F6C6F43, 0x00000072, 0x00040047, 0x00000009, 0x0000001E,
    0x00000000, 0x00040047, 0x0000000C, 0x0000001E, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003B, 0x00000008, 0x00000009,
    0x00000003, 0x00040017, 0x0000000A, 0x00000006, 0x00000003, 0x00040020, 0x0000000B, 0x00000001,
    0x0000000A, 0x0004003B, 0x0000000B, 0x0000000C, 0x00000001, 0x0004002B, 0x00000006, 0x0000000E,
    0x3F800000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200F8, 0x00000005,
    0x0004003D, 0x0000000A, 0x0000000D, 0x0000000C, 0x00050051, 0x00000006, 0x0000000F, 0x0000000D,
    0x00000000, 0x00050051, 0x00000006, 0x00000010, 0x0000000D, 0x00000001, 0x00050051, 0x00000006,
    0x00000011, 0x0000000D, 0x00000002, 0x00070050, 0x00000007, 0x00000012, 0x0000000F, 0x00000010,
    0x00000011, 0x0000000E, 0x0003003E, 0x00000009, 0x00000012, 0x000100FD, 0x00010038
};

/* 内部结构体定义已移至 ce_rhi_vulkan.h */

/* ---- 调试回调 ---- */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user_data)
{
    (void)type; (void)user_data;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        CE_LOG_ERROR("VULKAN", "%s", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        CE_LOG_WARN("VULKAN", "%s", data->pMessage);
    }
    return VK_FALSE;
}

/* ---- 辅助函数 ---- */

static VkResult create_debug_messenger(CeRhiDevice* dev) {
    VkDebugUtilsMessengerCreateInfoEXT info = {0};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debug_callback;

    PFN_vkCreateDebugUtilsMessengerEXT func =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            dev->instance, "vkCreateDebugUtilsMessengerEXT");
    if (func) {
        return func(dev->instance, &info, NULL, &dev->debug_messenger);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

uint32_t rhi_vk_find_memory_type(CeRhiDeviceVk* dev, uint32_t type_filter,
                                  VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(dev->physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

VkShaderModule rhi_vk_create_shader_module(CeRhiDeviceVk* dev,
                                            const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo info = {0};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode    = code;

    VkShaderModule module;
    vkCreateShaderModule(dev->device, &info, NULL, &module);
    return module;
}

/* ---- 前向声明 ---- */
static VkResult create_swapchain(CeRhiDevice* dev);
static VkResult create_render_pass(CeRhiDevice* dev);
static VkResult create_framebuffers(CeRhiDevice* dev);
static VkResult create_sync_objects(CeRhiDevice* dev);
static VkResult create_command_buffers(CeRhiDevice* dev);
static VkResult create_graphics_pipeline(CeRhiDevice* dev);

/* ---- 设备创建 ---- */

CeRhiDevice* rhi_create_device(const CeRhiConfig* config) {
    CeRhiDevice* dev = (CeRhiDevice*)calloc(1, sizeof(CeRhiDevice));
    dev->width  = config->width;
    dev->height = config->height;

    /* 1. 创建实例 */
    VkApplicationInfo app_info = {0};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = config->title ? config->title : "ChaosEngine";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName        = "ChaosEngine";
    app_info.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion         = VK_API_VERSION_1_2;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    const char* layers[] = {
        /* "VK_LAYER_KHRONOS_validation", -- disabled for NVIDIA compat */
    };

    VkInstanceCreateInfo instance_info = {0};
    instance_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo        = &app_info;
    instance_info.enabledExtensionCount   = 3;
    instance_info.ppEnabledExtensionNames = extensions;
    instance_info.enabledLayerCount       = 0;
    instance_info.ppEnabledLayerNames     = layers;

    VkResult result = vkCreateInstance(&instance_info, NULL, &dev->instance);
    if (result == VK_ERROR_LAYER_NOT_PRESENT) {
        /* 无验证层，重试 */
        instance_info.enabledLayerCount = 0;
        result = vkCreateInstance(&instance_info, NULL, &dev->instance);
    }
    if (result != VK_SUCCESS) {
        CE_LOG_ERROR("VULKAN", "Failed to create instance: %d", result);
        free(dev);
        return NULL;
    }

    create_debug_messenger(dev);

    /* 1.5. 创建 X11 窗口 */
    dev->x11_display = XOpenDisplay(NULL);
    if (!dev->x11_display) {
        CE_LOG_ERROR("VULKAN", "Failed to open X11 display");
        rhi_destroy_device(dev);
        return NULL;
    }

    int screen = DefaultScreen(dev->x11_display);
    dev->x11_window = XCreateSimpleWindow(
        dev->x11_display,
        RootWindow(dev->x11_display, screen),
        0, 0, dev->width, dev->height, 1,
        BlackPixel(dev->x11_display, screen),
        WhitePixel(dev->x11_display, screen)
    );

    XStoreName(dev->x11_display, dev->x11_window,
               config->title ? config->title : "ChaosEngine");
    XSelectInput(dev->x11_display, dev->x11_window,
                 ExposureMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                 StructureNotifyMask);

    /* 设置关闭协议 */
    Atom wm_delete = XInternAtom(dev->x11_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dev->x11_display, dev->x11_window, &wm_delete, 1);

    XMapWindow(dev->x11_display, dev->x11_window);
    XFlush(dev->x11_display);

    /* 1.6. 创建 Vulkan Surface */
    VkXlibSurfaceCreateInfoKHR surface_info = {0};
    surface_info.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    surface_info.dpy    = dev->x11_display;
    surface_info.window = dev->x11_window;

    if (vkCreateXlibSurfaceKHR(dev->instance, &surface_info, NULL,
                                &dev->surface) != VK_SUCCESS) {
        CE_LOG_ERROR("VULKAN", "Failed to create Xlib surface");
        rhi_destroy_device(dev);
        return NULL;
    }

    CE_LOG_INFO("VULKAN", "X11 window %dx%d created", dev->width, dev->height);

    /* 2. 选择物理设备 */
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(dev->instance, &device_count, NULL);
    if (device_count == 0) {
        CE_LOG_ERROR("VULKAN", "No Vulkan-capable GPU found");
        rhi_destroy_device(dev);
        return NULL;
    }

    VkPhysicalDevice* devices = (VkPhysicalDevice*)malloc(
        sizeof(VkPhysicalDevice) * device_count);
    vkEnumeratePhysicalDevices(dev->instance, &device_count, devices);
    dev->physical_device = devices[0];  /* 选第一个 */
    free(devices);

    /* 3. 查找队列族 */
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        dev->physical_device, &queue_family_count, NULL);
    VkQueueFamilyProperties* queue_families = (VkQueueFamilyProperties*)malloc(
        sizeof(VkQueueFamilyProperties) * queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        dev->physical_device, &queue_family_count, queue_families);

    dev->graphics_family = UINT32_MAX;
    dev->present_family  = UINT32_MAX;

    for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            dev->graphics_family = i;
        }
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(
            dev->physical_device, i, dev->surface, &present_support);
        if (present_support) {
            dev->present_family = i;
        }
        if (dev->graphics_family != UINT32_MAX &&
            dev->present_family != UINT32_MAX) break;
    }
    free(queue_families);

    /* 4. 创建逻辑设备 */
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_infos[2];
    uint32_t queue_info_count = 0;

    VkDeviceQueueCreateInfo gfx_queue = {0};
    gfx_queue.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    gfx_queue.queueFamilyIndex = dev->graphics_family;
    gfx_queue.queueCount       = 1;
    gfx_queue.pQueuePriorities = &queue_priority;
    queue_infos[queue_info_count++] = gfx_queue;

    if (dev->graphics_family != dev->present_family) {
        VkDeviceQueueCreateInfo pres_queue = {0};
        pres_queue.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        pres_queue.queueFamilyIndex = dev->present_family;
        pres_queue.queueCount       = 1;
        pres_queue.pQueuePriorities = &queue_priority;
        queue_infos[queue_info_count++] = pres_queue;
    }

    const char* device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkPhysicalDeviceFeatures device_features = {0};

    VkDeviceCreateInfo device_info = {0};
    device_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount    = queue_info_count;
    device_info.pQueueCreateInfos       = queue_infos;
    device_info.enabledExtensionCount   = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    device_info.pEnabledFeatures        = &device_features;

    if (vkCreateDevice(dev->physical_device, &device_info, NULL,
                       &dev->device) != VK_SUCCESS) {
        CE_LOG_ERROR("VULKAN", "Failed to create logical device");
        rhi_destroy_device(dev);
        return NULL;
    }

    vkGetDeviceQueue(dev->device, dev->graphics_family, 0, &dev->graphics_queue);
    vkGetDeviceQueue(dev->device, dev->present_family, 0, &dev->present_queue);

    /* 5. 创建命令池 */
    VkCommandPoolCreateInfo pool_info = {0};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = dev->graphics_family;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(dev->device, &pool_info, NULL, &dev->command_pool);

    /* 6. 创建交换链和渲染资源 */
    create_swapchain(dev);
    create_render_pass(dev);
    create_framebuffers(dev);
    create_sync_objects(dev);
    create_command_buffers(dev);
    if (create_graphics_pipeline(dev) != VK_SUCCESS) {
        fprintf(stderr, "VULKAN ERROR: Failed to create graphics pipeline\n");
        rhi_destroy_device(dev);
        return NULL;
    }

    CE_LOG_INFO("VULKAN", "Vulkan device created successfully");
    return dev;
}

void rhi_destroy_device(CeRhiDevice* dev) {
    if (!dev) return;

    vkDeviceWaitIdle(dev->device);

    /* 清理交换链 */
    if (dev->swapchain) {
        for (uint32_t i = 0; i < dev->image_count; i++) {
            if (dev->framebuffers) vkDestroyFramebuffer(dev->device, dev->framebuffers[i], NULL);
            if (dev->swapchain_views) vkDestroyImageView(dev->device, dev->swapchain_views[i], NULL);
        }
        free(dev->swapchain_images);
        free(dev->swapchain_views);
        free(dev->framebuffers);
        free(dev->command_buffers);
        free(dev->image_available);
        free(dev->render_finished);
        free(dev->in_flight_fences);

        vkDestroySwapchainKHR(dev->device, dev->swapchain, NULL);
    }

    if (dev->graphics_pipeline) vkDestroyPipeline(dev->device, dev->graphics_pipeline, NULL);
    if (dev->pipeline_layout) vkDestroyPipelineLayout(dev->device, dev->pipeline_layout, NULL);
    if (dev->render_pass) vkDestroyRenderPass(dev->device, dev->render_pass, NULL);
    if (dev->command_pool) vkDestroyCommandPool(dev->device, dev->command_pool, NULL);

    if (dev->debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT func =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                dev->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) func(dev->instance, dev->debug_messenger, NULL);
    }

    if (dev->device) vkDestroyDevice(dev->device, NULL);
    if (dev->surface) vkDestroySurfaceKHR(dev->instance, dev->surface, NULL);
    if (dev->instance) vkDestroyInstance(dev->instance, NULL);

    /* 清理 X11 窗口 */
    if (dev->x11_window) XDestroyWindow(dev->x11_display, dev->x11_window);
    if (dev->x11_display) XCloseDisplay(dev->x11_display);

    free(dev);
}

/* ---- 交换链 ---- */

static VkResult create_swapchain(CeRhiDevice* dev) {
    /* 查询表面能力 */
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        dev->physical_device, dev->surface, &caps);

    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        dev->physical_device, dev->surface, &format_count, NULL);
    VkSurfaceFormatKHR* formats = (VkSurfaceFormatKHR*)malloc(
        sizeof(VkSurfaceFormatKHR) * format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        dev->physical_device, dev->surface, &format_count, formats);

    VkSurfaceFormatKHR surface_format = formats[0];
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = formats[i];
            break;
        }
    }
    free(formats);

    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        dev->physical_device, dev->surface, &present_mode_count, NULL);
    VkPresentModeKHR* present_modes = (VkPresentModeKHR*)malloc(
        sizeof(VkPresentModeKHR) * present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        dev->physical_device, dev->surface, &present_mode_count, present_modes);

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    /* 优先选择不阻塞的呈现模式 */
    for (uint32_t i = 0; i < present_mode_count; i++) {
        if (present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
    }
    if (present_mode == VK_PRESENT_MODE_FIFO_KHR) {
        for (uint32_t i = 0; i < present_mode_count; i++) {
            if (present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                break;
            }
        }
    }
    free(present_modes);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width  = dev->width;
        extent.height = dev->height;
    }

    dev->image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && dev->image_count > caps.maxImageCount) {
        dev->image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swap_info = {0};
    swap_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swap_info.surface          = dev->surface;
    swap_info.minImageCount    = dev->image_count;
    swap_info.imageFormat      = surface_format.format;
    swap_info.imageColorSpace  = surface_format.colorSpace;
    swap_info.imageExtent      = extent;
    swap_info.imageArrayLayers = 1;
    swap_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swap_info.preTransform     = caps.currentTransform;
    swap_info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swap_info.presentMode      = present_mode;
    swap_info.clipped          = VK_TRUE;

    uint32_t families[] = {dev->graphics_family, dev->present_family};
    if (dev->graphics_family != dev->present_family) {
        swap_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        swap_info.queueFamilyIndexCount = 2;
        swap_info.pQueueFamilyIndices   = families;
    } else {
        swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (vkCreateSwapchainKHR(dev->device, &swap_info, NULL,
                             &dev->swapchain) != VK_SUCCESS) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    dev->swapchain_format = surface_format.format;
    dev->swapchain_extent = extent;

    /* 获取交换链图像 */
    vkGetSwapchainImagesKHR(dev->device, dev->swapchain,
                            &dev->image_count, NULL);
    dev->swapchain_images = (VkImage*)malloc(sizeof(VkImage) * dev->image_count);
    vkGetSwapchainImagesKHR(dev->device, dev->swapchain,
                            &dev->image_count, dev->swapchain_images);

    /* 创建图像视图 */
    dev->swapchain_views = (VkImageView*)malloc(
        sizeof(VkImageView) * dev->image_count);
    for (uint32_t i = 0; i < dev->image_count; i++) {
        VkImageViewCreateInfo view_info = {0};
        view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                           = dev->swapchain_images[i];
        view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format                          = dev->swapchain_format;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 1;
        vkCreateImageView(dev->device, &view_info, NULL, &dev->swapchain_views[i]);
    }

    return VK_SUCCESS;
}

static VkResult create_render_pass(CeRhiDevice* dev) {
    VkAttachmentDescription color_attachment = {0};
    color_attachment.format         = dev->swapchain_format;
    color_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref = {0};
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

    VkSubpassDependency dependency = {0};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo render_pass_info = {0};
    render_pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments    = &color_attachment;
    render_pass_info.subpassCount    = 1;
    render_pass_info.pSubpasses      = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies   = &dependency;

    return vkCreateRenderPass(dev->device, &render_pass_info, NULL,
                              &dev->render_pass);
}

static VkResult create_framebuffers(CeRhiDevice* dev) {
    dev->framebuffers = (VkFramebuffer*)malloc(
        sizeof(VkFramebuffer) * dev->image_count);

    for (uint32_t i = 0; i < dev->image_count; i++) {
        VkImageView attachments[] = {dev->swapchain_views[i]};

        VkFramebufferCreateInfo fb_info = {0};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = dev->render_pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments    = attachments;
        fb_info.width           = dev->swapchain_extent.width;
        fb_info.height          = dev->swapchain_extent.height;
        fb_info.layers          = 1;

        if (vkCreateFramebuffer(dev->device, &fb_info, NULL,
                                &dev->framebuffers[i]) != VK_SUCCESS) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return VK_SUCCESS;
}

static VkResult create_graphics_pipeline(CeRhiDevice* dev) {
    VkShaderModule vert_module = rhi_vk_create_shader_module(
        dev, g_vert_shader_spv, sizeof(g_vert_shader_spv));
    VkShaderModule frag_module = rhi_vk_create_shader_module(
        dev, g_frag_shader_spv, sizeof(g_frag_shader_spv));

    VkPipelineShaderStageCreateInfo vert_stage = {0};
    vert_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName  = "main";

    VkPipelineShaderStageCreateInfo frag_stage = {0};
    frag_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName  = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

    /* 顶点输入（空，三角形硬编码在着色器中） */
    VkPipelineVertexInputStateCreateInfo vertex_input = {0};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 0;
    vertex_input.vertexAttributeDescriptionCount = 0;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {0};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport = {
        0, 0,
        (float)dev->swapchain_extent.width,
        (float)dev->swapchain_extent.height,
        0.0f, 1.0f
    };
    VkRect2D scissor = {{0, 0}, dev->swapchain_extent};

    VkPipelineViewportStateCreateInfo viewport_state = {0};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports    = &viewport;
    viewport_state.scissorCount  = 1;
    viewport_state.pScissors     = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {0};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;  /* 调试：禁用剔除 */
    rasterizer.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {0};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState color_blend = {0};
    color_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                 VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT |
                                 VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending = {0};
    color_blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments    = &color_blend;

    VkPipelineLayoutCreateInfo pipeline_layout_info = {0};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    vkCreatePipelineLayout(dev->device, &pipeline_layout_info, NULL,
                           &dev->pipeline_layout);

    VkGraphicsPipelineCreateInfo pipeline_info = {0};
    pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount          = 2;
    pipeline_info.pStages             = stages;
    pipeline_info.pVertexInputState   = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState      = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState   = &multisampling;
    pipeline_info.pColorBlendState    = &color_blending;
    pipeline_info.layout              = dev->pipeline_layout;
    pipeline_info.renderPass          = dev->render_pass;
    pipeline_info.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE,
                                                 1, &pipeline_info, NULL,
                                                 &dev->graphics_pipeline);

    vkDestroyShaderModule(dev->device, vert_module, NULL);
    vkDestroyShaderModule(dev->device, frag_module, NULL);

    if (result != VK_SUCCESS) {
        fprintf(stderr, "VULKAN ERROR: create_graphics_pipeline failed: %d\n", result);
    }
    return result;
}

static VkResult create_sync_objects(CeRhiDevice* dev) {
    dev->image_available  = (VkSemaphore*)malloc(sizeof(VkSemaphore) * CE_VK_MAX_FRAMES_IN_FLIGHT);
    dev->render_finished  = (VkSemaphore*)malloc(sizeof(VkSemaphore) * CE_VK_MAX_FRAMES_IN_FLIGHT);
    dev->in_flight_fences = (VkFence*)malloc(sizeof(VkFence) * CE_VK_MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo sem_info = {0};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info = {0};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < CE_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(dev->device, &sem_info, NULL,
                              &dev->image_available[i]) != VK_SUCCESS ||
            vkCreateSemaphore(dev->device, &sem_info, NULL,
                              &dev->render_finished[i]) != VK_SUCCESS ||
            vkCreateFence(dev->device, &fence_info, NULL,
                          &dev->in_flight_fences[i]) != VK_SUCCESS) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return VK_SUCCESS;
}

static VkResult create_command_buffers(CeRhiDevice* dev) {
    dev->command_buffers = (VkCommandBuffer*)malloc(
        sizeof(VkCommandBuffer) * CE_VK_MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo alloc_info = {0};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool        = dev->command_pool;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = CE_VK_MAX_FRAMES_IN_FLIGHT;

    return vkAllocateCommandBuffers(dev->device, &alloc_info, dev->command_buffers);
}

/* ---- 公开 API ---- */

void rhi_resize(CeRhiDevice* dev, int width, int height) {
    dev->width  = width;
    dev->height = height;
    /* 重建交换链 */
    vkDeviceWaitIdle(dev->device);
    /* 简化处理：销毁旧资源 */
    for (uint32_t i = 0; i < dev->image_count; i++) {
        vkDestroyFramebuffer(dev->device, dev->framebuffers[i], NULL);
        vkDestroyImageView(dev->device, dev->swapchain_views[i], NULL);
    }
    free(dev->swapchain_images);
    free(dev->swapchain_views);
    free(dev->framebuffers);
    vkDestroySwapchainKHR(dev->device, dev->swapchain, NULL);

    create_swapchain(dev);
    create_render_pass(dev);
    create_framebuffers(dev);
}

CeBool rhi_should_close(CeRhiDevice* dev) {
    return dev->should_close;
}

void rhi_poll_events(CeRhiDevice* dev) {
    /* 处理 X11 事件 */
    if (dev->x11_display) {
        XEvent event;
        while (XPending(dev->x11_display)) {
            XNextEvent(dev->x11_display, &event);
            switch (event.type) {
                case ClientMessage:
                    if ((Atom)event.xclient.data.l[0] ==
                        XInternAtom(dev->x11_display, "WM_DELETE_WINDOW", False)) {
                        dev->should_close = CE_TRUE;
                    }
                    break;
                case DestroyNotify:
                    dev->should_close = CE_TRUE;
                    break;
                case KeyPress:
                    if (event.xkey.keycode == 9) { /* Escape */
                        dev->should_close = CE_TRUE;
                    }
                    break;
            }
        }
    }
    ce_window_poll_events();
    if (dev->window) {
        dev->should_close = ce_window_should_close(dev->window);
    }
}

/* ---- 资源创建 ---- */

CeRhiBuffer* rhi_create_buffer(CeRhiDevice* dev, const CeBufferDesc* desc) {
    CeRhiBuffer* buf = (CeRhiBuffer*)calloc(1, sizeof(CeRhiBuffer));
    buf->size = desc->size;

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (desc->type == CE_BUFFER_VERTEX) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (desc->type == CE_BUFFER_INDEX)  usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

    VkBufferCreateInfo buffer_info = {0};
    buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size        = desc->size;
    buffer_info.usage       = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(dev->device, &buffer_info, NULL, &buf->buffer) != VK_SUCCESS) {
        free(buf);
        return NULL;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(dev->device, buf->buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {0};
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_reqs.size;
    alloc_info.memoryTypeIndex = rhi_vk_find_memory_type(dev, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(dev->device, &alloc_info, NULL, &buf->memory);
    vkBindBufferMemory(dev->device, buf->buffer, buf->memory, 0);

    /* 上传数据 */
    if (desc->data) {
        void* mapped;
        vkMapMemory(dev->device, buf->memory, 0, desc->size, 0, &mapped);
        memcpy(mapped, desc->data, desc->size);
        vkUnmapMemory(dev->device, buf->memory);
    }

    return buf;
}

void rhi_destroy_buffer(CeRhiDevice* dev, CeRhiBuffer* buffer) {
    if (!buffer) return;
    vkDestroyBuffer(dev->device, buffer->buffer, NULL);
    vkFreeMemory(dev->device, buffer->memory, NULL);
    free(buffer);
}

CeRhiTexture* rhi_create_texture(CeRhiDevice* dev, const CeTextureDesc* desc) {
    CeRhiTextureVk* tex = (CeRhiTextureVk*)calloc(1, sizeof(CeRhiTextureVk));
    if (!tex) return NULL;

    /* 格式映射 */
    VkFormat vk_format = VK_FORMAT_R8G8B8A8_SRGB;
    switch (desc->format) {
        case CE_TEX_RGBA8:  vk_format = VK_FORMAT_R8G8B8A8_SRGB; break;
        case CE_TEX_RGB8:   vk_format = VK_FORMAT_R8G8B8_SRGB;   break;
        case CE_TEX_DEPTH24: vk_format = VK_FORMAT_D32_SFLOAT;   break;
    }
    tex->format = vk_format;
    tex->width  = desc->width;
    tex->height = desc->height;

    /* 1. 创建 VkImage */
    VkImageCreateInfo image_info = {0};
    image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType     = VK_IMAGE_TYPE_2D;
    image_info.format        = vk_format;
    image_info.extent.width  = desc->width;
    image_info.extent.height = desc->height;
    image_info.extent.depth  = 1;
    image_info.mipLevels     = 1;
    image_info.arrayLayers   = 1;
    image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(dev->device, &image_info, NULL, &tex->image) != VK_SUCCESS) {
        free(tex);
        return NULL;
    }

    /* 2. 分配内存 */
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(dev->device, tex->image, &mem_req);

    VkMemoryAllocateInfo alloc_info = {0};
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = mem_req.size;
    alloc_info.memoryTypeIndex = rhi_vk_find_memory_type(dev,
        mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(dev->device, &alloc_info, NULL, &tex->memory) != VK_SUCCESS) {
        vkDestroyImage(dev->device, tex->image, NULL);
        free(tex);
        return NULL;
    }
    vkBindImageMemory(dev->device, tex->image, tex->memory, 0);

    /* 3. 创建 ImageView */
    VkImageViewCreateInfo view_info = {0};
    view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image                           = tex->image;
    view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format                          = vk_format;
    view_info.subresourceRange.aspectMask     = (desc->format == CE_TEX_DEPTH24)
        ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel   = 0;
    view_info.subresourceRange.levelCount     = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(dev->device, &view_info, NULL, &tex->view) != VK_SUCCESS) {
        vkFreeMemory(dev->device, tex->memory, NULL);
        vkDestroyImage(dev->device, tex->image, NULL);
        free(tex);
        return NULL;
    }

    CE_LOG_INFO("VULKAN", "Texture created: %dx%d", desc->width, desc->height);
    return (CeRhiTexture*)tex;
}

void rhi_destroy_texture(CeRhiDevice* dev, CeRhiTexture* texture) {
    if (!texture) return;
    CeRhiTextureVk* tex = (CeRhiTextureVk*)texture;
    if (tex->view)   vkDestroyImageView(dev->device, tex->view, NULL);
    if (tex->image)  vkDestroyImage(dev->device, tex->image, NULL);
    if (tex->memory) vkFreeMemory(dev->device, tex->memory, NULL);
    free(tex);
}

CeRhiShader* rhi_create_shader(CeRhiDevice* dev, CeShaderStage stage,
                                const char* source) {
    /* source 是 SPIR-V 二进制数据，前 4 字节为大小（uint32_t） */
    CeRhiShaderVk* shader = (CeRhiShaderVk*)calloc(1, sizeof(CeRhiShaderVk));
    if (!shader) return NULL;
    shader->stage = stage;

    const uint32_t* spirv = (const uint32_t*)(source + sizeof(uint32_t));
    size_t spirv_size = *(const uint32_t*)source;

    VkShaderModuleCreateInfo info = {0};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv_size;
    info.pCode    = spirv;

    if (vkCreateShaderModule(dev->device, &info, NULL, &shader->module) != VK_SUCCESS) {
        free(shader);
        return NULL;
    }

    CE_LOG_INFO("VULKAN", "Shader created: stage=%d", stage);
    return (CeRhiShader*)shader;
}

void rhi_destroy_shader(CeRhiDevice* dev, CeRhiShader* shader) {
    if (!shader) return;
    CeRhiShaderVk* s = (CeRhiShaderVk*)shader;
    if (s->module) vkDestroyShaderModule(dev->device, s->module, NULL);
    free(s);
}

CeRhiPipeline* rhi_create_pipeline(CeRhiDevice* dev, const CePipelineDesc* desc) {
    if (!desc || !desc->vertex_shader || !desc->fragment_shader) return NULL;

    CeRhiPipelineVk* pipeline = (CeRhiPipelineVk*)calloc(1, sizeof(CeRhiPipelineVk));
    if (!pipeline) return NULL;

    CeRhiShaderVk* vs = (CeRhiShaderVk*)desc->vertex_shader;
    CeRhiShaderVk* fs = (CeRhiShaderVk*)desc->fragment_shader;

    /* 着色器阶段 */
    VkPipelineShaderStageCreateInfo stages[2];
    stages[0] = (VkPipelineShaderStageCreateInfo){0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->module;
    stages[0].pName  = "main";

    stages[1] = (VkPipelineShaderStageCreateInfo){0};
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs->module;
    stages[1].pName  = "main";

    /* 顶点输入布局 */
    VkVertexInputBindingDescription binding = {
        .binding   = 0,
        .stride    = desc->vertex_stride,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription* attrs = NULL;
    if (desc->attr_count > 0) {
        attrs = (VkVertexInputAttributeDescription*)malloc(
            sizeof(VkVertexInputAttributeDescription) * desc->attr_count);
        for (uint32_t i = 0; i < desc->attr_count; i++) {
            attrs[i] = (VkVertexInputAttributeDescription){
                .location = i,
                .binding  = 0,
                .format   = VK_FORMAT_R32G32B32_SFLOAT,
                .offset   = i * sizeof(float) * 3
            };
        }
    }

    VkPipelineVertexInputStateCreateInfo vertex_input = {0};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &binding;
    vertex_input.vertexAttributeDescriptionCount = desc->attr_count;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    /* 输入装配 */
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {0};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* 视口/裁剪 */
    VkViewport viewport = {0, 0, (float)dev->width, (float)dev->height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {dev->width, dev->height}};

    VkPipelineViewportStateCreateInfo viewport_state = {0};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports    = &viewport;
    viewport_state.scissorCount  = 1;
    viewport_state.pScissors     = &scissor;

    /* 光栅化 */
    VkPipelineRasterizationStateCreateInfo rasterizer = {0};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    /* 多重采样 */
    VkPipelineMultisampleStateCreateInfo multisampling = {0};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* 颜色混合 */
    VkPipelineColorBlendAttachmentState color_blend = {0};
    color_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                 VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT |
                                 VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending = {0};
    color_blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments    = &color_blend;

    /* Push Constant 范围（用于 Uniform） */
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(CeMat4)
    };

    /* 管线布局 */
    VkPipelineLayoutCreateInfo layout_info = {0};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(dev->device, &layout_info, NULL,
                               &pipeline->layout) != VK_SUCCESS) {
        free(attrs);
        free(pipeline);
        return NULL;
    }

    /* 创建管线 */
    VkGraphicsPipelineCreateInfo pipeline_info = {0};
    pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount          = 2;
    pipeline_info.pStages             = stages;
    pipeline_info.pVertexInputState   = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState      = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState   = &multisampling;
    pipeline_info.pColorBlendState    = &color_blending;
    pipeline_info.layout              = pipeline->layout;
    pipeline_info.renderPass          = dev->render_pass;
    pipeline_info.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE,
                                                 1, &pipeline_info, NULL,
                                                 &pipeline->pipeline);
    free(attrs);

    if (result != VK_SUCCESS) {
        vkDestroyPipelineLayout(dev->device, pipeline->layout, NULL);
        free(pipeline);
        return NULL;
    }

    CE_LOG_INFO("VULKAN", "Pipeline created");
    return (CeRhiPipeline*)pipeline;
}

void rhi_destroy_pipeline(CeRhiDevice* dev, CeRhiPipeline* pipeline) {
    if (!pipeline) return;
    CeRhiPipelineVk* p = (CeRhiPipelineVk*)pipeline;
    if (p->pipeline) vkDestroyPipeline(dev->device, p->pipeline, NULL);
    if (p->layout)   vkDestroyPipelineLayout(dev->device, p->layout, NULL);
    free(p);
}

/* ---- 渲染命令 ---- */

void rhi_begin_frame(CeRhiDevice* dev, CeColor clear_color) {
    uint32_t frame = dev->current_frame;

    vkWaitForFences(dev->device, 1, &dev->in_flight_fences[frame], VK_TRUE, UINT64_MAX);

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(dev->device, dev->swapchain,
        UINT64_MAX, dev->image_available[frame], VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        rhi_resize(dev, dev->width, dev->height);
        dev->frame_valid = CE_FALSE;
        return;
    }
    if (result != VK_SUCCESS) {
        dev->frame_valid = CE_FALSE;
        return;
    }

    dev->frame_valid = CE_TRUE;
    vkResetFences(dev->device, 1, &dev->in_flight_fences[frame]);

    VkCommandBuffer cmd = dev->command_buffers[frame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {0};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin_info);

    VkClearValue clear_value = {{
        clear_color.r, clear_color.g, clear_color.b, clear_color.a
    }};

    VkRenderPassBeginInfo rp_info = {0};
    rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass        = dev->render_pass;
    rp_info.framebuffer       = dev->framebuffers[image_index];
    rp_info.renderArea.offset = (VkOffset2D){0, 0};
    rp_info.renderArea.extent = dev->swapchain_extent;
    rp_info.clearValueCount   = 1;
    rp_info.pClearValues      = &clear_value;

    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    if (dev->graphics_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->graphics_pipeline);
    }

    dev->current_image_index = image_index;
}

void rhi_bind_pipeline(CeRhiDevice* dev, CeRhiPipeline* pipeline) {
    if (!pipeline) return;
    CeRhiPipelineVk* p = (CeRhiPipelineVk*)pipeline;
    VkCommandBuffer cmd = dev->command_buffers[dev->current_frame];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
}

void rhi_bind_buffer(CeRhiDevice* dev, CeRhiBuffer* buffer, uint32_t slot) {
    if (!buffer) return;
    CeRhiBufferVk* buf = (CeRhiBufferVk*)buffer;
    VkCommandBuffer cmd = dev->command_buffers[dev->current_frame];
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, slot, 1, &buf->buffer, offsets);
}

void rhi_set_uniform_mat4(CeRhiDevice* dev, const char* name, const CeMat4* mat) {
    (void)name;
    VkCommandBuffer cmd = dev->command_buffers[dev->current_frame];
    vkCmdPushConstants(cmd, dev->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(CeMat4), mat);
}

void rhi_draw(CeRhiDevice* dev, uint32_t vertex_count, uint32_t first_vertex) {
    if (!dev->frame_valid) return;
    VkCommandBuffer cmd = dev->command_buffers[dev->current_frame];
    vkCmdDraw(cmd, vertex_count, 1, first_vertex, 0);
}

void rhi_draw_indexed(CeRhiDevice* dev, uint32_t index_count, uint32_t first_index) {
    if (!dev->frame_valid) return;
    VkCommandBuffer cmd = dev->command_buffers[dev->current_frame];
    vkCmdDrawIndexed(cmd, index_count, 1, first_index, 0, 0);
}

void rhi_end_frame(CeRhiDevice* dev) {
    if (!dev->frame_valid) return;
    VkCommandBuffer cmd = dev->command_buffers[dev->current_frame];
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

void rhi_present(CeRhiDevice* dev) {
    if (!dev->frame_valid) {
        dev->current_frame = (dev->current_frame + 1) % CE_VK_MAX_FRAMES_IN_FLIGHT;
        return;
    }
    uint32_t frame = dev->current_frame;
    uint32_t image_index = dev->current_image_index;
    VkCommandBuffer cmd = dev->command_buffers[frame];

    VkSubmitInfo submit_info = {0};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore wait_semaphores[] = {dev->image_available[frame]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores    = wait_semaphores;
    submit_info.pWaitDstStageMask  = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers    = &cmd;

    VkSemaphore signal_semaphores[] = {dev->render_finished[frame]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores    = signal_semaphores;

    vkQueueSubmit(dev->graphics_queue, 1, &submit_info,
                  dev->in_flight_fences[frame]);

    VkPresentInfoKHR present_info = {0};
    present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = signal_semaphores;

    VkSwapchainKHR swapchains[] = {dev->swapchain};
    present_info.swapchainCount = 1;
    present_info.pSwapchains    = swapchains;
    present_info.pImageIndices  = &image_index;

    vkQueuePresentKHR(dev->present_queue, &present_info);

    dev->current_frame = (dev->current_frame + 1) % CE_VK_MAX_FRAMES_IN_FLIGHT;
}
