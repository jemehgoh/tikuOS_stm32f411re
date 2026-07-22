/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_wake_arch.c - STM32F411RE backend for the wake-source HAL
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <hal/tiku_wake_hal.h>
#include <stm32f411xe.h>
#include <stdint.h>
#include <string.h>

#ifndef TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
#define TIKU_STM32_TIM2_DEADLINE_EXPERIMENT 0
#endif

static int stm32f411_irq_enabled(uint32_t irq)
{
    return (NVIC->ISER[irq / 32U] & (1UL << (irq & 31U))) ? 1 : 0;
}

void tiku_wake_arch_query(tiku_wake_sources_t *out)
{
    uint32_t imr;
    uint8_t line;

    if (out == 0) {
        return;
    }

    memset(out, 0, sizeof(*out));

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    if (stm32f411_irq_enabled((uint32_t)TIM2_IRQn) &&
        (TIM2->DIER & TIM_DIER_CC1IE)) {
        out->sources |= TIKU_WAKE_SYSTICK;
    }
#else
    if (SysTick->CTRL & SysTick_CTRL_TICKINT_Msk) {
        out->sources |= TIKU_WAKE_SYSTICK;
    }
#endif
    if (stm32f411_irq_enabled((uint32_t)TIM5_IRQn)) {
        out->sources |= TIKU_WAKE_HTIMER;
    }
    if (stm32f411_irq_enabled((uint32_t)USART2_IRQn)) {
        out->sources |= TIKU_WAKE_UART_RX;
    }

    imr = EXTI->IMR;
    if ((imr & 0xFFFFU) != 0U &&
        (stm32f411_irq_enabled((uint32_t)EXTI0_IRQn)
      || stm32f411_irq_enabled((uint32_t)EXTI1_IRQn)
      || stm32f411_irq_enabled((uint32_t)EXTI2_IRQn)
      || stm32f411_irq_enabled((uint32_t)EXTI3_IRQn)
      || stm32f411_irq_enabled((uint32_t)EXTI4_IRQn)
      || stm32f411_irq_enabled((uint32_t)EXTI9_5_IRQn)
      || stm32f411_irq_enabled((uint32_t)EXTI15_10_IRQn))) {
        out->sources |= TIKU_WAKE_GPIO;
    }

    for (line = 0U; line < 8U; line++) {
        uint32_t exticr = SYSCFG->EXTICR[line / 4U];
        uint8_t port = (uint8_t)((exticr >> ((line & 3U) * 4U)) & 0x0FU);
        if ((imr & (1UL << line)) == 0U) {
            continue;
        }
        if (port < TIKU_WAKE_MAX_GPIO_PORTS) {
            out->gpio_ie[port] |= (uint8_t)(1U << line);
        }
    }
}
