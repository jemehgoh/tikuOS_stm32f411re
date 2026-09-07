/* The STM32N6 ATON cache is maintained by the platform integration. */
#ifndef TIKU_LL_ATON_NPU_CACHE_H_
#define TIKU_LL_ATON_NPU_CACHE_H_

#include <stdint.h>
static inline void npu_cache_clean_range(uintptr_t begin, uintptr_t end)
{ (void)begin; (void)end; }
static inline void npu_cache_clean_invalidate_range(uintptr_t begin, uintptr_t end)
{ (void)begin; (void)end; }

#endif
