/*
 * ChaosEngine high-resolution timer -- Windows implementation
 */

#ifdef _WIN32

#include "core/ce_time.h"
#include "core/ce_win_compat.h"
#include <stdlib.h>

struct CeTimePoint {
    struct timespec ts;
};

CeTimePoint ce_time_now(void) {
    CeTimePoint tp;
    clock_gettime(CLOCK_MONOTONIC, &tp.ts);
    return tp;
}

double ce_time_elapsed(CeTimePoint start, CeTimePoint end) {
    double s  = (double)(end.ts.tv_sec  - start.ts.tv_sec);
    double ns = (double)(end.ts.tv_nsec - start.ts.tv_nsec) / 1e9;
    return s + ns;
}

double ce_time_since(CeTimePoint start) {
    return ce_time_elapsed(start, ce_time_now());
}

struct CeFrameTimer {
    CeTimePoint start_time, last_tick;
    double total_time, delta_time, target_frame_time;
    float  fps;
    uint64_t frame_count;
    double   fps_accum;
    uint32_t fps_frame_count;
};

CeFrameTimer* ce_frame_timer_create(double target_fps) {
    CeFrameTimer* t = (CeFrameTimer*)malloc(sizeof(CeFrameTimer));
    t->start_time = ce_time_now();
    t->last_tick  = t->start_time;
    t->total_time = 0.0;
    t->delta_time = 1.0 / target_fps;
    t->target_frame_time = 1.0 / target_fps;
    t->fps = (float)target_fps;
    t->frame_count = 0;
    t->fps_accum = 0.0;
    t->fps_frame_count = 0;
    return t;
}

void   ce_frame_timer_destroy(CeFrameTimer* t) { free(t); }

double ce_frame_timer_tick(CeFrameTimer* t) {
    CeTimePoint now = ce_time_now();
    t->delta_time = ce_time_elapsed(t->last_tick, now);
    t->last_tick  = now;
    t->total_time += t->delta_time;
    t->frame_count++;
    t->fps_accum += t->delta_time;
    t->fps_frame_count++;
    if (t->fps_accum >= 1.0) {
        t->fps = (float)t->fps_frame_count / (float)t->fps_accum;
        t->fps_accum = 0.0;
        t->fps_frame_count = 0;
    }
    return t->delta_time;
}

double   ce_frame_timer_total(CeFrameTimer* t)       { return t->total_time; }
float    ce_frame_timer_fps(CeFrameTimer* t)          { return t->fps; }
uint64_t ce_frame_timer_frame_count(CeFrameTimer* t)  { return t->frame_count; }

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

#endif /* _WIN32 */
