/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_timer_arch.c - STM32F411RE timer backend
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_timer_arch.h"
#include "tiku_cpu_freq_boot_arch.h"
#include "tiku_rtc_arch.h"
#include <hal/tiku_cpu.h>
#include <kernel/cpu/tiku_hang.h>
#include <kernel/timers/tiku_clock.h>
#include <kernel/timers/tiku_timer.h>
#include <stdint.h>
#include <stm32f411xe.h>

/*---------------------------------------------------------------------------*/
/* Configuration                                                             */
/*---------------------------------------------------------------------------*/

#define TIKU_STM32_TICKLESS_NVIC_PRIO  1U

#define TIKU_STM32_CC_SRC_NONE   0U
#define TIKU_STM32_CC_SRC_TIMER  1U
#define TIKU_STM32_CC_SRC_HANG   2U
#define TIKU_STM32_DEADLINE16_SAFE_TICKS 0x8000U
#define TIKU_STM32_DEADLINE16_SAFE_SECONDS \
    (TIKU_STM32_DEADLINE16_SAFE_TICKS / TIKU_CLOCK_ARCH_SECOND)

#ifndef TIKU_STM32F411_RTC_WUT_THRESHOLD_TICKS
#define TIKU_STM32F411_RTC_WUT_THRESHOLD_TICKS \
    ((tiku_clock_time_t)(TIKU_CLOCK_ARCH_SECOND * 30U))
#endif

#ifndef TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS
#define TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS \
    ((tiku_clock_time_t)TIKU_CLOCK_ARCH_SECOND)
#endif

#ifndef TIKU_STM32F411_RTC_ALARM_THRESHOLD_TICKS
#define TIKU_STM32F411_RTC_ALARM_THRESHOLD_TICKS \
    ((tiku_clock_time_t)TIKU_CLOCK_SAFE_HALF_RANGE)
#endif

#ifndef TIKU_STM32F411_RTC_TIMER_ALARM_ID
#define TIKU_STM32F411_RTC_TIMER_ALARM_ID TIKU_STM32F411_RTC_ALARM_A
#endif

#ifndef TIKU_STM32F411_STOP_IDLE_ENABLE
#define TIKU_STM32F411_STOP_IDLE_ENABLE 0
#endif

#ifndef TIKU_STM32F411_STOP_BLOCK_WHEN_HANG_ARMED
#define TIKU_STM32F411_STOP_BLOCK_WHEN_HANG_ARMED 1
#endif

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

typedef enum {
    TIKU_STM32_TIMER_BACKEND_NONE = 0,
    TIKU_STM32_TIMER_BACKEND_TIM2 = 1,
    TIKU_STM32_TIMER_BACKEND_RTC_WUT = 2,
    TIKU_STM32_TIMER_BACKEND_RTC_ALARM = 3,
} tiku_stm32_timer_backend_t;

/*---------------------------------------------------------------------------*/
/* State                                                                     */
/*---------------------------------------------------------------------------*/

static volatile uint64_t       g_tick_count = 0ULL;
static volatile uint32_t       g_tick_remainder = 0U;
static volatile uint32_t       g_tim2_counts_per_tick = 0U;
static volatile uint8_t        g_tim2_cc1_armed = 0U;
static volatile uint8_t        g_tim2_cc1_source = TIKU_STM32_CC_SRC_NONE;
static volatile uint8_t        g_tim2_timer_armed = 0U;
static volatile uint8_t        g_tim2_timer_poll_pending = 0U;
static volatile tiku_clock_time_t g_tim2_timer_deadline16 = 0U;
static volatile tiku_stm32_timer_backend_t g_timer_backend =
    TIKU_STM32_TIMER_BACKEND_NONE;
static tiku_stm32f411_rtc_timestamp_t g_rtc_wut_epoch;
static volatile uint8_t        g_rtc_wut_armed = 0U;
static volatile uint8_t        g_rtc_wut_wake_pending = 0U;
static volatile tiku_clock_time_t g_rtc_wut_deadline16 = 0U;
static tiku_stm32f411_rtc_timestamp_t g_rtc_alarm_epoch;
static volatile uint8_t        g_rtc_alarm_armed = 0U;
static volatile uint8_t        g_rtc_alarm_wake_pending = 0U;
static volatile tiku_clock_time_t g_rtc_alarm_deadline16 = 0U;
static volatile uint8_t        g_tim2_hang_armed = 0U;
static volatile tiku_clock_time_t g_tim2_hang_deadline16 = 0U;
static volatile uint16_t       g_tim2_cc1_missed_writes = 0U;
static volatile uint32_t       g_tim2_last_count32 = 0U;
static volatile unsigned long  g_tim2_seconds_offset = 0UL;

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

