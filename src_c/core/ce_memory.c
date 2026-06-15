/*
 * 内存池实现 — 引擎内核专用，与编辑器内存完全隔离
 */

#include "core/ce_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 默认分配器（使用系统 malloc/free） ---- */

static void* default_malloc(size_t size, void* user_data) {
    (void)user_data;
    return malloc(size);
}

static void default_free(void* ptr, void* user_data) {
    (void)user_data;
    free(ptr);
}

static void* default_realloc(void* ptr, size_t new_size, void* user_data) {
    (void)user_data;
    return realloc(ptr, new_size);
}

static CeAllocatorVTable g_default_vtable = {
    .malloc  = default_malloc,
    .free    = default_free,
    .realloc = default_realloc
};

/* ---- 分配器实现 ---- */

CeAllocator* ce_allocator_create(const CeAllocatorVTable* vtable, void* user_data) {
    CeAllocator* alloc = (CeAllocator*)malloc(sizeof(CeAllocator));
    if (!alloc) return NULL;
    alloc->vtable    = vtable ? *vtable : g_default_vtable;
    alloc->user_data = user_data;
    return alloc;
}

void ce_allocator_destroy(CeAllocator* alloc) {
    if (alloc) free(alloc);
}

void* ce_alloc(CeAllocator* alloc, size_t size) {
    if (!alloc) return malloc(size);
    return alloc->vtable.malloc(size, alloc->user_data);
}

void* ce_alloc_zero(CeAllocator* alloc, size_t size) {
    void* ptr = ce_alloc(alloc, size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void ce_free(CeAllocator* alloc, void* ptr) {
    if (!alloc) { free(ptr); return; }
    alloc->vtable.free(ptr, alloc->user_data);
}

void* ce_realloc(CeAllocator* alloc, void* ptr, size_t new_size) {
    if (!alloc) return realloc(ptr, new_size);
    return alloc->vtable.realloc(ptr, new_size, alloc->user_data);
}

/* ---- 内存池（固定大小块分配器） ---- */

struct CeMemPool {
    CeAllocator* allocator;
    size_t       block_size;
    size_t       block_count;
    size_t       free_count;
    uint8_t*     memory;       /* 连续内存块 */
    uint32_t*    free_list;    /* 空闲块索引链表 */
    uint32_t     free_head;    /* 链表头 */
};

CeMemPool* ce_mempool_create(CeAllocator* alloc, size_t block_size, size_t block_count) {
    if (block_size < sizeof(void*) || block_count == 0) return NULL;

    CeMemPool* pool = (CeMemPool*)ce_alloc(alloc, sizeof(CeMemPool));
    if (!pool) return NULL;

    pool->allocator   = alloc;
    pool->block_size  = block_size;
    pool->block_count = block_count;
    pool->free_count  = block_count;

    /* 分配连续内存 */
    pool->memory = (uint8_t*)ce_alloc(alloc, block_size * block_count);
    if (!pool->memory) {
        ce_free(alloc, pool);
        return NULL;
    }

    /* 初始化空闲链表 */
    pool->free_list = (uint32_t*)ce_alloc(alloc, sizeof(uint32_t) * block_count);
    if (!pool->free_list) {
        ce_free(alloc, pool->memory);
        ce_free(alloc, pool);
        return NULL;
    }

    for (size_t i = 0; i < block_count - 1; i++) {
        pool->free_list[i] = (uint32_t)(i + 1);
    }
    pool->free_list[block_count - 1] = UINT32_MAX;
    pool->free_head = 0;

    return pool;
}

void ce_mempool_destroy(CeMemPool* pool) {
    if (!pool) return;
    ce_free(pool->allocator, pool->memory);
    ce_free(pool->allocator, pool->free_list);
    ce_free(pool->allocator, pool);
}

void* ce_mempool_alloc(CeMemPool* pool) {
    if (!pool || pool->free_head == UINT32_MAX) return NULL;

    uint32_t index = pool->free_head;
    pool->free_head = pool->free_list[index];
    pool->free_count--;

    return pool->memory + (index * pool->block_size);
}

void ce_mempool_free(CeMemPool* pool, void* ptr) {
    if (!pool || !ptr) return;

    /* 计算索引 */
    uintptr_t offset = (uintptr_t)((uint8_t*)ptr - pool->memory);
    uint32_t index = (uint32_t)(offset / pool->block_size);

    if (index >= pool->block_count) return; /* 越界保护 */

    pool->free_list[index] = pool->free_head;
    pool->free_head = index;
    pool->free_count++;
}

size_t ce_mempool_free_count(const CeMemPool* pool) {
    return pool ? pool->free_count : 0;
}

size_t ce_mempool_block_size(const CeMemPool* pool) {
    return pool ? pool->block_size : 0;
}
