/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_timer_arch.c - STM32F411RE system tick and tickless backend
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_timer_arch.h"
#include "tiku_cpu_freq_boot_arch.h"
#include <kernel/scheduler/tiku_sched.h>
#include <kernel/timers/tiku_clock.h>
#include <kernel/timers/tiku_timer.h>
#include <stdint.h>
#include <stm32f411xe.h>

/*---------------------------------------------------------------------------*/
/* Configuration                                                             */
/*---------------------------------------------------------------------------*/

#define TIKU_STM32_TICKLESS_NVIC_PRIO  1U

#ifndef TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
#define TIKU_STM32_TIM2_DEADLINE_EXPERIMENT 0
#endif

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
#define TIKU_STM32_UNUSED __attribute__((unused))
#endif

/*---------------------------------------------------------------------------*/
/* State                                                                     */
/*---------------------------------------------------------------------------*/

static volatile uint64_t       g_tick_count = 0ULL;
static volatile unsigned long  g_seconds = 0UL;
static volatile unsigned int   g_subsec_ticks = 0U;
static volatile uint32_t       g_tick_remainder = 0U;
static volatile uint8_t        g_in_tickless_sleep = 0U;
static volatile uint32_t       g_tickless_counts_per_tick = 0U;
static volatile uint32_t       g_tickless_core_cycles_per_count = 0U;
static volatile uint32_t       g_tickless_target_counts = 0U;
static volatile uint32_t       g_tickless_entry_counts = 0U;

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
static volatile uint8_t        g_tim2_cc1_armed = 0U;
static volatile tiku_clock_time_t g_tim2_cc1_deadline16 = 0U;
static volatile uint16_t       g_tim2_cc1_missed_writes = 0U;
#endif

/*---------------------------------------------------------------------------*/
/* Internal helpers                                                          */
/*---------------------------------------------------------------------------*/

static unsigned long stm32f411_tim2_clock_hz(void)
{
    unsigned long hclk = tiku_cpu_stm32f411_clock_get_hz();
    unsigned long pclk1 = tiku_cpu_stm32f411_pclk1_get_hz();

    if (pclk1 == 0UL) {
        return 16000000UL;
    }
    return (pclk1 == hclk) ? pclk1 : (pclk1 * 2UL);
}

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
static uint32_t stm32f411_tim2_psc_for_128hz(unsigned long timclk)
{
    uint32_t psc;

    psc = (uint32_t)(timclk / (unsigned long)TIKU_CLOCK_ARCH_SECOND);
    if (psc == 0U) {
        psc = 1U;
    }

    /*
     * The event-driven experiment intentionally preserves the public
     * 128 ticks/s resolution. If the APB1 timer clock is not an exact
     * multiple of 128 Hz, this integer prescaler is the closest lower
     * hardware divider and the final implementation should decide whether
     * to reject that clock plan or account for the small rate error.
     */
    return psc;
}

static void stm32f411_tim2_freerun_init(void)
{
    unsigned long timclk = stm32f411_tim2_clock_hz();
    uint32_t psc = stm32f411_tim2_psc_for_128hz(timclk);

    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM2RST;

    TIM2->CR1 = 0U;
    TIM2->PSC = psc - 1U;
    TIM2->ARR = 0xFFFFFFFFUL;
    TIM2->CNT = 0U;
    TIM2->CCR1 = 0U;
    TIM2->DIER = 0U;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0U;
    TIM2->CR1 = TIM_CR1_CEN;

    NVIC_SetPriority(TIM2_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
}

static inline TIKU_STM32_UNUSED uint32_t stm32f411_tim2_now32(void)
{
    return TIM2->CNT;
}

static inline TIKU_STM32_UNUSED int
stm32f411_tim2_reached(uint32_t now, uint32_t target)
{
    return ((int32_t)(now - target)) >= 0;
}

static inline TIKU_STM32_UNUSED int
stm32f411_tim2_before(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b)) < 0;
}

static uint32_t stm32f411_tim2_expand_deadline16(tiku_clock_time_t deadline16)
{
    uint32_t now32 = stm32f411_tim2_now32();
    tiku_clock_time_t now16 = (tiku_clock_time_t)now32;
    tiku_clock_time_t delay16 = (tiku_clock_time_t)(deadline16 - now16);

    return now32 + (uint32_t)delay16;
}

static TIKU_STM32_UNUSED int stm32f411_tim2_arm_cc1(uint32_t target)
{
    uint32_t now;

    g_tim2_cc1_armed = 0U;
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    TIM2->CCR1 = target;
    TIM2->SR = (uint32_t)~TIM_SR_CC1IF;
    TIM2->DIER |= TIM_DIER_CC1IE;

    now = stm32f411_tim2_now32();
    if (stm32f411_tim2_reached(now, target)) {
        TIM2->DIER &= ~TIM_DIER_CC1IE;
        TIM2->SR = (uint32_t)~TIM_SR_CC1IF;
        g_tim2_cc1_missed_writes++;
        return 1;
    }

    g_tim2_cc1_armed = 1U;
    return 0;
}

