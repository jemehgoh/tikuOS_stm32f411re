/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_gpio_irq_arch.c - STM32F411RE GPIO edge interrupt backend
 *
 * Bridges STM32 EXTI lines into TIKU_EVENT_GPIO broadcast events.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <hal/tiku_cpu.h>
#include <hal/tiku_gpio_irq_hal.h>
#include <kernel/process/tiku_process.h>
#include <kernel/vfs/tree/tiku_vfs_tree_gpio.h>
#include "tiku_pinmux_arch.h"
#include <stdint.h>
#include <stm32f411xe.h>

#define TIKU_STM32_GPIO_PUPD_UP     1U
#define TIKU_STM32_EXTI_LINE_COUNT  16U

#define TIKU_STM32_EXTI_MASK_0      (1UL << 0)
#define TIKU_STM32_EXTI_MASK_1      (1UL << 1)
#define TIKU_STM32_EXTI_MASK_2      (1UL << 2)
#define TIKU_STM32_EXTI_MASK_3      (1UL << 3)
#define TIKU_STM32_EXTI_MASK_4      (1UL << 4)
#define TIKU_STM32_EXTI_MASK_9_5    (0x1FUL << 5)
#define TIKU_STM32_EXTI_MASK_15_10  (0x3FUL << 10)

/*
Global arrays to track the configuration of each EXTI line.
May be modified in future to support concurrent API access.
*/
static volatile uint8_t owner_port[TIKU_STM32_EXTI_LINE_COUNT];
static volatile uint8_t owner_pin[TIKU_STM32_EXTI_LINE_COUNT];
static volatile uint16_t exti_enabled_mask;

static int
stm32f411_exti_port_code(uint8_t phys_port, uint8_t *code)
{
    if (code == (uint8_t *)0) {
        return -1;
    }

    switch (phys_port) {
    case 1U:
        *code = 0U;
        return 0;
    case 2U:
        *code = 1U;
        return 0;
    case 3U:
        *code = 2U;
        return 0;
    case 4U:
        *code = 3U;
        return 0;
    default:
        return -1;
    }
}

static IRQn_Type
stm32f411_exti_irqn(uint8_t line)
{
    switch (line) {
    case 0U: return EXTI0_IRQn;
    case 1U: return EXTI1_IRQn;
    case 2U: return EXTI2_IRQn;
    case 3U: return EXTI3_IRQn;
    case 4U: return EXTI4_IRQn;
    case 5U:
    case 6U:
    case 7U:
    case 8U:
    case 9U:
        return EXTI9_5_IRQn;
    default:
        return EXTI15_10_IRQn;
    }
}

static uint32_t
stm32f411_exti_group_mask(IRQn_Type irqn)
{
    switch (irqn) {
    case EXTI0_IRQn:
        return TIKU_STM32_EXTI_MASK_0;
    case EXTI1_IRQn:
        return TIKU_STM32_EXTI_MASK_1;
    case EXTI2_IRQn:
        return TIKU_STM32_EXTI_MASK_2;
    case EXTI3_IRQn:
        return TIKU_STM32_EXTI_MASK_3;
    case EXTI4_IRQn:
        return TIKU_STM32_EXTI_MASK_4;
    case EXTI9_5_IRQn:
        return TIKU_STM32_EXTI_MASK_9_5;
    case EXTI15_10_IRQn:
        return TIKU_STM32_EXTI_MASK_15_10;
    default:
        return 0U;
    }
}

static uint32_t
stm32f411_exti_line_mask(uint8_t line)
{
    return (1UL << line);
}

/*
* Line-specific EXTI dispatch handler.
*/
static void
stm32f411_exti_trace_line(uint8_t line)
{
    uint32_t bit;
    tiku_event_data_t data;

    if (line >= TIKU_STM32_EXTI_LINE_COUNT) {
        return;
    }

    bit = stm32f411_exti_line_mask(line);
    if ((exti_enabled_mask & (uint16_t)bit) == 0U) {
        return;
    }
    if (owner_port[line] == 0U || owner_pin[line] > 7U) {
        return;
    }

    data = (tiku_event_data_t)TIKU_GPIO_IRQ_PACK(owner_port[line],
                                                 owner_pin[line]);
    tiku_process_post(TIKU_PROCESS_BROADCAST, TIKU_EVENT_GPIO, data);
    tiku_vfs_tree_gpio_notify(owner_port[line], owner_pin[line]);
}

