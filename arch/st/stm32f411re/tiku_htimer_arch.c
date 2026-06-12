/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_htimer_arch.c - STM32F411RE TIM5-based htimer backend
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_cpu_freq_boot_arch.h"
#include <kernel/timers/tiku_htimer.h>
#include <stdint.h>
#include <stm32f411xe.h>

static unsigned long stm32f411_tim5_clock_hz(void)
{
    unsigned long hclk = tiku_cpu_stm32f411_clock_get_hz();
    unsigned long pclk1 = tiku_cpu_stm32f411_pclk1_get_hz();

    if (pclk1 == 0UL) {
        return 16000000UL;
    }
    return (pclk1 == hclk) ? pclk1 : (pclk1 * 2UL);
}

void tiku_htimer_arch_init(void)
{
    unsigned long timclk = stm32f411_tim5_clock_hz();
    uint32_t psc;

    RCC->APB1ENR |= RCC_APB1ENR_TIM5EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM5RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM5RST;

    psc = (uint32_t)(timclk / TIKU_HTIMER_ARCH_SECOND);
    if (psc == 0U) {
        psc = 1U;
    }

    TIM5->CR1 = 0U;
    TIM5->PSC = psc - 1U;
    TIM5->ARR = 0xFFFFFFFFUL;
    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR = 0U;
    TIM5->DIER = 0U;
    TIM5->CR1 = TIM_CR1_CEN;

    NVIC_ClearPendingIRQ(TIM5_IRQn);
    NVIC_EnableIRQ(TIM5_IRQn);
}

void tiku_htimer_arch_schedule(tiku_htimer_clock_t t)
{
    TIM5->CCR1 = (uint32_t)t;
    TIM5->SR &= ~TIM_SR_CC1IF;
    TIM5->DIER |= TIM_DIER_CC1IE;
}

tiku_htimer_clock_t tiku_htimer_arch_now(void)
{
    return (tiku_htimer_clock_t)TIM5->CNT;
}

void tiku_stm32f411_tim5_irq_handler(void)
{
    uint32_t sr = TIM5->SR;

    if (sr & TIM_SR_CC1IF) {
        TIM5->SR &= ~TIM_SR_CC1IF;
        tiku_htimer_run_next();
    }
}
