/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_cpu_freq_boot_arch.c - STM32F411RE CPU bring-up
 *
 * Default clock target:
 *
 *   HSI (16 MHz)
 *     -> PLLM = 16     -> PLL input = 1 MHz
 *     -> PLLN = 200    -> VCO = 200 MHz
 *     -> PLLP = 2      -> SYSCLK = 100 MHz
 *     -> AHB  /1       -> HCLK  = 100 MHz
 *     -> APB1 /2       -> PCLK1 = 50 MHz
 *     -> APB2 /1       -> PCLK2 = 100 MHz
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_cpu_freq_boot_arch.h"
#include <stdint.h>
#include <stm32f411xe.h>

/*---------------------------------------------------------------------------*/
/* Local constants                                                           */
/*---------------------------------------------------------------------------*/

#define STM32F411_HSI_HZ              16000000UL
#define STM32F411_CLOCK_SPIN_TIMEOUT  1000000U

#define TIKU_STM32_RCC_PLLCFGR_PLLM(v) \
    ((((uint32_t)(v)) << RCC_PLLCFGR_PLLM_Pos) & RCC_PLLCFGR_PLLM_Msk)
#define TIKU_STM32_RCC_PLLCFGR_PLLN(v) \
    ((((uint32_t)(v)) << RCC_PLLCFGR_PLLN_Pos) & RCC_PLLCFGR_PLLN_Msk)
#define TIKU_STM32_RCC_PLLCFGR_PLLQ(v) \
    ((((uint32_t)(v)) << RCC_PLLCFGR_PLLQ_Pos) & RCC_PLLCFGR_PLLQ_Msk)
#define TIKU_STM32_FLASH_ACR_LATENCY(v) \
    ((((uint32_t)(v)) << FLASH_ACR_LATENCY_Pos) & FLASH_ACR_LATENCY_Msk)

#define TIKU_STM32_PWR_CR_VOS_SCALE1  (0x03U << PWR_CR_VOS_Pos)
#define TIKU_STM32_PWR_CR_VOS_SCALE2  (0x02U << PWR_CR_VOS_Pos)
#define TIKU_STM32_PWR_CR_VOS_SCALE3  (0x01U << PWR_CR_VOS_Pos)
#define TIKU_STM32_PLLP_DIV2          0x00000000U
#define TIKU_STM32_PLLP_DIV4          RCC_PLLCFGR_PLLP_0

/*---------------------------------------------------------------------------*/
/* Cached clock rates                                                        */
/*---------------------------------------------------------------------------*/

static volatile unsigned long g_sysclk_hz  = STM32F411_HSI_HZ;
static volatile unsigned long g_hclk_hz    = STM32F411_HSI_HZ;
static volatile unsigned long g_pclk1_hz   = STM32F411_HSI_HZ;
static volatile unsigned long g_pclk2_hz   = STM32F411_HSI_HZ;
static volatile uint8_t       g_clock_fault = 0U;

/*---------------------------------------------------------------------------*/
/* Internal helpers                                                          */
/*---------------------------------------------------------------------------*/

static inline void stm32f411_disable_irq(void) {
    __disable_irq();
}

static int stm32f411_spin_until_set(volatile uint32_t *reg, uint32_t mask) {
    uint32_t i = STM32F411_CLOCK_SPIN_TIMEOUT;
    while (i--) {
        if ((*reg & mask) != 0U) {
            return 1;
        }
    }
    return 0;
}

static int stm32f411_spin_until_clear(volatile uint32_t *reg, uint32_t mask) {
    uint32_t i = STM32F411_CLOCK_SPIN_TIMEOUT;
    while (i--) {
        if ((*reg & mask) == 0U) {
            return 1;
        }
    }
    return 0;
}

static int stm32f411_spin_until_value(volatile uint32_t *reg,
                                      uint32_t mask,
                                      uint32_t value) {
    uint32_t i = STM32F411_CLOCK_SPIN_TIMEOUT;
    while (i--) {
        if ((*reg & mask) == value) {
            return 1;
        }
    }
    return 0;
}

