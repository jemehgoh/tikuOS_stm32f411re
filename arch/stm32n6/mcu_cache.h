/* Adapter for the ST cache interface; ranges are expressed as addr + len. */
#ifndef TIKU_LL_ATON_MCU_CACHE_H_
#define TIKU_LL_ATON_MCU_CACHE_H_

#include <stdint.h>
#include "tiku_cache_arch.h"

static inline void mcu_cache_clean_range(uintptr_t begin, uintptr_t end)
{
    if (end > begin) tiku_stm32n6_dcache_clean((const void *)begin, (size_t)(end - begin));
}
static inline void mcu_cache_invalidate_range(uintptr_t begin, uintptr_t end)
{
    if (end > begin) tiku_stm32n6_dcache_invalidate((const void *)begin, (size_t)(end - begin));
}
static inline void mcu_cache_clean_invalidate_range(uintptr_t begin, uintptr_t end)
{
    if (end > begin) {
        tiku_stm32n6_dcache_clean((const void *)begin, (size_t)(end - begin));
        tiku_stm32n6_dcache_invalidate((const void *)begin, (size_t)(end - begin));
    }
}

#endif
