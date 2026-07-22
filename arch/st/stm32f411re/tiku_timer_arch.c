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
#include <hal/tiku_cpu.h>
#include <kernel/cpu/tiku_hang.h>
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
#define TIKU_STM32_TIM2_DEADLINE_EXPERIMENT 1
#endif

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
#define TIKU_STM32_UNUSED __attribute__((unused))
#define TIKU_STM32_CC_SRC_NONE   0U
#define TIKU_STM32_CC_SRC_TIMER  1U
#define TIKU_STM32_CC_SRC_HANG   2U
#define TIKU_STM32_DEADLINE16_SAFE_TICKS 0x8000U
#define TIKU_STM32_DEADLINE16_SAFE_SECONDS \
    (TIKU_STM32_DEADLINE16_SAFE_TICKS / TIKU_CLOCK_ARCH_SECOND)

#if TIKU_CLOCK_ARCH_SECOND == 0
#error "TIKU_CLOCK_ARCH_SECOND must be nonzero"
#endif

/*
 * TIM2 free-runs using the same scaled prescaler layout as the legacy
 * tickless-idle path: several hardware timer counts make up one public
 * tikuOS tick when the APB1 timer clock cannot be divided directly to the
 * requested public tick rate. Software timer deadlines remain the public
 * 16-bit tiku_clock_time_t values, so modular ordering is only unambiguous
 * across half that public range: 32768 ticks (256 s at 128 Hz). Longer single
 * delays need a chained software timer or a wider public clock type.
 */
typedef char stm32f411_public_clock_must_be_16_bit[
    (sizeof(tiku_clock_time_t) == sizeof(uint16_t)) ? 1 : -1];
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
static volatile uint8_t        g_tim2_cc1_source = TIKU_STM32_CC_SRC_NONE;
static volatile uint8_t        g_tim2_timer_armed = 0U;
static volatile uint8_t        g_tim2_timer_poll_pending = 0U;
static volatile tiku_clock_time_t g_tim2_timer_deadline16 = 0U;
static volatile uint8_t        g_tim2_hang_armed = 0U;
static volatile tiku_clock_time_t g_tim2_hang_deadline16 = 0U;
static volatile uint16_t       g_tim2_cc1_missed_writes = 0U;
static volatile uint32_t       g_tim2_last_count32 = 0U;
static volatile unsigned long  g_tim2_seconds_offset = 0UL;
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

static uint32_t stm32f411_tim2_counts_per_tick(unsigned long timclk)
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

