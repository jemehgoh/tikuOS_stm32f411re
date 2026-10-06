


#include <stdint.h>

#if (LL_ATON_PLATFORM == LL_ATON_PLAT_STM32N6)

/* from stm32n657xx.h */

#if defined (__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
#define CPU_IN_SECURE_STATE
#endif

#define PERIPH_BASE_S                   0x50000000UL   /*!< Base address of : AHB/APB Peripherals                    */
#define AHB5PERIPH_BASE_S               (PERIPH_BASE_S + 0x08020000UL)
#define NPU_BASE_S                      (AHB5PERIPH_BASE_S + 0x0C0000UL)

#define PERIPH_BASE_NS                  0x40000000UL /*!< Base address of : AHB/APB Peripherals                      */
#define AHB5PERIPH_BASE_NS              (PERIPH_BASE_NS + 0x08020000UL)
#define NPU_BASE_NS                     (AHB5PERIPH_BASE_NS + 0x0C0000UL)

/* Minimal CMSIS names required by the vendored LL-ATON platform layer. */
#define NPU0_IRQn                       53
#define NPU1_IRQn                       54
#define NPU2_IRQn                       55
#define NPU3_IRQn                       56
#define __WFE()                         ((void)0)
#define __DSB()                         ((void)0)

void SCB_InvalidateICache_by_Addr(void *addr, int32_t size);


#if defined(CPU_IN_SECURE_STATE)
#define ATON_BASE NPU_BASE_S
#else
#define ATON_BASE NPU_BASE_NS
#endif

#else

#error "LL_ATON_PLATFORM should be equal to LL_ATON_PLAT_STM32N6"

#endif
