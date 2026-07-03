/*
 * ChaosEngine 平台抽象层
 * 统一跨平台接口：窗口、输入、文件、线程
 */

#ifndef CE_PLATFORM_H
#define CE_PLATFORM_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 窗口
 * ================================================================ */

typedef struct CeWindow CeWindow;

typedef struct CeWindowDesc {
    const char* title;
    int         width;
    int         height;
    CeBool      fullscreen;
    CeBool      resizable;
    int         msaa_samples;   /* 0 = 禁用 */
} CeWindowDesc;

CeWindow* ce_window_create(const CeWindowDesc* desc);
void      ce_window_destroy(CeWindow* window);
CeBool    ce_window_should_close(CeWindow* window);
void      ce_window_poll_events(void);
void      ce_window_swap_buffers(CeWindow* window);
void      ce_window_get_size(CeWindow* window, int* width, int* height);
float     ce_window_get_aspect(CeWindow* window);
void*     ce_window_get_native_handle(CeWindow* window);  /* HWND/NSWindow/X11 Window */

/* ================================================================
 * 输入
 * ================================================================ */

/* 按键码（物理键位，非字符） */
typedef enum CeKey {
    CE_KEY_UNKNOWN = 0,
    CE_KEY_SPACE, CE_KEY_APOSTROPHE, CE_KEY_COMMA,
    CE_KEY_MINUS, CE_KEY_PERIOD, CE_KEY_SLASH,
    CE_KEY_0, CE_KEY_1, CE_KEY_2, CE_KEY_3, CE_KEY_4,
    CE_KEY_5, CE_KEY_6, CE_KEY_7, CE_KEY_8, CE_KEY_9,
    CE_KEY_SEMICOLON, CE_KEY_EQUAL,
    CE_KEY_A, CE_KEY_B, CE_KEY_C, CE_KEY_D, CE_KEY_E,
    CE_KEY_F, CE_KEY_G, CE_KEY_H, CE_KEY_I, CE_KEY_J,
    CE_KEY_K, CE_KEY_L, CE_KEY_M, CE_KEY_N, CE_KEY_O,
    CE_KEY_P, CE_KEY_Q, CE_KEY_R, CE_KEY_S, CE_KEY_T,
    CE_KEY_U, CE_KEY_V, CE_KEY_W, CE_KEY_X, CE_KEY_Y, CE_KEY_Z,
    CE_KEY_LEFT_BRACKET, CE_KEY_BACKSLASH, CE_KEY_RIGHT_BRACKET,
    CE_KEY_GRAVE_ACCENT,
    CE_KEY_ESCAPE, CE_KEY_ENTER, CE_KEY_TAB, CE_KEY_BACKSPACE,
    CE_KEY_INSERT, CE_KEY_DELETE,
    CE_KEY_RIGHT, CE_KEY_LEFT, CE_KEY_DOWN, CE_KEY_UP,
    CE_KEY_PAGE_UP, CE_KEY_PAGE_DOWN, CE_KEY_HOME, CE_KEY_END,
    CE_KEY_CAPS_LOCK, CE_KEY_SCROLL_LOCK, CE_KEY_NUM_LOCK,
    CE_KEY_PRINT_SCREEN, CE_KEY_PAUSE,
    CE_KEY_F1, CE_KEY_F2, CE_KEY_F3, CE_KEY_F4,
    CE_KEY_F5, CE_KEY_F6, CE_KEY_F7, CE_KEY_F8,
    CE_KEY_F9, CE_KEY_F10, CE_KEY_F11, CE_KEY_F12,
    CE_KEY_KP_0, CE_KEY_KP_1, CE_KEY_KP_2, CE_KEY_KP_3, CE_KEY_KP_4,
    CE_KEY_KP_5, CE_KEY_KP_6, CE_KEY_KP_7, CE_KEY_KP_8, CE_KEY_KP_9,
    CE_KEY_KP_DECIMAL, CE_KEY_KP_DIVIDE, CE_KEY_KP_MULTIPLY,
    CE_KEY_KP_SUBTRACT, CE_KEY_KP_ADD, CE_KEY_KP_ENTER, CE_KEY_KP_EQUAL,
    CE_KEY_LEFT_SHIFT, CE_KEY_LEFT_CONTROL, CE_KEY_LEFT_ALT, CE_KEY_LEFT_SUPER,
    CE_KEY_RIGHT_SHIFT, CE_KEY_RIGHT_CONTROL, CE_KEY_RIGHT_ALT, CE_KEY_RIGHT_SUPER,
    CE_KEY_MENU,
    CE_KEY_COUNT
} CeKey;