static void stm32f411_clock_cache_hsi(void) {
    g_sysclk_hz = STM32F411_HSI_HZ;
    g_hclk_hz   = STM32F411_HSI_HZ;
    g_pclk1_hz  = STM32F411_HSI_HZ;
    g_pclk2_hz  = STM32F411_HSI_HZ;
}

static int stm32f411_hsi_enable(void) {
    RCC->CR |= RCC_CR_HSION;
    return stm32f411_spin_until_set(&RCC->CR, RCC_CR_HSIRDY);
}

static int stm32f411_switch_sysclk_to_hsi(void) {
    uint32_t cfgr;

    if (!stm32f411_hsi_enable()) {
        return 0;
    }

    cfgr = RCC->CFGR;
    cfgr &= ~RCC_CFGR_SW_Msk;
    cfgr |= RCC_CFGR_SW_HSI;
    RCC->CFGR = cfgr;

    return stm32f411_spin_until_value(&RCC->CFGR,
                                      RCC_CFGR_SWS_Msk,
                                      RCC_CFGR_SWS_HSI);
}

static void stm32f411_pll_disable(void) {
    RCC->CR &= ~RCC_CR_PLLON;
    (void)stm32f411_spin_until_clear(&RCC->CR, RCC_CR_PLLRDY);
}

static int stm32f411_flash_configure(uint8_t latency) {
    uint32_t acr = FLASH->ACR;

    acr &= ~FLASH_ACR_LATENCY_Msk;
    acr &= ~FLASH_ACR_PRFTEN;
    acr |= TIKU_STM32_FLASH_ACR_LATENCY(latency)
        | FLASH_ACR_ICEN
        | FLASH_ACR_DCEN;
    FLASH->ACR = acr;

    return stm32f411_spin_until_value(&FLASH->ACR,
                                      FLASH_ACR_LATENCY_Msk,
                                      TIKU_STM32_FLASH_ACR_LATENCY(latency));
}

static void stm32f411_voltage_scale(unsigned int target_mhz) {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;

    /*
        Set voltage scaling based on target frequency.
        The settings are based on those described in the STM32F411xE
        reference manual (RM0383), section 5.4.1, as follows:
        - Scale 1: fCLK > 84 MHz
        - Scale 2: fCLK <= 84 MHz
        - Scale 3: fCLK <= 64 MHz
    */
    switch (target_mhz) {
        case 48U:
            PWR->CR = (PWR->CR & ~PWR_CR_VOS_Msk)
                    | TIKU_STM32_PWR_CR_VOS_SCALE3;
            break;        
        case 84U:
            PWR->CR = (PWR->CR & ~PWR_CR_VOS_Msk)
                    | TIKU_STM32_PWR_CR_VOS_SCALE2;
            break;        
        default:
            PWR->CR = (PWR->CR & ~PWR_CR_VOS_Msk)
                    | TIKU_STM32_PWR_CR_VOS_SCALE1;
            break;
    }

    (void)stm32f411_spin_until_set(&PWR->CSR, PWR_CSR_VOSRDY);
}

static void stm32f411_enable_boot_peripherals(void) {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
}

struct stm32f411_clock_plan {
    unsigned int  target_mhz;
    uint8_t       use_pll;
    uint8_t       pllm;
    uint16_t      plln;
    uint32_t      pllp_bits;
    uint8_t       pllq;
    uint8_t       flash_latency;
    uint32_t      hpre_bits;
    uint32_t      ppre1_bits;
    uint32_t      ppre2_bits;
    unsigned long sysclk_hz;
    unsigned long hclk_hz;
    unsigned long pclk1_hz;
    unsigned long pclk2_hz;
};

