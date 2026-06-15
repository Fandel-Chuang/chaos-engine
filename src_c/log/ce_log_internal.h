/*
 * ChaosEngine 日志系统内部头文件
 */

#ifndef CE_LOG_INTERNAL_H
#define CE_LOG_INTERNAL_H

#include "public_api/ce_log.h"
#include "public_api/ce_types.h"
#include "core/ce_memory.h"

/* ---- 内部 API ---- */

CeResult ce_log_init(CeAllocator* allocator, CeLogLevel min_level, const char* file_path);
void     ce_log_shutdown(void);
void     ce_log_set_observe_mode(CeBool enable);
CeBool   ce_log_get_observe_mode(void);

#endif /* CE_LOG_INTERNAL_H */
