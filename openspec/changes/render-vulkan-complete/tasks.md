# Tasks: Render Vulkan RHI 完整化

> 从 render-vulkan-complete 拆分。AOI/Cell 已完成（test_aoi + test_cell 持续通过）。
> **全部 Phase 1-5 已实现** ✅

---

## Phase 1: 头文件重构 ✅

- [x] 1.1-1.10 全部完成：`ce_rhi_vulkan.h` (135 行)，结构体+内联+常量+辅助函数

## Phase 2: 纹理 ✅

- [x] 2.1-2.6 全部完成：`rhi_create_texture` (L788), `rhi_destroy_texture` (L864), staging upload

## Phase 3: 着色器 ✅

- [x] 3.1-3.3 全部完成：`rhi_create_shader` (L873), `rhi_destroy_shader` (L897)

## Phase 4: 管线 ✅

- [x] 4.1-4.3 全部完成：`rhi_create_pipeline` (L904), PushConstants + 完整管线状态

## Phase 5: Uniform ✅

- [x] 5.1-5.3 全部完成：`rhi_set_uniform_mat4` (L1113), `vkCmdPushConstants`

---

**总进度：25/25 已完成（100%）** ✅