static const struct stm32f411_clock_plan stm32f411_clock_plans[] = {
    {
        2U, 0U, 0U, 0U, 0U, 0U, 0U,
        RCC_CFGR_HPRE_DIV8,
        RCC_CFGR_PPRE1_DIV1,
        RCC_CFGR_PPRE2_DIV1,
        2000000UL, 2000000UL, 2000000UL, 2000000UL
    },
    {
        4U, 0U, 0U, 0U, 0U, 0U, 0U,
        RCC_CFGR_HPRE_DIV4,
        RCC_CFGR_PPRE1_DIV1,
        RCC_CFGR_PPRE2_DIV1,
        4000000UL, 4000000UL, 4000000UL, 4000000UL
    },
    {
        8U, 0U, 0U, 0U, 0U, 0U, 0U,
        RCC_CFGR_HPRE_DIV2,
        RCC_CFGR_PPRE1_DIV1,
        RCC_CFGR_PPRE2_DIV1,
        8000000UL, 8000000UL, 8000000UL, 8000000UL
    },
    {
        16U, 0U, 0U, 0U, 0U, 0U, 0U,
        RCC_CFGR_HPRE_DIV1,
        RCC_CFGR_PPRE1_DIV1,
        RCC_CFGR_PPRE2_DIV1,
        16000000UL, 16000000UL, 16000000UL, 16000000UL
    },
    {
        48U, 1U, 16U, 192U, TIKU_STM32_PLLP_DIV4, 4U, 1U,
        RCC_CFGR_HPRE_DIV1,
        RCC_CFGR_PPRE1_DIV1,
        RCC_CFGR_PPRE2_DIV1,
        48000000UL, 48000000UL, 48000000UL, 48000000UL
    },
    {
        84U, 1U, 16U, 336U, TIKU_STM32_PLLP_DIV4, 7U, 2U,
        RCC_CFGR_HPRE_DIV1,
        RCC_CFGR_PPRE1_DIV2,
        RCC_CFGR_PPRE2_DIV1,
        84000000UL, 84000000UL, 42000000UL, 84000000UL
    },
    {
        100U, 1U, 16U, 200U, TIKU_STM32_PLLP_DIV2, 4U, 3U,
        RCC_CFGR_HPRE_DIV1,
        RCC_CFGR_PPRE1_DIV2,
        RCC_CFGR_PPRE2_DIV1,
        100000000UL, 100000000UL, 50000000UL, 100000000UL
    },
};

#define STM32F411_CLOCK_PLAN_COUNT \
    (sizeof(stm32f411_clock_plans) / sizeof(stm32f411_clock_plans[0]))

static const struct stm32f411_clock_plan *
stm32f411_lookup_clock_plan(unsigned int target_mhz, uint8_t *unsupported) {
    unsigned int i;

    for (i = 0U; i < STM32F411_CLOCK_PLAN_COUNT; i++) {
        if (stm32f411_clock_plans[i].target_mhz == target_mhz) {
            *unsupported = 0U;
            return &stm32f411_clock_plans[i];
        }
    }

    *unsupported = 1U;
    return &stm32f411_clock_plans[STM32F411_CLOCK_PLAN_COUNT - 1U];
}

static void stm32f411_update_clock_cache(
    const struct stm32f411_clock_plan *plan) {
    g_sysclk_hz = plan->sysclk_hz;
    g_hclk_hz   = plan->hclk_hz;
    g_pclk1_hz  = plan->pclk1_hz;
    g_pclk2_hz  = plan->pclk2_hz;
}

static void stm32f411_apply_cfgr(const struct stm32f411_clock_plan *plan) {
    uint32_t cfgr = RCC->CFGR;

    cfgr &= ~(RCC_CFGR_HPRE_Msk
            | RCC_CFGR_PPRE1_Msk
            | RCC_CFGR_PPRE2_Msk);
    cfgr |= plan->hpre_bits | plan->ppre1_bits | plan->ppre2_bits;
    RCC->CFGR = cfgr;
}

static void stm32f411_fallback_hsi(void) {
    (void)stm32f411_switch_sysclk_to_hsi();
    stm32f411_pll_disable();
    (void)stm32f411_flash_configure(0U);
    stm32f411_clock_cache_hsi();
    SystemCoreClockUpdate();
    g_clock_fault = 1U;
}

