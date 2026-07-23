/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_rtc_arch.h - STM32F411RE RTC hardware API
 *
 * STM32-local RTC calendar, Alarm A/B, and Wakeup Timer declarations.
 * This is intentionally separate from kernel/cpu/tiku_rtc.[ch], which is the
 * portable soft Unix-seconds wall-clock layer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32F411_RTC_ARCH_H_
#define TIKU_STM32F411_RTC_ARCH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/
/* RETURN CODES                                                              */
/*---------------------------------------------------------------------------*/

#define TIKU_STM32F411_RTC_OK                   0
#define TIKU_STM32F411_RTC_ERR_INVALID         -1
#define TIKU_STM32F411_RTC_ERR_TIMEOUT         -2
#define TIKU_STM32F411_RTC_ERR_SOURCE_MISMATCH -3
#define TIKU_STM32F411_RTC_ERR_RANGE           -4
#define TIKU_STM32F411_RTC_ERR_NOT_INIT        -5
#define TIKU_STM32F411_RTC_ERR_BUSY            -6
#define TIKU_STM32F411_RTC_ERR_NONE            -7
#define TIKU_STM32F411_RTC_ERR_NOT_IMPL        -8

/*---------------------------------------------------------------------------*/
/* TYPES                                                                     */
/*---------------------------------------------------------------------------*/

typedef struct {
    uint16_t year;     /* 2000..2099 */
    uint8_t  month;    /* 1..12 */
    uint8_t  day;      /* 1..31 */
    uint8_t  weekday;  /* 1..7, STM32-compatible numbering */
    uint8_t  hour;     /* 0..23 */
    uint8_t  minute;   /* 0..59 */
    uint8_t  second;   /* 0..59 */
} tiku_stm32f411_rtc_calendar_t;

typedef enum {
    TIKU_STM32F411_RTC_CLOCK_LSE = 0,
    TIKU_STM32F411_RTC_CLOCK_LSI = 1,
    TIKU_STM32F411_RTC_CLOCK_HSE_DIV32 = 2,
} tiku_stm32f411_rtc_clock_source_t;

typedef enum {
    TIKU_STM32F411_RTC_ALARM_A = 0,
    TIKU_STM32F411_RTC_ALARM_B = 1,
} tiku_stm32f411_rtc_alarm_id_t;

typedef enum {
    TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV16 = 0x0,
    TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV8  = 0x1,
    TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV4  = 0x2,
    TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV2  = 0x3,
    TIKU_STM32F411_RTC_WAKEUP_CK_SPRE_16   = 0x4,
    TIKU_STM32F411_RTC_WAKEUP_CK_SPRE_17   = 0x6,
} tiku_stm32f411_rtc_wakeup_clock_t;

typedef enum {
    TIKU_STM32F411_RTC_IRQ_ALARM_A = 0,
    TIKU_STM32F411_RTC_IRQ_ALARM_B = 1,
    TIKU_STM32F411_RTC_IRQ_WAKEUP  = 2,
} tiku_stm32f411_rtc_interrupt_source_t;

typedef uint64_t tiku_stm32f411_rtc_ticks_t;
typedef uint32_t tiku_stm32f411_rtc_alarm_mask_t;

/* Alarm-mask bits name fields that should participate in an Alarm A/B match. */
#define TIKU_STM32F411_RTC_ALARM_MATCH_SECOND  (1UL << 0)
#define TIKU_STM32F411_RTC_ALARM_MATCH_MINUTE  (1UL << 1)
#define TIKU_STM32F411_RTC_ALARM_MATCH_HOUR    (1UL << 2)
#define TIKU_STM32F411_RTC_ALARM_MATCH_DAY     (1UL << 3)
#define TIKU_STM32F411_RTC_ALARM_MATCH_WEEKDAY (1UL << 4)

/*---------------------------------------------------------------------------*/
/* FUNCTION PROTOTYPES                                                       */
/*---------------------------------------------------------------------------*/

int tiku_stm32f411_rtc_init(
    const tiku_stm32f411_rtc_calendar_t *calendar,
    tiku_stm32f411_rtc_clock_source_t clock_source);

int tiku_stm32f411_rtc_set_calendar(
    const tiku_stm32f411_rtc_calendar_t *calendar);

int tiku_stm32f411_rtc_read_calendar(
    tiku_stm32f411_rtc_calendar_t *calendar);

int tiku_stm32f411_rtc_alarm_set(
    tiku_stm32f411_rtc_alarm_id_t alarm_id,
    const tiku_stm32f411_rtc_calendar_t *target,
    tiku_stm32f411_rtc_alarm_mask_t match_mask);

int tiku_stm32f411_rtc_alarm_cancel(
    tiku_stm32f411_rtc_alarm_id_t alarm_id);

int tiku_stm32f411_rtc_alarm_is_armed(
    tiku_stm32f411_rtc_alarm_id_t alarm_id,
    uint8_t *armed);

int tiku_stm32f411_rtc_wakeup_set(
    uint32_t duration_ticks,
    tiku_stm32f411_rtc_wakeup_clock_t clock_divider);

int tiku_stm32f411_rtc_wakeup_cancel(void);

int tiku_stm32f411_rtc_wakeup_is_armed(uint8_t *armed);

int tiku_stm32f411_rtc_clear_interrupt_flag(
    tiku_stm32f411_rtc_interrupt_source_t source);

int tiku_stm32f411_rtc_backup_domain_reset(void);

int tiku_stm32f411_rtc_calendar_to_ticks(
    const tiku_stm32f411_rtc_calendar_t *calendar,
    tiku_stm32f411_rtc_ticks_t *ticks);

int tiku_stm32f411_rtc_ticks_to_calendar(
    tiku_stm32f411_rtc_ticks_t ticks,
    tiku_stm32f411_rtc_calendar_t *calendar);

#ifdef __cplusplus
}
#endif

#endif /* TIKU_STM32F411_RTC_ARCH_H_ */