static void stm32f411_tim2_freerun_init(void)
{
    unsigned long timclk = stm32f411_tim2_clock_hz();
    uint32_t psc_div;

    g_tim2_counts_per_tick = stm32f411_tim2_counts_per_tick(timclk);
    psc_div = stm32f411_tim2_prescaler_div(timclk,
                                           g_tim2_counts_per_tick);

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

static inline uint32_t stm32f411_tim2_now32(void)
{
    return TIM2->CNT;
}

static uint64_t stm32f411_tick_advance_counts_locked(uint32_t elapsed_counts)
{
    uint32_t counts_per_tick = g_tim2_counts_per_tick;
    uint64_t total_counts;

    if (counts_per_tick == 0U) {
        counts_per_tick = 1U;
    }

    total_counts = (uint64_t)g_tick_remainder + elapsed_counts;
    g_tick_count += total_counts / counts_per_tick;
    g_tick_remainder = (uint32_t)(total_counts % counts_per_tick);

    return g_tick_count;
}

static uint64_t stm32f411_tick_advance_bulk_locked(uint64_t elapsed_ticks)
{
    g_tick_count += elapsed_ticks;
    return g_tick_count;
}

static uint64_t stm32f411_tick_resync_bulk_locked(uint64_t elapsed_ticks)
{
    uint64_t ticks = stm32f411_tick_advance_bulk_locked(elapsed_ticks);

    /*
     * RTC-mediated resync accounts for the elapsed sleep window. Move the
     * TIM2 baseline to "now" so the next TIM2 sync does not count the same
     * wall time again.
     */
    g_tick_remainder = 0U;
    g_tim2_last_count32 = stm32f411_tim2_now32();
    return ticks;
}

static uint64_t stm32f411_tim2_sync_ticks_locked(void)
{
    uint32_t now32 = stm32f411_tim2_now32();
    uint32_t elapsed_counts = now32 - g_tim2_last_count32;
    uint64_t ticks = stm32f411_tick_advance_counts_locked(elapsed_counts);

    g_tim2_last_count32 = now32;
    return ticks;
}

static uint64_t stm32f411_tim2_ticks64(void)
{
    uint64_t ticks;

    tiku_atomic_enter();
    ticks = stm32f411_tim2_sync_ticks_locked();
    tiku_atomic_exit();

    return ticks;
}

static inline int
stm32f411_tim2_reached(uint32_t now, uint32_t target)
{
    return ((int32_t)(now - target)) >= 0;
}

static inline int
stm32f411_clock16_before(tiku_clock_time_t a, tiku_clock_time_t b)
{
    return TIKU_CLOCK_LT(a, b);
}

static tiku_clock_time_t
stm32f411_timer_delay_to_deadline_locked(tiku_clock_time_t deadline16)
{
    tiku_clock_time_t now16 =
        (tiku_clock_time_t)stm32f411_tim2_sync_ticks_locked();

    if (!stm32f411_clock16_before(now16, deadline16)) {
        return 0U;
    }

    return (tiku_clock_time_t)(deadline16 - now16);
}

static uint64_t
stm32f411_rtc_elapsed_timer_ticks(
    const tiku_stm32f411_rtc_timestamp_t *start,
    const tiku_stm32f411_rtc_timestamp_t *end)
{
    uint32_t scale;
    uint64_t start_subticks;
    uint64_t end_subticks;
    uint64_t elapsed_subticks;

    if ((start == 0) || (end == 0) ||
        (start->subsecond_scale == 0U) ||
        (start->subsecond_scale != end->subsecond_scale)) {
        return 0ULL;
    }

    scale = start->subsecond_scale;
    start_subticks = (start->ticks * (uint64_t)scale) + start->subsecond;
    end_subticks = (end->ticks * (uint64_t)scale) + end->subsecond;
    if (end_subticks < start_subticks) {
        return 0ULL;
    }

    elapsed_subticks = end_subticks - start_subticks;
    return (elapsed_subticks * (uint64_t)TIKU_CLOCK_ARCH_SECOND) /
           (uint64_t)scale;
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
    uint32_t counts_per_tick = g_tim2_counts_per_tick;
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
static int stm32f411_tim2_arm_cc1(uint32_t target)
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

static tiku_stm32_timer_backend_t
stm32f411_timer_backend_classify_locked(tiku_clock_time_t next)
{
    tiku_clock_time_t delay = stm32f411_timer_delay_to_deadline_locked(next);

    if (delay >= TIKU_STM32F411_RTC_ALARM_THRESHOLD_TICKS &&
        delay < TIKU_CLOCK_SAFE_HALF_RANGE &&
        delay > TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS) {
        return TIKU_STM32_TIMER_BACKEND_RTC_ALARM;
    }

    if (delay >= TIKU_STM32F411_RTC_WUT_THRESHOLD_TICKS &&
        delay < TIKU_CLOCK_SAFE_HALF_RANGE &&
        delay > TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS) {
        return TIKU_STM32_TIMER_BACKEND_RTC_WUT;
    }

    return TIKU_STM32_TIMER_BACKEND_TIM2;
}

static uint8_t
stm32f411_timer_rtc_idle_allowed_locked(void)
{
#if TIKU_STM32F411_STOP_IDLE_ENABLE && \
    TIKU_STM32F411_STOP_BLOCK_WHEN_HANG_ARMED
    if (g_tim2_hang_armed) {
        return 0U;
    }
#endif

    return 1U;
}

static void
stm32f411_timer_backend_disarm_rtc_wut_locked(void)
{
    if (g_rtc_wut_armed) {
        (void)tiku_stm32f411_rtc_wakeup_cancel();
    }

    g_rtc_wut_armed = 0U;
    g_rtc_wut_wake_pending = 0U;
    g_rtc_wut_deadline16 = 0U;
}

static int
stm32f411_timer_backend_arm_rtc_wut_locked(tiku_clock_time_t next)
{
    tiku_clock_time_t delay = stm32f411_timer_delay_to_deadline_locked(next);
    tiku_clock_time_t wake_delta;
    uint32_t wut_seconds;
    int ret;

    if (delay <= TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS) {
        return 0;
    }

    wake_delta = (tiku_clock_time_t)
        (delay - TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS);
    wut_seconds = (uint32_t)(wake_delta / TIKU_CLOCK_ARCH_SECOND);
    if (wut_seconds == 0U) {
        return 0;
    }

    ret = tiku_stm32f411_rtc_read_timestamp(&g_rtc_wut_epoch, 0U);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return 0;
    }

    ret = tiku_stm32f411_rtc_wakeup_set(
        wut_seconds, TIKU_STM32F411_RTC_WAKEUP_CK_SPRE_16);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return 0;
    }

    g_rtc_wut_deadline16 = next;
    g_rtc_wut_wake_pending = 0U;
    g_rtc_wut_armed = 1U;
    g_timer_backend = TIKU_STM32_TIMER_BACKEND_RTC_WUT;
    return 1;
}

