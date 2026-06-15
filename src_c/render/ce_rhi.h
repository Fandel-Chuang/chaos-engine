/*
 * ChaosEngine 渲染抽象层 (RHI) 接口定义
 * 纯 C，最小接口集
 */

#ifndef CE_RHI_H
#define CE_RHI_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 枚举 ---- */

typedef enum CeRhiBackend {
    CE_RHI_OPENGL = 0,
    CE_RHI_VULKAN,
    CE_RHI_METAL,
    CE_RHI_NONE  /* 无头模式 */
} CeRhiBackend;

typedef enum CeShaderStage {
    CE_SHADER_VERTEX = 0,
    CE_SHADER_FRAGMENT
} CeShaderStage;

typedef enum CeBufferType {
    CE_BUFFER_VERTEX = 0,
    CE_BUFFER_INDEX
} CeBufferType;

typedef enum CeTextureFormat {
    CE_TEX_RGBA8 = 0,
    CE_TEX_RGB8,
    CE_TEX_DEPTH24
} CeTextureFormat;

/* ---- 不透明句柄 ---- */

typedef struct CeRhiDevice   CeRhiDevice;
typedef struct CeRhiBuffer   CeRhiBuffer;
typedef struct CeRhiTexture  CeRhiTexture;
typedef struct CeRhiShader   CeRhiShader;
typedef struct CeRhiPipeline CeRhiPipeline;

/* ---- 配置 ---- */

typedef struct CeRhiConfig {
    CeRhiBackend backend;
    void*        native_window;  /* 平台原生窗口句柄 */
    int          width;
    int          height;
    const char*  title;
    CeBool       vsync;
} CeRhiConfig;

typedef struct CeBufferDesc {
    CeBufferType type;
    size_t       size;
    const void*  data;
} CeBufferDesc;

typedef struct CeTextureDesc {
    CeTextureFormat format;
    int             width;
    int             height;
    const void*     data;
} CeTextureDesc;

typedef struct CePipelineDesc {
    CeRhiShader* vertex_shader;
    CeRhiShader* fragment_shader;
    /* 顶点布局描述 */
    const char**  attributes;   /* 属性名数组 */
    uint32_t      attr_count;
    uint32_t      vertex_stride;
} CePipelineDesc;

/* ---- 设备 ---- */

CeRhiDevice* rhi_create_device(const CeRhiConfig* config);
void         rhi_destroy_device(CeRhiDevice* device);
void         rhi_resize(CeRhiDevice* device, int width, int height);
CeBool       rhi_should_close(CeRhiDevice* device);
void         rhi_poll_events(CeRhiDevice* device);

/* ---- 资源 ---- */

CeRhiBuffer*   rhi_create_buffer(CeRhiDevice* device, const CeBufferDesc* desc);
void           rhi_destroy_buffer(CeRhiDevice* device, CeRhiBuffer* buffer);

CeRhiTexture*  rhi_create_texture(CeRhiDevice* device, const CeTextureDesc* desc);
void           rhi_destroy_texture(CeRhiDevice* device, CeRhiTexture* texture);

CeRhiShader*   rhi_create_shader(CeRhiDevice* device, CeShaderStage stage,
                                  const char* source);
void           rhi_destroy_shader(CeRhiDevice* device, CeRhiShader* shader);

CeRhiPipeline* rhi_create_pipeline(CeRhiDevice* device, const CePipelineDesc* desc);
void           rhi_destroy_pipeline(CeRhiDevice* device, CeRhiPipeline* pipeline);

/* ---- 渲染命令 ---- */

void rhi_begin_frame(CeRhiDevice* device, CeColor clear_color);
void rhi_bind_pipeline(CeRhiDevice* device, CeRhiPipeline* pipeline);
void rhi_bind_buffer(CeRhiDevice* device, CeRhiBuffer* buffer, uint32_t slot);
void rhi_set_uniform_mat4(CeRhiDevice* device, const char* name, const CeMat4* mat);
void rhi_draw(CeRhiDevice* device, uint32_t vertex_count, uint32_t first_vertex);
void rhi_draw_indexed(CeRhiDevice* device, uint32_t index_count, uint32_t first_index);
void rhi_end_frame(CeRhiDevice* device);
void rhi_present(CeRhiDevice* device);

#ifdef __cplusplus
}
#endif

#endif /* CE_RHI_H */
