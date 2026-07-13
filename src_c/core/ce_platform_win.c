/*
 * ChaosEngine platform layer -- Windows implementation
 * Covers: file I/O, threads, mutex, condvar, window stub
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/ce_platform.h"
#include "core/ce_memory.h"
#include "core/ce_win_compat.h"

/* ================================================================
 * File I/O
 * ================================================================ */

struct CeFile {
    FILE*  handle;
    size_t size;
};

CeFile* ce_file_open(const char* path, CeFileMode mode) {
    const char* m;
    switch (mode) {
        case CE_FILE_READ:   m = "rb"; break;
        case CE_FILE_WRITE:  m = "wb"; break;
        case CE_FILE_APPEND: m = "ab"; break;
        default: return NULL;
    }
    FILE* f = fopen(path, m);
    if (!f) return NULL;
    CeFile* file = (CeFile*)malloc(sizeof(CeFile));
    file->handle = f;
    fseek(f, 0, SEEK_END);
    file->size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    return file;
}

void ce_file_close(CeFile* file) {
    if (!file) return;
    fclose(file->handle);
    free(file);
}

size_t ce_file_read(CeFile* file, void* buf, size_t sz) {
    return file ? fread(buf, 1, sz, file->handle) : 0;
}

size_t ce_file_write(CeFile* file, const void* buf, size_t sz) {
    return file ? fwrite(buf, 1, sz, file->handle) : 0;
}

CeBool ce_file_seek(CeFile* file, int64_t offset, int origin) {
    return (file && _fseeki64(file->handle, offset, origin) == 0) ? CE_TRUE : CE_FALSE;
}

int64_t ce_file_tell(CeFile* file) {
    return file ? (int64_t)_ftelli64(file->handle) : -1;
}

size_t ce_file_size(CeFile* file) {
    return file ? file->size : 0;
}

char* ce_file_read_all(const char* path, size_t* out_size) {
    CeFile* f = ce_file_open(path, CE_FILE_READ);
    if (!f) return NULL;
    size_t sz = ce_file_size(f);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { ce_file_close(f); return NULL; }
    size_t n = ce_file_read(f, buf, sz);
    buf[n] = '\0';
    if (out_size) *out_size = n;
    ce_file_close(f);
    return buf;
}

CeBool ce_file_write_all(const char* path, const void* data, size_t size) {
    CeFile* f = ce_file_open(path, CE_FILE_WRITE);
    if (!f) return CE_FALSE;
    size_t n = ce_file_write(f, data, size);
    ce_file_close(f);
    return n == size ? CE_TRUE : CE_FALSE;
}

