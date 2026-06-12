/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_crit_arch.c - STM32F411RE IRQ-mask backend for tiku_crit
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <hal/tiku_crit_hal.h>
#include <kernel/timers/tiku_crit.h>
#include <stm32f411xe.h>
#include <stdint.h>

struct stm32f411_crit_state {
    uint32_t iser0_saved;
    uint32_t iser1_saved;
    uint32_t iser2_saved;
    uint32_t syst_csr_saved;
};

static struct stm32f411_crit_state crit_state;

static void stm32f411_irq_keep(uint32_t irq, uint32_t *w0,
                               uint32_t *w1, uint32_t *w2)
{
    if (irq < 32U) {
        *w0 |= (1UL << irq);
    } else if (irq < 64U) {
        *w1 |= (1UL << (irq - 32U));
    } else if (irq < 96U) {
        *w2 |= (1UL << (irq - 64U));
    }
}

void tiku_crit_arch_mask_irqs(uint8_t preserve_mask)
{
    uint32_t keep0 = 0U;
    uint32_t keep1 = 0U;
    uint32_t keep2 = 0U;
    uint32_t mask;

    crit_state.iser0_saved = NVIC->ISER[0];
    crit_state.iser1_saved = NVIC->ISER[1];
    crit_state.iser2_saved = NVIC->ISER[2];
    crit_state.syst_csr_saved = SysTick->CTRL;

    if (preserve_mask & TIKU_CRIT_PRESERVE_HTIMER) {
        stm32f411_irq_keep((uint32_t)TIM5_IRQn, &keep0, &keep1, &keep2);
    }
    if (preserve_mask & TIKU_CRIT_PRESERVE_UART) {
        stm32f411_irq_keep((uint32_t)USART2_IRQn, &keep0, &keep1, &keep2);
    }
    if (preserve_mask & TIKU_CRIT_PRESERVE_GPIO) {
        stm32f411_irq_keep((uint32_t)EXTI0_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)EXTI1_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)EXTI2_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)EXTI3_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)EXTI4_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)EXTI9_5_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)EXTI15_10_IRQn, &keep0, &keep1, &keep2);
    }
    if (preserve_mask & TIKU_CRIT_PRESERVE_ADC) {
        stm32f411_irq_keep((uint32_t)ADC_IRQn, &keep0, &keep1, &keep2);
    }
    if (preserve_mask & TIKU_CRIT_PRESERVE_I2C) {
        stm32f411_irq_keep((uint32_t)I2C1_EV_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)I2C1_ER_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)I2C2_EV_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)I2C2_ER_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)I2C3_EV_IRQn, &keep0, &keep1, &keep2);
        stm32f411_irq_keep((uint32_t)I2C3_ER_IRQn, &keep0, &keep1, &keep2);
    }

    if ((preserve_mask & TIKU_CRIT_PRESERVE_TICK) == 0U) {
        SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;
    }

    mask = crit_state.iser0_saved & ~keep0;
    if (mask != 0U) {
        NVIC->ICER[0] = mask;
    }
    mask = crit_state.iser1_saved & ~keep1;
    if (mask != 0U) {
        NVIC->ICER[1] = mask;
    }
    mask = crit_state.iser2_saved & ~keep2;
    if (mask != 0U) {
        NVIC->ICER[2] = mask;
    }

    __DSB();
    __ISB();
}

void tiku_crit_arch_unmask_irqs(void)
{
    SysTick->CTRL = crit_state.syst_csr_saved;
    NVIC->ISER[0] = crit_state.iser0_saved;
    NVIC->ISER[1] = crit_state.iser1_saved;
    NVIC->ISER[2] = crit_state.iser2_saved;

    __DSB();
    __ISB();
}
