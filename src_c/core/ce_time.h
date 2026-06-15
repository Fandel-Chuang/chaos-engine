/*
 * ChaosEngine 高精度计时器
 */

#ifndef CE_TIME_H
#define CE_TIME_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 时间点（不透明） ---- */

typedef struct CeTimePoint CeTimePoint;

/** 获取当前时间点 */
CeTimePoint ce_time_now(void);

/** 计算两个时间点之间的秒数 */
double ce_time_elapsed(CeTimePoint start, CeTimePoint end);

/** 从 start 到现在的秒数 */
double ce_time_since(CeTimePoint start);

/* ---- 帧计时器 ---- */

typedef struct CeFrameTimer CeFrameTimer;

/** 创建帧计时器 */
CeFrameTimer* ce_frame_timer_create(double target_fps);

/** 销毁 */
void ce_frame_timer_destroy(CeFrameTimer* timer);

/** 标记一帧开始，返回上一帧的 delta_time */
double ce_frame_timer_tick(CeFrameTimer* timer);

/** 获取总运行时间 */
double ce_frame_timer_total(CeFrameTimer* timer);

/** 获取当前 FPS */
float ce_frame_timer_fps(CeFrameTimer* timer);

/** 获取帧数 */
uint64_t ce_frame_timer_frame_count(CeFrameTimer* timer);

/* ---- 便捷函数 ---- */

/** 获取高精度时间戳（秒） */
double ce_time_now_seconds(void);

/** 获取高精度时间戳（毫秒） */
uint64_t ce_time_now_ms(void);

/** 获取高精度时间戳（微秒） */
uint64_t ce_time_now_us(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_TIME_H */
