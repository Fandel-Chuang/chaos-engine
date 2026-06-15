/*
 * ChaosEngine 平台抽象层 — Linux 实现
 * 窗口: X11 (后续可替换为 GLFW/Wayland)
 * 输入: X11 事件
 * 文件: POSIX
 * 线程: pthread
 */

#define _POSIX_C_SOURCE 199309L
#include "core/ce_platform.h"
#include "core/ce_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* ================================================================
 * 文件 IO — POSIX
 * ================================================================ */

struct CeFile {
    FILE* handle;
    size_t size;
};

CeFile* ce_file_open(const char* path, CeFileMode mode) {
    const char* mode_str;
    switch (mode) {
        case CE_FILE_READ:   mode_str = "rb"; break;
        case CE_FILE_WRITE:  mode_str = "wb"; break;
        case CE_FILE_APPEND: mode_str = "ab"; break;
        default: return NULL;
    }

    FILE* f = fopen(path, mode_str);
    if (!f) return NULL;

    CeFile* file = (CeFile*)malloc(sizeof(CeFile));
    file->handle = f;

    /* 获取文件大小 */
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

size_t ce_file_read(CeFile* file, void* buffer, size_t size) {
    if (!file) return 0;
    return fread(buffer, 1, size, file->handle);
}

size_t ce_file_write(CeFile* file, const void* buffer, size_t size) {
    if (!file) return 0;
    return fwrite(buffer, 1, size, file->handle);
}

CeBool ce_file_seek(CeFile* file, int64_t offset, int origin) {
    if (!file) return CE_FALSE;
    return fseek(file->handle, (long)offset, origin) == 0 ? CE_TRUE : CE_FALSE;
}

int64_t ce_file_tell(CeFile* file) {
    if (!file) return -1;
    return (int64_t)ftell(file->handle);
}

size_t ce_file_size(CeFile* file) {
    return file ? file->size : 0;
}

char* ce_file_read_all(const char* path, size_t* out_size) {
    CeFile* f = ce_file_open(path, CE_FILE_READ);
    if (!f) return NULL;

    size_t sz = ce_file_size(f);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        ce_file_close(f);
        return NULL;
    }

    size_t read = ce_file_read(f, buf, sz);
    buf[read] = '\0';

    if (out_size) *out_size = read;
    ce_file_close(f);
    return buf;
}

CeBool ce_file_write_all(const char* path, const void* data, size_t size) {
    CeFile* f = ce_file_open(path, CE_FILE_WRITE);
    if (!f) return CE_FALSE;

    size_t written = ce_file_write(f, data, size);
    ce_file_close(f);
    return written == size ? CE_TRUE : CE_FALSE;
}

CeBool ce_file_exists(const char* path) {
    return access(path, F_OK) == 0 ? CE_TRUE : CE_FALSE;
}

/* ================================================================
 * 线程 — pthread
 * ================================================================ */

struct CeThread {
    pthread_t handle;
    CeThreadFn fn;
    void* user_data;
    CeBool running;
};

static void* thread_entry(void* arg) {
    CeThread* t = (CeThread*)arg;
    t->fn(t->user_data);
    t->running = CE_FALSE;
    return NULL;
}

