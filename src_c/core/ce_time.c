/*
 * ChaosEngine 高精度计时器 — Linux 实现
 */

#define _POSIX_C_SOURCE 199309L
#include "core/ce_time.h"
#include <stdlib.h>
#include <time.h>

/* ---- 时间点 ---- */

struct CeTimePoint {
    struct timespec ts;
};

CeTimePoint ce_time_now(void) {
    CeTimePoint tp;
    clock_gettime(CLOCK_MONOTONIC, &tp.ts);
    return tp;
}

double ce_time_elapsed(CeTimePoint start, CeTimePoint end) {
    double s = (double)(end.ts.tv_sec - start.ts.tv_sec);
    double ns = (double)(end.ts.tv_nsec - start.ts.tv_nsec) / 1e9;
    return s + ns;
}

double ce_time_since(CeTimePoint start) {
    return ce_time_elapsed(start, ce_time_now());
}

/* ---- 帧计时器 ---- */

struct CeFrameTimer {
    CeTimePoint start_time;
    CeTimePoint last_tick;
    double      total_time;
    double      delta_time;
    double      target_frame_time;
    float       fps;
    uint64_t    frame_count;
    double      fps_accum;
    uint32_t    fps_frame_count;
};

CeFrameTimer* ce_frame_timer_create(double target_fps) {
    CeFrameTimer* t = (CeFrameTimer*)malloc(sizeof(CeFrameTimer));
    t->start_time       = ce_time_now();
    t->last_tick        = t->start_time;
    t->total_time       = 0.0;
    t->delta_time       = 1.0 / target_fps;
    t->target_frame_time = 1.0 / target_fps;
    t->fps              = (float)target_fps;
    t->frame_count      = 0;
    t->fps_accum        = 0.0;
    t->fps_frame_count  = 0;
    return t;
}

void ce_frame_timer_destroy(CeFrameTimer* timer) {
    free(timer);
}

double ce_frame_timer_tick(CeFrameTimer* timer) {
    CeTimePoint now = ce_time_now();
    timer->delta_time = ce_time_elapsed(timer->last_tick, now);
    timer->last_tick = now;
    timer->total_time += timer->delta_time;
    timer->frame_count++;

    /* 每秒更新一次 FPS */
    timer->fps_accum += timer->delta_time;
    timer->fps_frame_count++;
    if (timer->fps_accum >= 1.0) {
        timer->fps = (float)timer->fps_frame_count / (float)timer->fps_accum;
        timer->fps_accum = 0.0;
        timer->fps_frame_count = 0;
    }

    return timer->delta_time;
}

double ce_frame_timer_total(CeFrameTimer* timer) {
    return timer->total_time;
}

float ce_frame_timer_fps(CeFrameTimer* timer) {
    return timer->fps;
}

uint64_t ce_frame_timer_frame_count(CeFrameTimer* timer) {
    return timer->frame_count;
}

/* ---- 便捷函数 ---- */

double ce_time_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

uint64_t ce_time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint64_t ce_time_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