typedef enum CeMouseButton {
    CE_MOUSE_BUTTON_1 = 0,
    CE_MOUSE_BUTTON_2 = 1,
    CE_MOUSE_BUTTON_3 = 2,
    CE_MOUSE_BUTTON_4 = 3,
    CE_MOUSE_BUTTON_5 = 4,
    CE_MOUSE_BUTTON_LEFT   = CE_MOUSE_BUTTON_1,
    CE_MOUSE_BUTTON_RIGHT  = CE_MOUSE_BUTTON_2,
    CE_MOUSE_BUTTON_MIDDLE = CE_MOUSE_BUTTON_3,
} CeMouseButton;

typedef enum CeInputAction {
    CE_INPUT_RELEASE = 0,
    CE_INPUT_PRESS   = 1,
    CE_INPUT_REPEAT  = 2,
} CeInputAction;

/* 键盘 */
CeBool ce_input_is_key_down(CeKey key);
CeBool ce_input_is_key_pressed(CeKey key);   /* 本帧刚按下 */
CeBool ce_input_is_key_released(CeKey key);  /* 本帧刚释放 */

/* 内部输入更新（由平台事件层调用） */
void ce_input_set_key_state(CeKey key, CeBool down);
void ce_input_set_mouse_state(CeMouseButton button, CeBool down);
void ce_input_set_mouse_pos(float x, float y);
void ce_input_add_mouse_delta(float dx, float dy);
void ce_input_set_mouse_scroll(float scroll);

/* 鼠标 */
CeBool ce_input_is_mouse_down(CeMouseButton button);
CeBool ce_input_is_mouse_pressed(CeMouseButton button);
void   ce_input_get_mouse_pos(float* x, float* y);
void   ce_input_get_mouse_delta(float* dx, float* dy);
float  ce_input_get_mouse_scroll(void);       /* 本帧滚轮值 */

/* 每帧末尾调用，更新 pressed/released 状态 */
void ce_input_end_frame(void);

/* ================================================================
 * 文件 IO
 * ================================================================ */

typedef struct CeFile CeFile;

typedef enum CeFileMode {
    CE_FILE_READ  = 0,
    CE_FILE_WRITE = 1,
    CE_FILE_APPEND = 2,
} CeFileMode;

CeFile*    ce_file_open(const char* path, CeFileMode mode);
void       ce_file_close(CeFile* file);
size_t     ce_file_read(CeFile* file, void* buffer, size_t size);
size_t     ce_file_write(CeFile* file, const void* buffer, size_t size);
CeBool     ce_file_seek(CeFile* file, int64_t offset, int origin);
int64_t    ce_file_tell(CeFile* file);
size_t     ce_file_size(CeFile* file);

/* 便捷函数：一次性读取整个文件 */
char*      ce_file_read_all(const char* path, size_t* out_size);
CeBool     ce_file_write_all(const char* path, const void* data, size_t size);
CeBool     ce_file_exists(const char* path);

/* ================================================================
 * 线程
 * ================================================================ */

typedef struct CeThread CeThread;
typedef struct CeMutex  CeMutex;
typedef struct CeCondVar CeCondVar;

typedef int (*CeThreadFn)(void* user_data);

/* 线程 */
CeThread* ce_thread_create(CeThreadFn fn, void* user_data);
void      ce_thread_destroy(CeThread* thread);
void      ce_thread_join(CeThread* thread);
void      ce_thread_sleep_ms(uint32_t ms);

/* 互斥锁 */
CeMutex*  ce_mutex_create(void);
void      ce_mutex_destroy(CeMutex* mutex);
void      ce_mutex_lock(CeMutex* mutex);
void      ce_mutex_unlock(CeMutex* mutex);

/* 条件变量 */
CeCondVar* ce_condvar_create(void);
void       ce_condvar_destroy(CeCondVar* cv);
void       ce_condvar_wait(CeCondVar* cv, CeMutex* mutex);
void       ce_condvar_signal(CeCondVar* cv);
void       ce_condvar_broadcast(CeCondVar* cv);

/* CPU 核心数 */
uint32_t ce_thread_hardware_concurrency(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_PLATFORM_H */