CeThread* ce_thread_create(CeThreadFn fn, void* user_data) {
    CeThread* t = (CeThread*)malloc(sizeof(CeThread));
    t->fn = fn;
    t->user_data = user_data;
    t->running = CE_TRUE;

    if (pthread_create(&t->handle, NULL, thread_entry, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void ce_thread_destroy(CeThread* thread) {
    if (!thread) return;
    free(thread);
}

void ce_thread_join(CeThread* thread) {
    if (!thread) return;
    pthread_join(thread->handle, NULL);
}

void ce_thread_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

/* 互斥锁 */
struct CeMutex {
    pthread_mutex_t handle;
};

CeMutex* ce_mutex_create(void) {
    CeMutex* m = (CeMutex*)malloc(sizeof(CeMutex));
    pthread_mutex_init(&m->handle, NULL);
    return m;
}

void ce_mutex_destroy(CeMutex* mutex) {
    if (!mutex) return;
    pthread_mutex_destroy(&mutex->handle);
    free(mutex);
}

void ce_mutex_lock(CeMutex* mutex) {
    if (mutex) pthread_mutex_lock(&mutex->handle);
}

void ce_mutex_unlock(CeMutex* mutex) {
    if (mutex) pthread_mutex_unlock(&mutex->handle);
}

/* 条件变量 */
struct CeCondVar {
    pthread_cond_t handle;
};

CeCondVar* ce_condvar_create(void) {
    CeCondVar* cv = (CeCondVar*)malloc(sizeof(CeCondVar));
    pthread_cond_init(&cv->handle, NULL);
    return cv;
}

void ce_condvar_destroy(CeCondVar* cv) {
    if (!cv) return;
    pthread_cond_destroy(&cv->handle);
    free(cv);
}

void ce_condvar_wait(CeCondVar* cv, CeMutex* mutex) {
    if (cv && mutex) pthread_cond_wait(&cv->handle, &mutex->handle);
}

void ce_condvar_signal(CeCondVar* cv) {
    if (cv) pthread_cond_signal(&cv->handle);
}

void ce_condvar_broadcast(CeCondVar* cv) {
    if (cv) pthread_cond_broadcast(&cv->handle);
}

uint32_t ce_thread_hardware_concurrency(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

/* ================================================================
 * 窗口 — 无头模式占位（后续集成 GLFW/X11）
 * ================================================================ */

struct CeWindow {
    char  title[128];
    int   width;
    int   height;
    CeBool fullscreen;
    CeBool resizable;
    CeBool should_close;
};

CeWindow* ce_window_create(const CeWindowDesc* desc) {
    CeWindow* w = (CeWindow*)malloc(sizeof(CeWindow));
    memset(w, 0, sizeof(*w));
    strncpy(w->title, desc->title, sizeof(w->title) - 1);
    w->width      = desc->width;
    w->height     = desc->height;
    w->fullscreen = desc->fullscreen;
    w->resizable  = desc->resizable;
    w->should_close = CE_FALSE;
    return w;
}

void ce_window_destroy(CeWindow* window) {
    free(window);
}

CeBool ce_window_should_close(CeWindow* window) {
    return window ? window->should_close : CE_TRUE;
}

void ce_window_poll_events(void) {
    /* 无头模式：空实现，后续接入 GLFW */
}

void ce_window_swap_buffers(CeWindow* window) {
    (void)window;
    /* 无头模式：空实现 */
}

void ce_window_get_size(CeWindow* window, int* width, int* height) {
    if (window) {
        if (width)  *width  = window->width;
        if (height) *height = window->height;
    }
}

float ce_window_get_aspect(CeWindow* window) {
    if (!window || window->height == 0) return 1.0f;
    return (float)window->width / (float)window->height;
}

void* ce_window_get_native_handle(CeWindow* window) {
    (void)window;
    return NULL;  /* 无头模式无原生窗口 */
}

/* ================================================================
 * 输入 — 占位实现
 * ================================================================ */

static struct {
    CeBool keys[CE_KEY_COUNT];
    CeBool keys_prev[CE_KEY_COUNT];
    CeBool mouse_buttons[5];
    CeBool mouse_buttons_prev[5];
    float  mouse_x, mouse_y;
    float  mouse_dx, mouse_dy;
    float  mouse_scroll;
} g_input;

CeBool ce_input_is_key_down(CeKey key) {
    if (key >= CE_KEY_COUNT) return CE_FALSE;
    return g_input.keys[key];
}

CeBool ce_input_is_key_pressed(CeKey key) {
    if (key >= CE_KEY_COUNT) return CE_FALSE;
    return g_input.keys[key] && !g_input.keys_prev[key];
}

CeBool ce_input_is_key_released(CeKey key) {
    if (key >= CE_KEY_COUNT) return CE_FALSE;
    return !g_input.keys[key] && g_input.keys_prev[key];
}

CeBool ce_input_is_mouse_down(CeMouseButton button) {
    if (button > 4) return CE_FALSE;
    return g_input.mouse_buttons[button];
}

CeBool ce_input_is_mouse_pressed(CeMouseButton button) {
    if (button > 4) return CE_FALSE;
    return g_input.mouse_buttons[button] && !g_input.mouse_buttons_prev[button];
}

void ce_input_get_mouse_pos(float* x, float* y) {
    if (x) *x = g_input.mouse_x;
    if (y) *y = g_input.mouse_y;
}

void ce_input_get_mouse_delta(float* dx, float* dy) {
    if (dx) *dx = g_input.mouse_dx;
    if (dy) *dy = g_input.mouse_dy;
}

float ce_input_get_mouse_scroll(void) {
    return g_input.mouse_scroll;
}

void ce_input_end_frame(void) {
    memcpy(g_input.keys_prev, g_input.keys, sizeof(g_input.keys));
    memcpy(g_input.mouse_buttons_prev, g_input.mouse_buttons, sizeof(g_input.mouse_buttons));
    g_input.mouse_dx = 0;
    g_input.mouse_dy = 0;
    g_input.mouse_scroll = 0;
}
