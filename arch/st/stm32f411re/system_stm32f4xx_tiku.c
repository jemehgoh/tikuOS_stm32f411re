/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * system_stm32f4xx_tiku.c - Tiku-adapted CMSIS system support for STM32F411RE
 *
 * Adapted from ST's CMSIS STM32F4 system template. This version keeps the
 * standard CMSIS ownership of SystemInit/SystemCoreClock/SystemCoreClockUpdate
 * while preserving Tiku's explicit vector-table relocation and HSI-first boot
 * policy.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stm32f4xx.h>

#ifndef HSE_VALUE
#define HSE_VALUE    ((uint32_t)8000000UL)
#endif

#ifndef HSI_VALUE
#define HSI_VALUE    ((uint32_t)16000000UL)
#endif

extern uint32_t __vectors_start;

uint32_t SystemCoreClock = HSI_VALUE;
const uint8_t AHBPrescTable[16] = {
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U
};
const uint8_t APBPrescTable[8] = {
    0U, 0U, 0U, 0U, 1U, 2U, 3U, 4U
};

void SystemInit(void)
{
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    SCB->CPACR |= ((3UL << (10U * 2U)) | (3UL << (11U * 2U)));
#endif

    /* Tiku's linker script owns the active vector table location. */
    SCB->VTOR = (uint32_t)(uintptr_t)&__vectors_start;
    __DSB();
    __ISB();

    SystemCoreClock = HSI_VALUE;
}

void SystemCoreClockUpdate(void)
{
    uint32_t tmp;
    uint32_t pllvco;
    uint32_t pllp;
    uint32_t pllsource;
    uint32_t pllm;

    tmp = RCC->CFGR & RCC_CFGR_SWS;

    switch (tmp) {
    case RCC_CFGR_SWS_HSI:
        SystemCoreClock = HSI_VALUE;
        break;

    case RCC_CFGR_SWS_HSE:
        SystemCoreClock = HSE_VALUE;
        break;

    case RCC_CFGR_SWS_PLL:
        pllsource = (RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) >> 22;
        pllm = RCC->PLLCFGR & RCC_PLLCFGR_PLLM;

        if (pllm == 0U) {
            SystemCoreClock = HSI_VALUE;
            break;
        }

        if (pllsource != 0U) {
            pllvco = (HSE_VALUE / pllm)
                   * ((RCC->PLLCFGR & RCC_PLLCFGR_PLLN) >> 6);
        } else {
            pllvco = (HSI_VALUE / pllm)
                   * ((RCC->PLLCFGR & RCC_PLLCFGR_PLLN) >> 6);
        }

        pllp = (((RCC->PLLCFGR & RCC_PLLCFGR_PLLP) >> 16) + 1U) * 2U;
        SystemCoreClock = pllvco / pllp;
        break;

    default:
        SystemCoreClock = HSI_VALUE;
        break;
    }

    tmp = AHBPrescTable[(RCC->CFGR & RCC_CFGR_HPRE) >> 4];
    SystemCoreClock >>= tmp;
}