static int stm32f411_apply_pll_plan(
    const struct stm32f411_clock_plan *plan) {
    uint32_t cfgr;

    if (!stm32f411_switch_sysclk_to_hsi()) {
        return 0;
    }

    stm32f411_pll_disable();
    stm32f411_voltage_scale(plan->target_mhz);

    if (!stm32f411_flash_configure(plan->flash_latency)) {
        return 0;
    }

    stm32f411_apply_cfgr(plan);

    RCC->PLLCFGR =
        TIKU_STM32_RCC_PLLCFGR_PLLM(plan->pllm)
        | TIKU_STM32_RCC_PLLCFGR_PLLN(plan->plln)
        | plan->pllp_bits
        | RCC_PLLCFGR_PLLSRC_HSI
        | TIKU_STM32_RCC_PLLCFGR_PLLQ(plan->pllq);

    RCC->CR |= RCC_CR_PLLON;
    if (!stm32f411_spin_until_set(&RCC->CR, RCC_CR_PLLRDY)) {
        return 0;
    }

    cfgr = RCC->CFGR;
    cfgr &= ~RCC_CFGR_SW_Msk;
    cfgr |= RCC_CFGR_SW_PLL;
    RCC->CFGR = cfgr;

    return stm32f411_spin_until_value(&RCC->CFGR,
                                      RCC_CFGR_SWS_Msk,
                                      RCC_CFGR_SWS_PLL);
}

/*---------------------------------------------------------------------------*/
/* Public HAL entry points                                                   */
/*---------------------------------------------------------------------------*/

void tiku_cpu_boot_stm32f411_init(void) {
    stm32f411_disable_irq();
    SystemInit();

    if (!stm32f411_hsi_enable()) {
        g_clock_fault = 1U;
    }

    stm32f411_clock_cache_hsi();
    SystemCoreClockUpdate();
    stm32f411_enable_boot_peripherals();
}

void tiku_cpu_freq_stm32f411_init(unsigned int target_mhz) {
    uint8_t unsupported = 0U;
    const struct stm32f411_clock_plan *plan =
        stm32f411_lookup_clock_plan(target_mhz, &unsupported);

    if (!plan->use_pll) {
        if (!stm32f411_switch_sysclk_to_hsi()) {
            stm32f411_fallback_hsi();
            return;
        }

        stm32f411_pll_disable();
        if (!stm32f411_flash_configure(plan->flash_latency)) {
            g_clock_fault = 1U;
        } else {
            stm32f411_apply_cfgr(plan);
            g_clock_fault = unsupported;
        }
        stm32f411_update_clock_cache(plan);
        SystemCoreClockUpdate();
        return;
    }

    if (!stm32f411_apply_pll_plan(plan)) {
        stm32f411_fallback_hsi();
        return;
    }

    stm32f411_update_clock_cache(plan);
    SystemCoreClockUpdate();
    g_clock_fault = unsupported;
}

void tiku_cpu_boot_stm32f411_power_wfi_enter(void) {
    /* The scheduler calls the idle hook from inside an atomic section.
     * Re-enable IRQs for the sleep window, then return with them masked
     * again so the post-WFI tickless reconcile path still runs inside the
     * same critical section. */
    __asm__ volatile (
        "cpsie i\n"
        "dsb\n"
        "isb\n"
        "wfi\n"
        "cpsid i\n"
        ::: "memory");
}

void tiku_cpu_boot_stm32f411_reset(void) {
    NVIC_SystemReset();
    for (;;) { }
}

unsigned long tiku_cpu_stm32f411_clock_get_hz(void) {
    return g_hclk_hz;
}

unsigned long tiku_cpu_stm32f411_smclk_get_hz(void) {
    return g_pclk1_hz;
}

unsigned long tiku_cpu_stm32f411_aclk_get_hz(void) {
    return 0UL;
}

unsigned long tiku_cpu_stm32f411_pclk1_get_hz(void) {
    return g_pclk1_hz;
}

unsigned long tiku_cpu_stm32f411_pclk2_get_hz(void) {
    return g_pclk2_hz;
}

int tiku_cpu_stm32f411_clock_has_fault(void) {
    if (g_clock_fault) {
        return 1;
    }

    return (RCC->CIR & RCC_CIR_CSSF) ? 1 : 0;
}
