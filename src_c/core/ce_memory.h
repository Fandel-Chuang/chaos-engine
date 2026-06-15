/*
 * 内存管理头文件 — 引擎内部使用（非 public_api）
 */

#ifndef CE_MEMORY_H
#define CE_MEMORY_H

#include "public_api/ce_types.h"
#include <stdint.h>

/* ---- 分配器结构（内部定义） ---- */

struct CeAllocator {
    CeAllocatorVTable vtable;
    void*             user_data;
};

/* ---- 分配器 API ---- */

CeAllocator* ce_allocator_create(const CeAllocatorVTable* vtable, void* user_data);
void         ce_allocator_destroy(CeAllocator* alloc);
void*        ce_alloc(CeAllocator* alloc, size_t size);
void*        ce_alloc_zero(CeAllocator* alloc, size_t size);
void         ce_free(CeAllocator* alloc, void* ptr);
void*        ce_realloc(CeAllocator* alloc, void* ptr, size_t new_size);

/* ---- 内存池 ---- */

typedef struct CeMemPool CeMemPool;

CeMemPool* ce_mempool_create(CeAllocator* alloc, size_t block_size, size_t block_count);
void       ce_mempool_destroy(CeMemPool* pool);
void*      ce_mempool_alloc(CeMemPool* pool);
void       ce_mempool_free(CeMemPool* pool, void* ptr);
size_t     ce_mempool_free_count(const CeMemPool* pool);
size_t     ce_mempool_block_size(const CeMemPool* pool);

#endif /* CE_MEMORY_H */