static uint32_t stm32f411_tim2_prescaler_div(unsigned long timclk,
                                             uint32_t counts_per_tick)
{
    uint32_t psc_div;

    psc_div = (uint32_t)(timclk /
              ((unsigned long)TIKU_CLOCK_ARCH_SECOND *
               (unsigned long)counts_per_tick));
    if (psc_div == 0U) {
        psc_div = 1U;
    }

    return psc_div;
}

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
static void stm32f411_tim2_freerun_init(void)
{
    unsigned long timclk = stm32f411_tim2_clock_hz();
    uint32_t psc_div;

    g_tickless_counts_per_tick = stm32f411_tim2_counts_per_tick(timclk);
    psc_div = stm32f411_tim2_prescaler_div(timclk,
                                           g_tickless_counts_per_tick);

    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM2RST;

    TIM2->CR1 = 0U;
    TIM2->PSC = psc_div - 1U;
    TIM2->ARR = 0xFFFFFFFFUL;
    TIM2->CNT = 0U;
    /* TIM2 CC1 is reserved here for the system deadline scheduler. */
    TIM2->CCR1 = 0U;
    TIM2->DIER = 0U;
    TIM2->EGR = TIM_EGR_UG;
    /* Peripheral was just reset and CC interrupts are disabled. */
    TIM2->SR = 0U;
    TIM2->CR1 = TIM_CR1_CEN;
    g_tim2_last_count32 = 0U;

    NVIC_SetPriority(TIM2_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
}

static inline TIKU_STM32_UNUSED uint32_t stm32f411_tim2_now32(void)
{
    return TIM2->CNT;
}

static uint64_t stm32f411_tim2_sync_ticks_locked(void)
{
    uint32_t now32 = stm32f411_tim2_now32();
    uint32_t counts_per_tick = g_tickless_counts_per_tick;
    uint32_t elapsed_counts = now32 - g_tim2_last_count32;
    uint64_t total_counts;

    if (counts_per_tick == 0U) {
        counts_per_tick = 1U;
    }

    total_counts = (uint64_t)g_tick_remainder + elapsed_counts;
    g_tick_count += total_counts / counts_per_tick;
    g_tick_remainder = (uint32_t)(total_counts % counts_per_tick);
    g_tim2_last_count32 = now32;

    return g_tick_count;
}

static uint64_t stm32f411_tim2_ticks64(void)
{
    uint64_t ticks;

    tiku_atomic_enter();
    ticks = stm32f411_tim2_sync_ticks_locked();
    tiku_atomic_exit();

    return ticks;
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

static inline int
stm32f411_clock16_before(tiku_clock_time_t a, tiku_clock_time_t b)
{
    return TIKU_CLOCK_LT(a, b);
}

static inline void
stm32f411_tim2_clear_cc1if(void)
{
    TIM2->SR = (uint32_t)~TIM_SR_CC1IF;
}

static uint32_t stm32f411_tim2_expand_deadline16(tiku_clock_time_t deadline16)
{
    uint64_t now_ticks = stm32f411_tim2_sync_ticks_locked();
    tiku_clock_time_t now16 = (tiku_clock_time_t)now_ticks;
    uint32_t counts_per_tick = g_tickless_counts_per_tick;
    tiku_clock_time_t delay16;
    uint64_t delay_counts;

    if (counts_per_tick == 0U) {
        counts_per_tick = 1U;
    }

    if (!stm32f411_clock16_before(now16, deadline16)) {
        return g_tim2_last_count32;
    }

    delay16 = (tiku_clock_time_t)(deadline16 - now16);
    delay_counts = ((uint64_t)delay16 * counts_per_tick) - g_tick_remainder;
    if (delay_counts == 0ULL) {
        delay_counts = 1ULL;
    }

    return g_tim2_last_count32 + (uint32_t)delay_counts;
}

/*
 * CC1 helpers run either from TIM2 ISR context or with tiku_atomic_enter()
 * held by a public arch rearm hook. Do not call them from unlocked
 * thread-mode code: the DIER/CCR/SR sequence is the critical compare update.
 */
static TIKU_STM32_UNUSED int stm32f411_tim2_arm_cc1(uint32_t target)
{
    uint32_t now;

    g_tim2_cc1_armed = 0U;
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    TIM2->CCR1 = target;
    stm32f411_tim2_clear_cc1if();
    TIM2->DIER |= TIM_DIER_CC1IE;

    now = stm32f411_tim2_now32();
    if (stm32f411_tim2_reached(now, target)) {
        TIM2->DIER &= ~TIM_DIER_CC1IE;
        stm32f411_tim2_clear_cc1if();
        g_tim2_cc1_missed_writes++;
        return 1;
    }

    g_tim2_cc1_armed = 1U;
    return 0;
}

static void stm32f411_tim2_disarm_cc1(void)
{
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    stm32f411_tim2_clear_cc1if();
    g_tim2_cc1_armed = 0U;
    g_tim2_cc1_source = TIKU_STM32_CC_SRC_NONE;
}

/*
 * Pick the nearest logical deadline and program TIM2 CC1. Caller is serialized
 * against thread-mode rearm hooks; service actions returned from here must run
 * after leaving the critical section.
 */
static uint8_t stm32f411_tim2_rearm_cc1_mux_locked(void)
{
    uint8_t actions = 0U;

    for (;;) {
        uint8_t source = TIKU_STM32_CC_SRC_NONE;
        tiku_clock_time_t deadline16 = 0U;
        uint32_t target;

        if (g_tim2_timer_armed && !g_tim2_timer_poll_pending) {
            source = TIKU_STM32_CC_SRC_TIMER;
            deadline16 = g_tim2_timer_deadline16;
        }

        if (g_tim2_hang_armed &&
            (source == TIKU_STM32_CC_SRC_NONE ||
             !stm32f411_clock16_before(deadline16,
                                       g_tim2_hang_deadline16))) {
            source = TIKU_STM32_CC_SRC_HANG;
            deadline16 = g_tim2_hang_deadline16;
        }

        if (source == TIKU_STM32_CC_SRC_NONE) {
            stm32f411_tim2_disarm_cc1();
            return actions;
        }

        target = stm32f411_tim2_expand_deadline16(deadline16);
        g_tim2_cc1_source = source;
        if (!stm32f411_tim2_arm_cc1(target)) {
            return actions;
        }

        actions |= source;
        if (source == TIKU_STM32_CC_SRC_TIMER) {
            g_tim2_timer_poll_pending = 1U;
            continue;
        }

        g_tim2_hang_armed = 0U;
        stm32f411_tim2_disarm_cc1();
        return actions;
    }
}

static void stm32f411_tim2_service_actions(uint8_t actions)
{
    /*
     * Runs after the CC1 critical section. Timer deadlines only wake the timer
     * process; software timer callbacks remain process-context dispatch.
     */
    if (actions & TIKU_STM32_CC_SRC_HANG) {
        tiku_hang_deadline_expired();
    }
    if (actions & TIKU_STM32_CC_SRC_TIMER) {
        tiku_timer_request_poll();
    }
}
#endif

#if !TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
static inline void stm32f411_tim2_clear_uif(void)
{
    TIM2->SR = (uint32_t)~TIM_SR_UIF;
}

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
    stm32f411_tim2_clear_uif();
    TIM2->EGR = TIM_EGR_UG;
    stm32f411_tim2_clear_uif();
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
    g_tickless_counts_per_tick = 0U;
    g_in_tickless_sleep = 0U;
    g_tickless_target_counts = 0U;
    g_tickless_entry_counts = 0U;
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    g_tim2_cc1_armed = 0U;
    g_tim2_cc1_source = TIKU_STM32_CC_SRC_NONE;
    g_tim2_timer_armed = 0U;
    g_tim2_timer_poll_pending = 0U;
    g_tim2_timer_deadline16 = 0U;
    g_tim2_hang_armed = 0U;
    g_tim2_hang_deadline16 = 0U;
    g_tim2_cc1_missed_writes = 0U;
    g_tim2_last_count32 = 0U;
    g_tim2_seconds_offset = 0UL;
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

#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    SysTick->CTRL = 0U;
    stm32f411_tim2_freerun_init();

    NVIC_SetPriority(SysTick_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
#else
    SysTick->CTRL = SysTick_CTRL_ENABLE_Msk
                  | SysTick_CTRL_TICKINT_Msk
                  | SysTick_CTRL_CLKSOURCE_Msk;

    // Compute tickless timer parameters - counts/tick and prescaler
    // This is necessary as the maximum tick interval for a 32-bit timer in the STM32F411 
    // is shorter than the system clock tick.
    timclk = stm32f411_tim2_clock_hz();
    g_tickless_counts_per_tick = stm32f411_tim2_counts_per_tick(timclk);
    psc_div = stm32f411_tim2_prescaler_div(timclk,
                                           g_tickless_counts_per_tick);
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
    /* Peripheral was just reset and only update interrupts are enabled. */
    TIM2->SR = 0U;

    NVIC_SetPriority(SysTick_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
    NVIC_SetPriority(TIM2_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
    NVIC_ClearPendingIRQ(TIM2_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
#endif
}

tiku_clock_arch_time_t tiku_clock_arch_time(void)
{
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    return (tiku_clock_arch_time_t)stm32f411_tim2_ticks64();
#else
    return (tiku_clock_arch_time_t)g_tick_count;
#endif
}

unsigned long tiku_clock_arch_seconds(void)
{
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    return g_tim2_seconds_offset
         + (unsigned long)(stm32f411_tim2_ticks64()
                         / (uint32_t)TIKU_CLOCK_ARCH_SECOND);
#else
    return g_seconds;
#endif
}

void tiku_clock_arch_set_seconds(unsigned long sec)
{
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    unsigned long now_sec =
        (unsigned long)(stm32f411_tim2_ticks64()
                      / (uint32_t)TIKU_CLOCK_ARCH_SECOND);
    g_tim2_seconds_offset = sec - now_sec;
#else
    g_seconds = sec;
#endif
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
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    return 0U;
#else
    uint32_t cvr = SysTick->VAL;
    uint32_t rvr = SysTick->LOAD;
    uint32_t fine;

    if (rvr == 0U) {
        return 0U;
    }

    fine = ((rvr - cvr) * 0xFFFFU) / rvr;
    return (unsigned short)fine;
#endif
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
    uint8_t actions;

    tiku_atomic_enter();
    if (!armed) {
        g_tim2_timer_armed = 0U;
        g_tim2_timer_poll_pending = 0U;
        g_tim2_timer_deadline16 = 0U;
    } else {
        g_tim2_timer_armed = 1U;
        g_tim2_timer_poll_pending = 0U;
        g_tim2_timer_deadline16 = next;
    }

    actions = stm32f411_tim2_rearm_cc1_mux_locked();
    tiku_atomic_exit();
    stm32f411_tim2_service_actions(actions);
}

void tiku_hang_arch_rearm(tiku_clock_time_t deadline, uint8_t armed)
{
    uint8_t actions;

    tiku_atomic_enter();
    if (!armed) {
        g_tim2_hang_armed = 0U;
        g_tim2_hang_deadline16 = 0U;
    } else {
        g_tim2_hang_armed = 1U;
        g_tim2_hang_deadline16 = deadline;
    }

    actions = stm32f411_tim2_rearm_cc1_mux_locked();
    tiku_atomic_exit();
    stm32f411_tim2_service_actions(actions);
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
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    /* SysTick is not the scheduler heartbeat in TIM2 deadline mode. */
#else
    stm32f411_tick_advance_n(1U);
#endif
}

void tiku_stm32f411_tim2_irq_handler(void)
{
#if TIKU_STM32_TIM2_DEADLINE_EXPERIMENT
    uint32_t sr = TIM2->SR;
    uint8_t source;
    uint8_t actions = 0U;

    if (sr & TIM_SR_CC1IF) {
        source = g_tim2_cc1_source;
        stm32f411_tim2_disarm_cc1();

        if (source == TIKU_STM32_CC_SRC_TIMER) {
            g_tim2_timer_poll_pending = 1U;
            actions |= TIKU_STM32_CC_SRC_TIMER;
        } else if (source == TIKU_STM32_CC_SRC_HANG) {
            g_tim2_hang_armed = 0U;
            actions |= TIKU_STM32_CC_SRC_HANG;
        }

        actions |= stm32f411_tim2_rearm_cc1_mux_locked();
        stm32f411_tim2_service_actions(actions);
    }
#else
    stm32f411_tim2_clear_uif();
    stm32f411_tickless_reconcile();
#endif
}