static void stm32f411_tim2_disarm_cc1(void)
{
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    TIM2->SR = (uint32_t)~TIM_SR_CC1IF;
    g_tim2_cc1_armed = 0U;
}
#endif

#if !TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
static uint32_t stm32f411_tickless_counts_per_tick(unsigned long timclk)
{
    uint32_t counts;
    uint32_t min_counts;
    unsigned long base_hz;

    base_hz = timclk / (unsigned long)TIKU_CLOCK_ARCH_SECOND;
    if (base_hz == 0UL) {
        return 1U;
    }

    min_counts = (uint32_t)((base_hz + 65535UL) / 65536UL);
    if (min_counts == 0U) {
        min_counts = 1U;
    }

    for (counts = min_counts; counts <= (uint32_t)base_hz; counts++) {
        if ((base_hz % counts) == 0UL) {
            return counts;
        }
    }

    return (uint32_t)base_hz;
}
#endif

// Update the total tick count and sub-second ticks, and notify the scheduler of a tick event
static void stm32f411_tick_advance_n(uint32_t n_ticks)
{
    if (n_ticks == 0U) {
        return;
    }

    g_tick_count += (uint64_t)n_ticks;
    g_subsec_ticks += n_ticks;

    if (g_subsec_ticks >= TIKU_CLOCK_ARCH_SECOND) {
        g_seconds += g_subsec_ticks / TIKU_CLOCK_ARCH_SECOND;
        g_subsec_ticks = g_subsec_ticks % TIKU_CLOCK_ARCH_SECOND;
    }

    tiku_sched_notify();
}

#if !TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
static uint32_t stm32f411_systick_partial_counts(void)
{
    uint32_t elapsed_cycles;
    uint32_t reload = SysTick->LOAD + 1U;

    if (g_tickless_core_cycles_per_count == 0U || reload == 0U) {
        return 0U;
    }

    elapsed_cycles = reload - SysTick->VAL;
    return elapsed_cycles / g_tickless_core_cycles_per_count;
}

// Stop TIM2 for tickless idle exit (upon event trigger)
static void stm32f411_tickless_stop_tim2(void)
{
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->CNT = 0U;
}

// Set up TIM2 for tickless idle, counting down to the next timer deadline
static void stm32f411_tickless_arm_tim2(uint32_t counts_needed)
{
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->CNT = 0U;
    TIM2->ARR = counts_needed;
    TIM2->SR = 0U;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0U;
    TIM2->CR1 = TIM_CR1_OPM | TIM_CR1_CEN;
}

// Updates the elapsed tick count upon exiting tickless idle
static void stm32f411_tickless_reconcile(void)
{
    uint32_t counts_elapsed;
    uint32_t total_counts;
    uint32_t whole_ticks;

    if (!g_in_tickless_sleep) {
        return;
    }

    g_in_tickless_sleep = 0U;

    if ((TIM2->CR1 & TIM_CR1_CEN) == 0U) {
        counts_elapsed = g_tickless_target_counts;
    } else {
        counts_elapsed = TIM2->CNT;
    }

    stm32f411_tickless_stop_tim2();

    total_counts = counts_elapsed + g_tick_remainder + g_tickless_entry_counts;
    whole_ticks = total_counts / g_tickless_counts_per_tick;
    g_tick_remainder = total_counts % g_tickless_counts_per_tick;
    g_tickless_target_counts = 0U;
    g_tickless_entry_counts = 0U;

    SysTick->VAL = 0U;
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;

    if (whole_ticks != 0U) {
        stm32f411_tick_advance_n(whole_ticks);
    }
}
#endif

/*---------------------------------------------------------------------------*/
/* HAL                                                                       */
/*---------------------------------------------------------------------------*/

