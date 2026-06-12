/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_timer_arch.c - STM32F411RE system tick
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_timer_arch.h"
#include "tiku_cpu_freq_boot_arch.h"
#include <kernel/scheduler/tiku_sched.h>
#include <stdint.h>
#include <stm32f411xe.h>

/*---------------------------------------------------------------------------*/
/* State                                                                     */
/*---------------------------------------------------------------------------*/

static volatile tiku_clock_arch_time_t g_ticks   = 0UL;
static volatile unsigned long          g_seconds = 0UL;

/*---------------------------------------------------------------------------*/
/* HAL                                                                       */
/*---------------------------------------------------------------------------*/

void tiku_clock_arch_init(void) {
    unsigned long hclk = tiku_cpu_stm32f411_clock_get_hz();
    uint32_t reload;

    g_ticks = 0UL;
    g_seconds = 0UL;

    if (hclk == 0UL) {
        hclk = TIKU_MAIN_CPU_HZ;
    }

    reload = (uint32_t)(hclk / TIKU_CLOCK_ARCH_SECOND);
    if (reload == 0U) {
        reload = 1U;
    }
    reload -= 1U;

    if (reload > 0x00FFFFFFU) {
        reload = 0x00FFFFFFU;
    }

    SysTick->LOAD = reload;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_ENABLE_Msk
                  | SysTick_CTRL_TICKINT_Msk
                  | SysTick_CTRL_CLKSOURCE_Msk;
}

tiku_clock_arch_time_t tiku_clock_arch_time(void) {
    return g_ticks;
}

unsigned long tiku_clock_arch_seconds(void) {
    return g_seconds;
}

void tiku_clock_arch_set_seconds(unsigned long sec) {
    g_seconds = sec;
}

void tiku_clock_arch_wait(tiku_clock_arch_time_t t) {
    tiku_clock_arch_time_t target = g_ticks + t;
    while ((tiku_clock_arch_time_t)(target - g_ticks) > 0UL) {
        /* spin */
    }
}

void tiku_clock_arch_delay(unsigned int us) {
    unsigned long hclk = tiku_cpu_stm32f411_clock_get_hz();
    unsigned long loops;

    if (hclk == 0UL) {
        hclk = TIKU_MAIN_CPU_HZ;
    }

    loops = (hclk / 3000000UL) * (unsigned long)us;
    if (loops == 0UL) {
        loops = us;
    }

    while (loops--) {
        __asm__ volatile ("nop");
    }
}

unsigned short tiku_clock_arch_fine(void) {
    uint32_t cvr = SysTick->VAL;
    uint32_t rvr = SysTick->LOAD;
    uint32_t fine;

    if (rvr == 0U) {
        return 0U;
    }

    fine = ((rvr - cvr) * 0xFFFFU) / rvr;
    return (unsigned short)fine;
}

int tiku_clock_arch_fine_max(void) {
    return 0xFFFF;
}

unsigned char tiku_clock_arch_fault(void) {
    return 0U;
}

/*---------------------------------------------------------------------------*/
/* SysTick ISR                                                               */
/*---------------------------------------------------------------------------*/

void tiku_stm32f411_systick_handler(void) {
    g_ticks++;
    if ((g_ticks % TIKU_CLOCK_ARCH_SECOND) == 0UL) {
        g_seconds++;
    }
    tiku_sched_notify();
}