static void
stm32f411_timer_backend_disarm_rtc_alarm_locked(void)
{
    if (g_rtc_alarm_armed) {
        (void)tiku_stm32f411_rtc_alarm_cancel(
            TIKU_STM32F411_RTC_TIMER_ALARM_ID);
    }

    g_rtc_alarm_armed = 0U;
    g_rtc_alarm_wake_pending = 0U;
    g_rtc_alarm_deadline16 = 0U;
}

static int
stm32f411_timer_backend_arm_rtc_alarm_locked(tiku_clock_time_t next)
{
    tiku_stm32f411_rtc_calendar_t target;
    tiku_clock_time_t delay = stm32f411_timer_delay_to_deadline_locked(next);
    tiku_clock_time_t wake_delta;
    tiku_stm32f411_rtc_ticks_t target_ticks;
    tiku_stm32f411_rtc_alarm_mask_t mask =
        TIKU_STM32F411_RTC_ALARM_MATCH_SECOND |
        TIKU_STM32F411_RTC_ALARM_MATCH_MINUTE |
        TIKU_STM32F411_RTC_ALARM_MATCH_HOUR |
        TIKU_STM32F411_RTC_ALARM_MATCH_DAY;
    uint32_t wake_seconds;
    int ret;

    if (delay <= TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS) {
        return 0;
    }

    wake_delta = (tiku_clock_time_t)
        (delay - TIKU_STM32F411_RTC_WAKE_MARGIN_TICKS);
    wake_seconds = (uint32_t)(wake_delta / TIKU_CLOCK_ARCH_SECOND);
    if (wake_seconds == 0U) {
        return 0;
    }

    ret = tiku_stm32f411_rtc_read_timestamp(&g_rtc_alarm_epoch, 0U);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return 0;
    }

    target_ticks = g_rtc_alarm_epoch.ticks +
        (tiku_stm32f411_rtc_ticks_t)wake_seconds;
    ret = tiku_stm32f411_rtc_ticks_to_calendar(target_ticks, &target);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return 0;
    }
    if (target.year != g_rtc_alarm_epoch.calendar.year ||
        target.month != g_rtc_alarm_epoch.calendar.month) {
        return 0;
    }

    ret = tiku_stm32f411_rtc_alarm_set(
        TIKU_STM32F411_RTC_TIMER_ALARM_ID, &target, mask);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return 0;
    }

    g_rtc_alarm_deadline16 = next;
    g_rtc_alarm_wake_pending = 0U;
    g_rtc_alarm_armed = 1U;
    g_timer_backend = TIKU_STM32_TIMER_BACKEND_RTC_ALARM;
    return 1;
}