/*
Shared EXTI line dispatch for lines 5-9 and 10-15. 
These lines share interrupt vectors, so we need to check which specific line triggered the interrupt.
*/
static void
stm32f411_exti_dispatch_shared(uint32_t pending,
                               uint8_t first_line,
                               uint8_t last_line)
{
    uint8_t line;

    for (line = first_line; line <= last_line; line++) {
        if ((pending & stm32f411_exti_line_mask(line)) == 0U) {
            continue;
        }
        stm32f411_exti_trace_line(line);
    }
}

/*
Dispatch EXTI interrupts for a specific group of lines.
*/
static void
stm32f411_exti_dispatch(uint32_t group_mask)
{
    uint32_t pending;

    pending = EXTI->PR & EXTI->IMR & group_mask;
    if (pending == 0U) {
        return;
    }

    EXTI->PR = pending;

    // Line dispatch handler calls, called by ISR
    // For lines 0-4 (one ISR per line), the line-specific handler is called directly.
    // Otherwise (multiple lines share an ISR), we dispatch to the shared handler.
    switch (group_mask) {
    case TIKU_STM32_EXTI_MASK_0:
        stm32f411_exti_trace_line(0U);
        break;
    case TIKU_STM32_EXTI_MASK_1:
        stm32f411_exti_trace_line(1U);
        break;
    case TIKU_STM32_EXTI_MASK_2:
        stm32f411_exti_trace_line(2U);
        break;
    case TIKU_STM32_EXTI_MASK_3:
        stm32f411_exti_trace_line(3U);
        break;
    case TIKU_STM32_EXTI_MASK_4:
        stm32f411_exti_trace_line(4U);
        break;
    case TIKU_STM32_EXTI_MASK_9_5:
        stm32f411_exti_dispatch_shared(pending, 5U, 9U);
        break;
    case TIKU_STM32_EXTI_MASK_15_10:
        stm32f411_exti_dispatch_shared(pending, 10U, 15U);
        break;
    default:
        break;
    }
}

int
tiku_gpio_irq_arch_enable(uint8_t port, uint8_t pin,
                          tiku_gpio_edge_t edge)
{
    uint8_t phys_port;
    uint8_t phys_pin;
    uint8_t exti_port;
    uint8_t line;
    uint8_t exticr_idx;
    uint8_t exticr_shift;
    uint32_t line_mask;
    uint32_t exticr;
    IRQn_Type irqn;
    uint32_t priority = (1UL << __NVIC_PRIO_BITS) - 1UL;

    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return TIKU_GPIO_IRQ_ERR_INVALID;
    }
    if (stm32f411_exti_port_code(phys_port, &exti_port) != 0) {
        return TIKU_GPIO_IRQ_ERR_INVALID;
    }
    if (edge != TIKU_GPIO_EDGE_RISING &&
        edge != TIKU_GPIO_EDGE_FALLING &&
        edge != TIKU_GPIO_EDGE_BOTH) {
        return TIKU_GPIO_IRQ_ERR_INVALID;
    }

    // Set up the pin as input with pull-up.
    // This is done in line with the MSP430 port's EXTI implementation.
    if (tiku_stm32f411_pinmux_init_input(phys_port, phys_pin,
                                         TIKU_STM32_GPIO_PUPD_UP) != 0) {
        return TIKU_GPIO_IRQ_ERR_INVALID;
    }

    line = phys_pin;
    line_mask = stm32f411_exti_line_mask(line);
    exticr_idx = (uint8_t)(line / 4U);
    exticr_shift = (uint8_t)((line & 3U) * 4U);
    irqn = stm32f411_exti_irqn(line);

    tiku_atomic_enter();

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;

    exticr = SYSCFG->EXTICR[exticr_idx];
    exticr &= ~(0xFUL << exticr_shift);
    exticr |= ((uint32_t)exti_port << exticr_shift);
    SYSCFG->EXTICR[exticr_idx] = exticr;

    EXTI->RTSR &= ~line_mask;
    EXTI->FTSR &= ~line_mask;

    // Setup edge trigger configuration for EXTI line.
    // For the STM32, rising and falling edges are configured independently (with RTSR and FTSR)
    // To support both edges, both bits are set.
    if (edge == TIKU_GPIO_EDGE_RISING || edge == TIKU_GPIO_EDGE_BOTH) {
        EXTI->RTSR |= line_mask;
    }
    if (edge == TIKU_GPIO_EDGE_FALLING || edge == TIKU_GPIO_EDGE_BOTH) {
        EXTI->FTSR |= line_mask;
    }

    EXTI->PR = line_mask;
    EXTI->IMR |= line_mask;

    owner_port[line] = port;
    owner_pin[line] = pin;
    exti_enabled_mask |= (uint16_t)line_mask;

    NVIC_SetPriority(irqn, priority);
    NVIC_ClearPendingIRQ(irqn);
    NVIC_EnableIRQ(irqn);

    tiku_atomic_exit();

    return TIKU_GPIO_IRQ_OK;
}