void tiku_clock_arch_init(void)
{
    unsigned long hclk = tiku_cpu_stm32f411_clock_get_hz();
#if !TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    unsigned long timclk;
    uint32_t psc_div;
#endif
    uint32_t reload;

    g_tick_count = 0ULL;
    g_seconds = 0UL;
    g_subsec_ticks = 0U;
    g_tick_remainder = 0U;
    g_in_tickless_sleep = 0U;
    g_tickless_target_counts = 0U;
    g_tickless_entry_counts = 0U;
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    g_tim2_cc1_armed = 0U;
    g_tim2_cc1_deadline16 = 0U;
    g_tim2_cc1_missed_writes = 0U;
#endif

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

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    stm32f411_tim2_freerun_init();

    NVIC_SetPriority(SysTick_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
#else
    // Compute tickless timer parameters - counts/tick and prescaler
    // This is necessary as the maximum tick interval for a 32-bit timer in the STM32F411 
    // is shorter than the system clock tick.
    timclk = stm32f411_tim2_clock_hz();
    g_tickless_counts_per_tick = stm32f411_tickless_counts_per_tick(timclk);
    psc_div = (uint32_t)(timclk /
              ((unsigned long)TIKU_CLOCK_ARCH_SECOND *
               (unsigned long)g_tickless_counts_per_tick));
    if (psc_div == 0U) {
        psc_div = 1U;
    }
    g_tickless_core_cycles_per_count = (reload + 1U) / g_tickless_counts_per_tick;

    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM2RST;

    // Set up TIM2 in one-pulse mode for tickless idle
    // This allows the timer to be automatically cleared once it reaches the deadline
    TIM2->CR1 = TIM_CR1_OPM;
    TIM2->PSC = psc_div - 1U;
    TIM2->ARR = 0xFFFFFFFFUL;
    TIM2->CNT = 0U;
    TIM2->DIER = TIM_DIER_UIE;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0U;

    NVIC_SetPriority(SysTick_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
    NVIC_SetPriority(TIM2_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
#endif
}

tiku_clock_arch_time_t tiku_clock_arch_time(void)
{
    return (tiku_clock_arch_time_t)g_tick_count;
}

unsigned long tiku_clock_arch_seconds(void)
{
    return g_seconds;
}

void tiku_clock_arch_set_seconds(unsigned long sec)
{
    g_seconds = sec;
}

void tiku_clock_arch_wait(tiku_clock_arch_time_t t)
{
    tiku_clock_arch_time_t target = tiku_clock_arch_time() + t;

    while ((tiku_clock_arch_time_t)(target - tiku_clock_arch_time()) > 0UL) {
        /* spin */
    }
}

void tiku_clock_arch_delay(unsigned int us)
{
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

unsigned short tiku_clock_arch_fine(void)
{
    uint32_t cvr = SysTick->VAL;
    uint32_t rvr = SysTick->LOAD;
    uint32_t fine;

    if (rvr == 0U) {
        return 0U;
    }

    fine = ((rvr - cvr) * 0xFFFFU) / rvr;
    return (unsigned short)fine;
}

int tiku_clock_arch_fine_max(void)
{
    return 0xFFFF;
}

unsigned char tiku_clock_arch_fault(void)
{
    return 0U;
}

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
void tiku_timer_arch_rearm(tiku_clock_time_t next, uint8_t armed)
{
    uint32_t target;

    if (!armed) {
        g_tim2_cc1_deadline16 = 0U;
        stm32f411_tim2_disarm_cc1();
        return;
    }

    g_tim2_cc1_deadline16 = next;
    target = stm32f411_tim2_expand_deadline16(next);
    if (stm32f411_tim2_arm_cc1(target)) {
        tiku_timer_request_poll();
    }
}
#endif

/*---------------------------------------------------------------------------*/
/* Tickless hooks                                                            */
/*---------------------------------------------------------------------------*/

int tiku_clock_tickless_begin(tiku_clock_time_t ticks_ahead)
{
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    (void)ticks_ahead;
    return 0;
#else
    uint32_t counts_needed;

    if (ticks_ahead <= 1u || g_tickless_counts_per_tick == 0U) {
        return 0;
    }

    g_tickless_entry_counts = stm32f411_systick_partial_counts();
    counts_needed = ((uint32_t)ticks_ahead * g_tickless_counts_per_tick)
                  - (g_tick_remainder + g_tickless_entry_counts);

    if (counts_needed == 0U) {
        counts_needed = 1U;
    }

    SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;

    // Start idle timer (TIM2) to track next timer deadline
    g_tickless_target_counts = counts_needed;
    g_in_tickless_sleep = 1U;
    stm32f411_tickless_arm_tim2(counts_needed);
    return 1;
#endif
}

void tiku_clock_tickless_end(void)
{
#if !TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    stm32f411_tickless_reconcile();
#endif
}

int tiku_clock_tickless_available(void)
{
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    return 0;
#else
    return 1;
#endif
}

/*---------------------------------------------------------------------------*/
/* ISRs                                                                      */
/*---------------------------------------------------------------------------*/

void tiku_stm32f411_systick_handler(void)
{
    stm32f411_tick_advance_n(1U);
}

void tiku_stm32f411_tim2_irq_handler(void)
{
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    uint32_t sr = TIM2->SR;

    if (sr & TIM_SR_CC1IF) {
        stm32f411_tim2_disarm_cc1();
        tiku_timer_request_poll();
    }
#else
    TIM2->SR = 0U;
    stm32f411_tickless_reconcile();
#endif
}