static void
stm32f411_timer_backend_disarm_locked(void)
{
    switch (g_timer_backend) {
    case TIKU_STM32_TIMER_BACKEND_TIM2:
        g_tim2_timer_armed = 0U;
        g_tim2_timer_poll_pending = 0U;
        g_tim2_timer_deadline16 = 0U;
        break;
    case TIKU_STM32_TIMER_BACKEND_RTC_WUT:
        stm32f411_timer_backend_disarm_rtc_wut_locked();
        break;
    case TIKU_STM32_TIMER_BACKEND_RTC_ALARM:
        stm32f411_timer_backend_disarm_rtc_alarm_locked();
        break;
    case TIKU_STM32_TIMER_BACKEND_NONE:
    default:
        break;
    }

    g_timer_backend = TIKU_STM32_TIMER_BACKEND_NONE;
}

static int
stm32f411_timer_backend_arm_locked(tiku_stm32_timer_backend_t backend,
                                   tiku_clock_time_t next)
{
    switch (backend) {
    case TIKU_STM32_TIMER_BACKEND_TIM2:
        g_tim2_timer_armed = 1U;
        g_tim2_timer_poll_pending = 0U;
        g_tim2_timer_deadline16 = next;
        g_timer_backend = TIKU_STM32_TIMER_BACKEND_TIM2;
        return 1;
    case TIKU_STM32_TIMER_BACKEND_RTC_WUT:
        if (stm32f411_timer_backend_arm_rtc_wut_locked(next)) {
            return 1;
        }
        g_timer_backend = TIKU_STM32_TIMER_BACKEND_NONE;
        return 0;
    case TIKU_STM32_TIMER_BACKEND_RTC_ALARM:
        if (stm32f411_timer_backend_arm_rtc_alarm_locked(next)) {
            return 1;
        }
        g_timer_backend = TIKU_STM32_TIMER_BACKEND_NONE;
        return 0;
    case TIKU_STM32_TIMER_BACKEND_NONE:
    default:
        g_timer_backend = TIKU_STM32_TIMER_BACKEND_NONE;
        return 0;
    }
}

static uint32_t
stm32f411_timer_rtc_alarm_flag(void)
{
    switch ((tiku_stm32f411_rtc_alarm_id_t)TIKU_STM32F411_RTC_TIMER_ALARM_ID) {
    case TIKU_STM32F411_RTC_ALARM_A:
        return RTC_ISR_ALRAF;
    case TIKU_STM32F411_RTC_ALARM_B:
        return RTC_ISR_ALRBF;
    default:
        return 0U;
    }
}