int
tiku_gpio_irq_arch_disable(uint8_t port, uint8_t pin)
{
    uint8_t phys_port;
    uint8_t phys_pin;
    uint8_t line;
    uint32_t line_mask;
    IRQn_Type irqn;
    uint32_t group_mask;

    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return TIKU_GPIO_IRQ_ERR_INVALID;
    }

    line = phys_pin;
    line_mask = stm32f411_exti_line_mask(line);
    irqn = stm32f411_exti_irqn(line);
    group_mask = stm32f411_exti_group_mask(irqn);

    tiku_atomic_enter();

    if ((exti_enabled_mask & (uint16_t)line_mask) == 0U ||
        owner_port[line] != port ||
        owner_pin[line] != pin) {
        tiku_atomic_exit();
        return TIKU_GPIO_IRQ_OK;
    }

    // Reset EXTI line configuration
    EXTI->IMR &= ~line_mask;
    EXTI->RTSR &= ~line_mask;
    EXTI->FTSR &= ~line_mask;
    EXTI->PR = line_mask;

    owner_port[line] = 0U;
    owner_pin[line] = 0U;
    exti_enabled_mask &= (uint16_t)~line_mask;

    if ((((uint32_t)exti_enabled_mask) & group_mask) == 0U) {
        NVIC_DisableIRQ(irqn);
        NVIC_ClearPendingIRQ(irqn);
    }

    tiku_atomic_exit();

    return TIKU_GPIO_IRQ_OK;
}

void tiku_stm32f411_exti0_irq_handler(void)
{
    stm32f411_exti_dispatch(TIKU_STM32_EXTI_MASK_0);
}

void tiku_stm32f411_exti1_irq_handler(void)
{
    stm32f411_exti_dispatch(TIKU_STM32_EXTI_MASK_1);
}

void tiku_stm32f411_exti2_irq_handler(void)
{
    stm32f411_exti_dispatch(TIKU_STM32_EXTI_MASK_2);
}

void tiku_stm32f411_exti3_irq_handler(void)
{
    stm32f411_exti_dispatch(TIKU_STM32_EXTI_MASK_3);
}

void tiku_stm32f411_exti4_irq_handler(void)
{
    stm32f411_exti_dispatch(TIKU_STM32_EXTI_MASK_4);
}

void tiku_stm32f411_exti9_5_irq_handler(void)
{
    stm32f411_exti_dispatch(TIKU_STM32_EXTI_MASK_9_5);
}

void tiku_stm32f411_exti15_10_irq_handler(void)
{
    stm32f411_exti_dispatch(TIKU_STM32_EXTI_MASK_15_10);
}