CeBool ce_file_exists(const char* path) {
    return (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ? CE_TRUE : CE_FALSE;
}

/* ================================================================
 * Threads
 * ================================================================ */

struct CeThread {
    HANDLE     handle;
    CeThreadFn fn;
    void*      user_data;
};

static DWORD WINAPI thread_entry(LPVOID arg) {
    CeThread* t = (CeThread*)arg;
    t->fn(t->user_data);
    return 0;
}

CeThread* ce_thread_create(CeThreadFn fn, void* user_data) {
    CeThread* t = (CeThread*)malloc(sizeof(CeThread));
    t->fn = fn;
    t->user_data = user_data;
    t->handle = CreateThread(NULL, 0, thread_entry, t, 0, NULL);
    if (!t->handle) { free(t); return NULL; }
    return t;
}

void ce_thread_destroy(CeThread* thread) {
    if (!thread) return;
    CloseHandle(thread->handle);
    free(thread);
}

void ce_thread_join(CeThread* thread) {
    if (thread) WaitForSingleObject(thread->handle, INFINITE);
}

void ce_thread_sleep_ms(uint32_t ms) {
    Sleep(ms);
}

uint32_t ce_thread_hardware_concurrency(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (uint32_t)si.dwNumberOfProcessors;
}

/* ================================================================
 * Mutex
 * ================================================================ */

struct CeMutex {
    CRITICAL_SECTION cs;
};

CeMutex* ce_mutex_create(void) {
    CeMutex* m = (CeMutex*)malloc(sizeof(CeMutex));
    InitializeCriticalSection(&m->cs);
    return m;
}

void ce_mutex_destroy(CeMutex* mutex) {
    if (!mutex) return;
    DeleteCriticalSection(&mutex->cs);
    free(mutex);
}

void ce_mutex_lock(CeMutex* mutex) {
    if (mutex) EnterCriticalSection(&mutex->cs);
}

void ce_mutex_unlock(CeMutex* mutex) {
    if (mutex) LeaveCriticalSection(&mutex->cs);
}

/* ================================================================
 * Condition Variable
 * ================================================================ */

struct CeCondVar {
    CONDITION_VARIABLE cv;
};

CeCondVar* ce_condvar_create(void) {
    CeCondVar* c = (CeCondVar*)malloc(sizeof(CeCondVar));
    InitializeConditionVariable(&c->cv);
    return c;
}

void ce_condvar_destroy(CeCondVar* cv) {
    free(cv);
}

void ce_condvar_wait(CeCondVar* cv, CeMutex* mutex) {
    if (cv && mutex) SleepConditionVariableCS(&cv->cv, &mutex->cs, INFINITE);
}

void ce_condvar_signal(CeCondVar* cv) {
    if (cv) WakeConditionVariable(&cv->cv);
}

void ce_condvar_broadcast(CeCondVar* cv) {
    if (cv) WakeAllConditionVariable(&cv->cv);
}

/* ================================================================
 * Window stub (headless on Windows)
 * ================================================================ */

struct CeWindow {
    char  title[128];
    int   width, height;
    CeBool fullscreen, resizable, should_close;
};

CeWindow* ce_window_create(const CeWindowDesc* desc) {
    CeWindow* w = (CeWindow*)calloc(1, sizeof(CeWindow));
    strncpy(w->title, desc->title, sizeof(w->title) - 1);
    w->width = desc->width;
    w->height = desc->height;
    w->fullscreen = desc->fullscreen;
    w->resizable = desc->resizable;
    return w;
}

void      ce_window_destroy(CeWindow* w)           { free(w); }
CeBool    ce_window_should_close(CeWindow* w)       { return w ? w->should_close : CE_TRUE; }
void      ce_window_poll_events(void)               {}
void      ce_window_swap_buffers(CeWindow* w)       { (void)w; }
void      ce_window_get_size(CeWindow* w, int* x, int* y) {
    if (w) { if (x) *x = w->width; if (y) *y = w->height; }
}
float     ce_window_get_aspect(CeWindow* w) {
    return (!w || w->height == 0) ? 1.0f : (float)w->width / (float)w->height;
}
void*     ce_window_get_native_handle(CeWindow* w)  { (void)w; return NULL; }

/* ================================================================
 * Input stub
 * ================================================================ */

static struct {
    CeBool keys[CE_KEY_COUNT], keys_prev[CE_KEY_COUNT];
    CeBool mouse[5], mouse_prev[5];
    float mx, my, mdx, mdy, scroll;
} g_input;

CeBool ce_input_is_key_down(CeKey k)      { return k < CE_KEY_COUNT ? g_input.keys[k] : CE_FALSE; }
CeBool ce_input_is_key_pressed(CeKey k)   { return k < CE_KEY_COUNT ? (g_input.keys[k] && !g_input.keys_prev[k]) : CE_FALSE; }
CeBool ce_input_is_key_released(CeKey k)  { return k < CE_KEY_COUNT ? (!g_input.keys[k] && g_input.keys_prev[k]) : CE_FALSE; }
CeBool ce_input_is_mouse_down(CeMouseButton b)    { return b <= 4 ? g_input.mouse[b] : CE_FALSE; }
CeBool ce_input_is_mouse_pressed(CeMouseButton b) { return b <= 4 ? (g_input.mouse[b] && !g_input.mouse_prev[b]) : CE_FALSE; }
void   ce_input_get_mouse_pos(float* x, float* y)   { if (x) *x = g_input.mx; if (y) *y = g_input.my; }
void   ce_input_get_mouse_delta(float* dx, float* dy){ if (dx) *dx = g_input.mdx; if (dy) *dy = g_input.mdy; }
float  ce_input_get_mouse_scroll(void)              { return g_input.scroll; }

void ce_input_end_frame(void) {
    memcpy(g_input.keys_prev,  g_input.keys,  sizeof(g_input.keys));
    memcpy(g_input.mouse_prev, g_input.mouse, sizeof(g_input.mouse));
    g_input.mdx = g_input.mdy = g_input.scroll = 0;
}

#endif /* _WIN32 */