static uint8_t
stm32f411_timer_leave_rtc_backend_locked(void)
{
    tiku_stm32f411_rtc_timestamp_t now;
    const tiku_stm32f411_rtc_timestamp_t *epoch;
    tiku_clock_time_t deadline16;
    uint64_t elapsed_ticks;
    uint8_t wake_pending;
    int ret;

    switch (g_timer_backend) {
    case TIKU_STM32_TIMER_BACKEND_RTC_WUT:
        if (!g_rtc_wut_armed) {
            return 0U;
        }
        if ((RTC->ISR & RTC_ISR_WUTF) != 0U) {
            g_rtc_wut_wake_pending = 1U;
        }
        epoch = &g_rtc_wut_epoch;
        deadline16 = g_rtc_wut_deadline16;
        wake_pending = g_rtc_wut_wake_pending;
        break;
    case TIKU_STM32_TIMER_BACKEND_RTC_ALARM:
        if (!g_rtc_alarm_armed) {
            return 0U;
        }
        if ((RTC->ISR & stm32f411_timer_rtc_alarm_flag()) != 0U) {
            g_rtc_alarm_wake_pending = 1U;
        }
        epoch = &g_rtc_alarm_epoch;
        deadline16 = g_rtc_alarm_deadline16;
        wake_pending = g_rtc_alarm_wake_pending;
        break;
    case TIKU_STM32_TIMER_BACKEND_NONE:
    case TIKU_STM32_TIMER_BACKEND_TIM2:
    default:
        return 0U;
    }

    if (wake_pending) {
        /*
         * If a future STM32 idle hook enters STOP, the core resumes on HSI
         * with PLL disabled. Restore the configured clock tree before the
         * timestamp/resync work; WFI-only idle makes this a cheap no-op.
         */
        (void)tiku_cpu_boot_stm32f411_post_stop_wake();
        ret = tiku_stm32f411_rtc_read_timestamp(&now, 1U);
        if (ret == TIKU_STM32F411_RTC_OK) {
            elapsed_ticks =
                stm32f411_rtc_elapsed_timer_ticks(epoch, &now);
            (void)stm32f411_tick_resync_bulk_locked(elapsed_ticks);
        } else {
            /*
             * Phase 4 keeps idle as WFI, so TIM2 has continued to run. If
             * the RTC timestamp path fails, fall back to TIM2 sync rather
             * than wedging the software timer backend.
             */
            (void)stm32f411_tim2_sync_ticks_locked();
        }
    } else {
        /*
         * A non-RTC interrupt ended the idle window early. TIM2 kept running
         * during WFI, so its free-running counter is the best local clock for
         * the partial sleep interval.
         */
        (void)stm32f411_tim2_sync_ticks_locked();
    }

    if (g_timer_backend == TIKU_STM32_TIMER_BACKEND_RTC_WUT) {
        stm32f411_timer_backend_disarm_rtc_wut_locked();
    } else {
        stm32f411_timer_backend_disarm_rtc_alarm_locked();
    }
    g_timer_backend = TIKU_STM32_TIMER_BACKEND_NONE;

    (void)stm32f411_timer_backend_arm_locked(
        TIKU_STM32_TIMER_BACKEND_TIM2, deadline16);
    return stm32f411_tim2_rearm_cc1_mux_locked();
}

static uint8_t
stm32f411_timer_enter_rtc_backend_locked(uint8_t *stretched)
{
    tiku_stm32_timer_backend_t backend;
    tiku_clock_time_t deadline16;
    uint8_t actions;

    *stretched = 0U;

    if (g_timer_backend != TIKU_STM32_TIMER_BACKEND_TIM2 ||
        !g_tim2_timer_armed ||
        g_tim2_timer_poll_pending) {
        return 0U;
    }

    if (!stm32f411_timer_rtc_idle_allowed_locked()) {
        return 0U;
    }

    deadline16 = g_tim2_timer_deadline16;
    backend = stm32f411_timer_backend_classify_locked(deadline16);
    if (backend == TIKU_STM32_TIMER_BACKEND_TIM2 ||
        backend == TIKU_STM32_TIMER_BACKEND_NONE) {
        return 0U;
    }

    g_tim2_timer_armed = 0U;
    g_tim2_timer_poll_pending = 0U;
    g_tim2_timer_deadline16 = 0U;
    g_timer_backend = TIKU_STM32_TIMER_BACKEND_NONE;

    if (!stm32f411_timer_backend_arm_locked(backend, deadline16)) {
        (void)stm32f411_timer_backend_arm_locked(
            TIKU_STM32_TIMER_BACKEND_TIM2, deadline16);
        return stm32f411_tim2_rearm_cc1_mux_locked();
    }

    *stretched = 1U;
    actions = stm32f411_tim2_rearm_cc1_mux_locked();
    return actions;
}

/*---------------------------------------------------------------------------*/
/* HAL                                                                       */
/*---------------------------------------------------------------------------*/

void tiku_clock_arch_init(void)
{
    g_tick_count = 0ULL;
    g_tick_remainder = 0U;
    g_tim2_counts_per_tick = 0U;
    g_tim2_cc1_armed = 0U;
    g_tim2_cc1_source = TIKU_STM32_CC_SRC_NONE;
    g_tim2_timer_armed = 0U;
    g_tim2_timer_poll_pending = 0U;
    g_tim2_timer_deadline16 = 0U;
    g_timer_backend = TIKU_STM32_TIMER_BACKEND_NONE;
    g_rtc_wut_armed = 0U;
    g_rtc_wut_wake_pending = 0U;
    g_rtc_wut_deadline16 = 0U;
    g_rtc_alarm_armed = 0U;
    g_rtc_alarm_wake_pending = 0U;
    g_rtc_alarm_deadline16 = 0U;
    g_tim2_hang_armed = 0U;
    g_tim2_hang_deadline16 = 0U;
    g_tim2_cc1_missed_writes = 0U;
    g_tim2_last_count32 = 0U;
    g_tim2_seconds_offset = 0UL;

    SysTick->CTRL = 0U;
    stm32f411_tim2_freerun_init();

    NVIC_SetPriority(SysTick_IRQn, TIKU_STM32_TICKLESS_NVIC_PRIO);
}

tiku_clock_arch_time_t tiku_clock_arch_time(void)
{
    return (tiku_clock_arch_time_t)stm32f411_tim2_ticks64();
}

unsigned long tiku_clock_arch_seconds(void)
{
    return g_tim2_seconds_offset
         + (unsigned long)(stm32f411_tim2_ticks64()
                         / (uint32_t)TIKU_CLOCK_ARCH_SECOND);
}

void tiku_clock_arch_set_seconds(unsigned long sec)
{
    unsigned long now_sec =
        (unsigned long)(stm32f411_tim2_ticks64()
                      / (uint32_t)TIKU_CLOCK_ARCH_SECOND);
    g_tim2_seconds_offset = sec - now_sec;
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
    return 0U;
}

int tiku_clock_arch_fine_max(void)
{
    return 0xFFFF;
}

unsigned char tiku_clock_arch_fault(void)
{
    return 0U;
}

void tiku_timer_arch_rearm(tiku_clock_time_t next, uint8_t armed)
{
    uint8_t actions;

    tiku_atomic_enter();
    stm32f411_timer_backend_disarm_locked();
    if (armed) {
        (void)stm32f411_timer_backend_arm_locked(
            TIKU_STM32_TIMER_BACKEND_TIM2, next);
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

/*---------------------------------------------------------------------------*/
/* Tickless hooks                                                            */
/*---------------------------------------------------------------------------*/

int tiku_clock_tickless_begin(tiku_clock_time_t ticks_ahead)
{
    (void)ticks_ahead;
    uint8_t actions;
    uint8_t stretched;

    tiku_atomic_enter();
    actions = stm32f411_timer_leave_rtc_backend_locked();
    if (actions == 0U) {
        actions = stm32f411_timer_enter_rtc_backend_locked(&stretched);
    } else {
        stretched = 0U;
    }
    tiku_atomic_exit();

    stm32f411_tim2_service_actions(actions);
    if (actions != 0U && !stretched) {
        NVIC_SetPendingIRQ(TIM2_IRQn);
    }
    return stretched;
}

void tiku_clock_tickless_end(void)
{
    uint8_t actions;

    tiku_atomic_enter();
    actions = stm32f411_timer_leave_rtc_backend_locked();
    tiku_atomic_exit();

    stm32f411_tim2_service_actions(actions);
}

int tiku_clock_tickless_available(void)
{
    return 1;
}

/*---------------------------------------------------------------------------*/
/* ISRs                                                                      */
/*---------------------------------------------------------------------------*/

void tiku_stm32f411_systick_handler(void)
{
    /* SysTick is not the scheduler heartbeat in TIM2 deadline mode. */
}

void tiku_stm32f411_timer_rtc_wakeup_irq(void)
{
    if (g_timer_backend == TIKU_STM32_TIMER_BACKEND_RTC_WUT &&
        g_rtc_wut_armed) {
        g_rtc_wut_wake_pending = 1U;
    }
}

void tiku_stm32f411_timer_rtc_alarm_irq(uint32_t flags)
{
    if (g_timer_backend == TIKU_STM32_TIMER_BACKEND_RTC_ALARM &&
        g_rtc_alarm_armed &&
        ((flags & stm32f411_timer_rtc_alarm_flag()) != 0U)) {
        g_rtc_alarm_wake_pending = 1U;
    }
}

void tiku_stm32f411_tim2_irq_handler(void)
{
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
}
