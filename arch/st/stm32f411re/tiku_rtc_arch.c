/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_rtc_arch.c - STM32F411RE RTC hardware API skeleton
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_rtc_arch.h"

#include "hal/tiku_cpu.h"
#include "stm32f411xe.h"

#define TIKU_STM32F411_RTC_EPOCH_YEAR  2000U
#define TIKU_STM32F411_RTC_MAX_YEAR    2099U
#define TIKU_STM32F411_RTC_SECONDS_DAY 86400ULL
#define TIKU_STM32F411_RTC_SPIN_TIMEOUT 1000000U
#define TIKU_STM32F411_RTC_LSE_HZ       32768UL
#define TIKU_STM32F411_RTC_LSI_HZ       32000UL
#define TIKU_STM32F411_RTC_BKP_MAGIC    0x54435231UL
#define TIKU_STM32F411_RTC_NVIC_PRIO    1U

#ifndef HSE_VALUE
#define HSE_VALUE ((uint32_t)8000000UL)
#endif

#define TIKU_STM32F411_RTC_ALARM_MATCH_ALL \
    (TIKU_STM32F411_RTC_ALARM_MATCH_SECOND | \
     TIKU_STM32F411_RTC_ALARM_MATCH_MINUTE | \
     TIKU_STM32F411_RTC_ALARM_MATCH_HOUR | \
     TIKU_STM32F411_RTC_ALARM_MATCH_DAY | \
     TIKU_STM32F411_RTC_ALARM_MATCH_WEEKDAY)

#if defined(__GNUC__)
#define TIKU_STM32F411_RTC_UNUSED __attribute__((unused))
#else
#define TIKU_STM32F411_RTC_UNUSED
#endif

typedef struct {
    uint8_t restore_dbp;
} tiku_stm32f411_rtc_backup_access_t;

typedef struct {
    uint32_t prediv_a;
    uint32_t prediv_s;
} tiku_stm32f411_rtc_prescaler_t;

typedef struct {
    uint32_t msk1;
    uint32_t msk2;
    uint32_t msk3;
    uint32_t msk4;
    uint32_t wdsel;
    uint8_t st_pos;
    uint8_t su_pos;
    uint8_t mnt_pos;
    uint8_t mnu_pos;
    uint8_t ht_pos;
    uint8_t hu_pos;
    uint8_t dt_pos;
    uint8_t du_pos;
} tiku_stm32f411_rtc_alarm_reg_desc_t;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t day;
    uint8_t weekday;
    uint8_t use_weekday;
    tiku_stm32f411_rtc_alarm_mask_t match_mask;
} tiku_stm32f411_rtc_alarm_fields_t;

static const tiku_stm32f411_rtc_alarm_reg_desc_t
tiku_stm32f411_rtc_alrmar_desc = {
    RTC_ALRMAR_MSK1,
    RTC_ALRMAR_MSK2,
    RTC_ALRMAR_MSK3,
    RTC_ALRMAR_MSK4,
    RTC_ALRMAR_WDSEL,
    RTC_ALRMAR_ST_Pos,
    RTC_ALRMAR_SU_Pos,
    RTC_ALRMAR_MNT_Pos,
    RTC_ALRMAR_MNU_Pos,
    RTC_ALRMAR_HT_Pos,
    RTC_ALRMAR_HU_Pos,
    RTC_ALRMAR_DT_Pos,
    RTC_ALRMAR_DU_Pos,
};

static const tiku_stm32f411_rtc_alarm_reg_desc_t
tiku_stm32f411_rtc_alrmbr_desc = {
    RTC_ALRMBR_MSK1,
    RTC_ALRMBR_MSK2,
    RTC_ALRMBR_MSK3,
    RTC_ALRMBR_MSK4,
    RTC_ALRMBR_WDSEL,
    RTC_ALRMBR_ST_Pos,
    RTC_ALRMBR_SU_Pos,
    RTC_ALRMBR_MNT_Pos,
    RTC_ALRMBR_MNU_Pos,
    RTC_ALRMBR_HT_Pos,
    RTC_ALRMBR_HU_Pos,
    RTC_ALRMBR_DT_Pos,
    RTC_ALRMBR_DU_Pos,
};

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_spin_until_set(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t timeout = TIKU_STM32F411_RTC_SPIN_TIMEOUT;
         timeout > 0U;
         timeout--) {
        if ((*reg & mask) != 0U) {
            return TIKU_STM32F411_RTC_OK;
        }
    }

    return TIKU_STM32F411_RTC_ERR_TIMEOUT;
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_spin_until_clear(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t timeout = TIKU_STM32F411_RTC_SPIN_TIMEOUT;
         timeout > 0U;
         timeout--) {
        if ((*reg & mask) == 0U) {
            return TIKU_STM32F411_RTC_OK;
        }
    }

    return TIKU_STM32F411_RTC_ERR_TIMEOUT;
}

static TIKU_STM32F411_RTC_UNUSED void
tiku_stm32f411_rtc_backup_access_begin(
    tiku_stm32f411_rtc_backup_access_t *access)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;

    if (access != 0) {
        access->restore_dbp = ((PWR->CR & PWR_CR_DBP) == 0U) ? 1U : 0U;
    }

    PWR->CR |= PWR_CR_DBP;
}

static TIKU_STM32F411_RTC_UNUSED void
tiku_stm32f411_rtc_backup_access_end(
    const tiku_stm32f411_rtc_backup_access_t *access)
{
    if ((access != 0) && (access->restore_dbp != 0U)) {
        PWR->CR &= ~PWR_CR_DBP;
    }
}

static TIKU_STM32F411_RTC_UNUSED void
tiku_stm32f411_rtc_write_unlock(void)
{
    RTC->WPR = 0xCAU;
    RTC->WPR = 0x53U;
}

static TIKU_STM32F411_RTC_UNUSED void
tiku_stm32f411_rtc_write_lock(void)
{
    RTC->WPR = 0xFFU;
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_enter_init_mode(void)
{
    RTC->ISR |= RTC_ISR_INIT;
    int ret = tiku_stm32f411_rtc_spin_until_set(&RTC->ISR, RTC_ISR_INITF);

    if (ret != TIKU_STM32F411_RTC_OK) {
        RTC->ISR &= ~RTC_ISR_INIT;
    }

    return ret;
}

static TIKU_STM32F411_RTC_UNUSED void
tiku_stm32f411_rtc_exit_init_mode(void)
{
    RTC->ISR &= ~RTC_ISR_INIT;
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_shadow_sync(void)
{
    if ((RTC->CR & RTC_CR_BYPSHAD) != 0U) {
        return TIKU_STM32F411_RTC_OK;
    }

    RTC->ISR &= ~RTC_ISR_RSF;
    return tiku_stm32f411_rtc_spin_until_set(&RTC->ISR, RTC_ISR_RSF);
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_source_to_rtcsel(
    tiku_stm32f411_rtc_clock_source_t clock_source,
    uint32_t *rtcsel)
{
    if (rtcsel == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    switch (clock_source) {
    case TIKU_STM32F411_RTC_CLOCK_LSE:
        *rtcsel = RCC_BDCR_RTCSEL_0;
        return TIKU_STM32F411_RTC_OK;
    case TIKU_STM32F411_RTC_CLOCK_LSI:
        *rtcsel = RCC_BDCR_RTCSEL_1;
        return TIKU_STM32F411_RTC_OK;
    case TIKU_STM32F411_RTC_CLOCK_HSE_DIV32:
        *rtcsel = RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCSEL_1;
        return TIKU_STM32F411_RTC_OK;
    default:
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_source_hz(
    tiku_stm32f411_rtc_clock_source_t clock_source,
    unsigned long *clock_hz)
{
    if (clock_hz == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    switch (clock_source) {
    case TIKU_STM32F411_RTC_CLOCK_LSE:
        *clock_hz = TIKU_STM32F411_RTC_LSE_HZ;
        return TIKU_STM32F411_RTC_OK;
    case TIKU_STM32F411_RTC_CLOCK_LSI:
        *clock_hz = TIKU_STM32F411_RTC_LSI_HZ;
        return TIKU_STM32F411_RTC_OK;
    case TIKU_STM32F411_RTC_CLOCK_HSE_DIV32:
        *clock_hz = (unsigned long)HSE_VALUE / 32UL;
        return TIKU_STM32F411_RTC_OK;
    default:
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_compute_prescaler(
    unsigned long clock_hz,
    tiku_stm32f411_rtc_prescaler_t *prescaler)
{
    if ((clock_hz == 0UL) || (prescaler == 0)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    for (uint32_t async_div = 128U; async_div > 0U; async_div--) {
        if ((clock_hz % async_div) != 0UL) {
            continue;
        }

        unsigned long sync_div = clock_hz / async_div;
        if ((sync_div == 0UL) || (sync_div > 32768UL)) {
            continue;
        }

        prescaler->prediv_a = async_div - 1U;
        prescaler->prediv_s = (uint32_t)sync_div - 1U;
        return TIKU_STM32F411_RTC_OK;
    }

    return TIKU_STM32F411_RTC_ERR_RANGE;
}

static TIKU_STM32F411_RTC_UNUSED uint32_t
tiku_stm32f411_rtc_pack_prer(
    const tiku_stm32f411_rtc_prescaler_t *prescaler)
{
    return ((prescaler->prediv_a << RTC_PRER_PREDIV_A_Pos) &
            RTC_PRER_PREDIV_A) |
           ((prescaler->prediv_s << RTC_PRER_PREDIV_S_Pos) &
            RTC_PRER_PREDIV_S);
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_enable_clock_source(
    tiku_stm32f411_rtc_clock_source_t clock_source)
{
    switch (clock_source) {
    case TIKU_STM32F411_RTC_CLOCK_LSE:
        RCC->BDCR |= RCC_BDCR_LSEON;
        return tiku_stm32f411_rtc_spin_until_set(&RCC->BDCR,
                                                 RCC_BDCR_LSERDY);
    case TIKU_STM32F411_RTC_CLOCK_LSI:
        RCC->CSR |= RCC_CSR_LSION;
        return tiku_stm32f411_rtc_spin_until_set(&RCC->CSR,
                                                 RCC_CSR_LSIRDY);
    case TIKU_STM32F411_RTC_CLOCK_HSE_DIV32:
        RCC->CR |= RCC_CR_HSEON;
        return tiku_stm32f411_rtc_spin_until_set(&RCC->CR,
                                                 RCC_CR_HSERDY);
    default:
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_select_clock_source(
    tiku_stm32f411_rtc_clock_source_t clock_source)
{
    uint32_t rtcsel = 0U;
    int ret = tiku_stm32f411_rtc_source_to_rtcsel(clock_source, &rtcsel);

    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    uint32_t bdcr = RCC->BDCR;
    if ((bdcr & RCC_BDCR_RTCEN) != 0U) {
        if ((bdcr & RCC_BDCR_RTCSEL) != rtcsel) {
            return TIKU_STM32F411_RTC_ERR_SOURCE_MISMATCH;
        }

        return tiku_stm32f411_rtc_enable_clock_source(clock_source);
    }

    ret = tiku_stm32f411_rtc_enable_clock_source(clock_source);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | rtcsel;
    RCC->BDCR |= RCC_BDCR_RTCEN;
    return TIKU_STM32F411_RTC_OK;
}

static int
tiku_stm32f411_rtc_clock_matches(
    tiku_stm32f411_rtc_clock_source_t clock_source)
{
    uint32_t rtcsel = 0U;
    int ret = tiku_stm32f411_rtc_source_to_rtcsel(clock_source, &rtcsel);

    if (ret != TIKU_STM32F411_RTC_OK) {
        return 0;
    }

    return ((RCC->BDCR & RCC_BDCR_RTCEN) != 0U) &&
           ((RCC->BDCR & RCC_BDCR_RTCSEL) == rtcsel);
}

static int
tiku_stm32f411_rtc_backup_magic_valid(void)
{
    return (RTC->BKP0R == TIKU_STM32F411_RTC_BKP_MAGIC) ? 1 : 0;
}

static int
tiku_stm32f411_rtc_is_initialized(void)
{
    return ((RCC->BDCR & RCC_BDCR_RTCEN) != 0U) &&
           ((RTC->ISR & RTC_ISR_INITS) != 0U);
}

static inline uint8_t
tiku_stm32f411_rtc_bcd_to_u8(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

static inline uint8_t
tiku_stm32f411_rtc_u8_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static inline int
tiku_stm32f411_rtc_bcd_valid(uint8_t bcd, uint8_t max)
{
    if (((bcd >> 4) > 9U) || ((bcd & 0x0FU) > 9U)) {
        return 0;
    }

    return tiku_stm32f411_rtc_bcd_to_u8(bcd) <= max;
}

static int
tiku_stm32f411_rtc_is_leap_year(uint16_t year)
{
    return ((year % 4U) == 0U) &&
           (((year % 100U) != 0U) || ((year % 400U) == 0U));
}

static uint8_t
tiku_stm32f411_rtc_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t month_days[12] = {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };

    if ((month < 1U) || (month > 12U)) {
        return 0U;
    }

    if ((month == 2U) && tiku_stm32f411_rtc_is_leap_year(year)) {
        return 29U;
    }

    return month_days[month - 1U];
}

static uint32_t
tiku_stm32f411_rtc_days_before_year(uint16_t year)
{
    uint32_t days = 0U;

    for (uint16_t y = TIKU_STM32F411_RTC_EPOCH_YEAR; y < year; y++) {
        days += tiku_stm32f411_rtc_is_leap_year(y) ? 366U : 365U;
    }

    return days;
}

static uint32_t
tiku_stm32f411_rtc_days_before_month(uint16_t year, uint8_t month)
{
    uint32_t days = 0U;

    for (uint8_t m = 1U; m < month; m++) {
        days += tiku_stm32f411_rtc_days_in_month(year, m);
    }

    return days;
}

static uint32_t
tiku_stm32f411_rtc_days_since_epoch(uint16_t year,
                                    uint8_t month,
                                    uint8_t day)
{
    return tiku_stm32f411_rtc_days_before_year(year) +
           tiku_stm32f411_rtc_days_before_month(year, month) +
           (uint32_t)(day - 1U);
}

static uint8_t
tiku_stm32f411_rtc_weekday_from_days(uint32_t days_since_epoch)
{
    /* STM32 weekday numbering is Monday=1 ... Sunday=7; 2000-01-01 was Saturday. */
    return (uint8_t)(((days_since_epoch + 5U) % 7U) + 1U);
}

static uint8_t
tiku_stm32f411_rtc_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    return tiku_stm32f411_rtc_weekday_from_days(
        tiku_stm32f411_rtc_days_since_epoch(year, month, day));
}

static int
tiku_stm32f411_rtc_calendar_validate(
    const tiku_stm32f411_rtc_calendar_t *calendar)
{
    if (calendar == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if ((calendar->year < TIKU_STM32F411_RTC_EPOCH_YEAR) ||
        (calendar->year > TIKU_STM32F411_RTC_MAX_YEAR)) {
        return TIKU_STM32F411_RTC_ERR_RANGE;
    }

    uint8_t days_in_month = tiku_stm32f411_rtc_days_in_month(
        calendar->year, calendar->month);
    if ((days_in_month == 0U) ||
        (calendar->day < 1U) ||
        (calendar->day > days_in_month) ||
        (calendar->weekday < 1U) ||
        (calendar->weekday > 7U) ||
        (calendar->hour > 23U) ||
        (calendar->minute > 59U) ||
        (calendar->second > 59U)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (calendar->weekday != tiku_stm32f411_rtc_weekday(calendar->year,
                                                        calendar->month,
                                                        calendar->day)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    return TIKU_STM32F411_RTC_OK;
}

static tiku_stm32f411_rtc_ticks_t
tiku_stm32f411_rtc_max_ticks(void)
{
    tiku_stm32f411_rtc_calendar_t calendar = {
        TIKU_STM32F411_RTC_MAX_YEAR,
        12U,
        31U,
        tiku_stm32f411_rtc_weekday(TIKU_STM32F411_RTC_MAX_YEAR, 12U, 31U),
        23U,
        59U,
        59U,
    };
    tiku_stm32f411_rtc_ticks_t ticks =
        (tiku_stm32f411_rtc_ticks_t)tiku_stm32f411_rtc_days_since_epoch(
            calendar.year, calendar.month, calendar.day) *
        TIKU_STM32F411_RTC_SECONDS_DAY;

    ticks += ((tiku_stm32f411_rtc_ticks_t)calendar.hour * 3600ULL) +
             ((tiku_stm32f411_rtc_ticks_t)calendar.minute * 60ULL) +
             calendar.second;

    return ticks;
}

static TIKU_STM32F411_RTC_UNUSED uint32_t
tiku_stm32f411_rtc_pack_tr(const tiku_stm32f411_rtc_calendar_t *calendar)
{
    uint8_t bcd_hour = tiku_stm32f411_rtc_u8_to_bcd(calendar->hour);
    uint8_t bcd_minute = tiku_stm32f411_rtc_u8_to_bcd(calendar->minute);
    uint8_t bcd_second = tiku_stm32f411_rtc_u8_to_bcd(calendar->second);

    return (((uint32_t)(bcd_hour >> 4) & 0x3UL) << RTC_TR_HT_Pos) |
           (((uint32_t)bcd_hour & 0xFUL) << RTC_TR_HU_Pos) |
           (((uint32_t)(bcd_minute >> 4) & 0x7UL) << RTC_TR_MNT_Pos) |
           (((uint32_t)bcd_minute & 0xFUL) << RTC_TR_MNU_Pos) |
           (((uint32_t)(bcd_second >> 4) & 0x7UL) << RTC_TR_ST_Pos) |
           (((uint32_t)bcd_second & 0xFUL) << RTC_TR_SU_Pos);
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_unpack_tr(uint32_t tr,
                             tiku_stm32f411_rtc_calendar_t *calendar)
{
    if (calendar == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    uint8_t bcd_hour = (uint8_t)(
        ((((tr & RTC_TR_HT) >> RTC_TR_HT_Pos) << 4) |
         ((tr & RTC_TR_HU) >> RTC_TR_HU_Pos)));
    uint8_t bcd_minute = (uint8_t)(
        ((((tr & RTC_TR_MNT) >> RTC_TR_MNT_Pos) << 4) |
         ((tr & RTC_TR_MNU) >> RTC_TR_MNU_Pos)));
    uint8_t bcd_second = (uint8_t)(
        ((((tr & RTC_TR_ST) >> RTC_TR_ST_Pos) << 4) |
         ((tr & RTC_TR_SU) >> RTC_TR_SU_Pos)));

    if (!tiku_stm32f411_rtc_bcd_valid(bcd_hour, 23U) ||
        !tiku_stm32f411_rtc_bcd_valid(bcd_minute, 59U) ||
        !tiku_stm32f411_rtc_bcd_valid(bcd_second, 59U)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    calendar->hour = tiku_stm32f411_rtc_bcd_to_u8(bcd_hour);
    calendar->minute = tiku_stm32f411_rtc_bcd_to_u8(bcd_minute);
    calendar->second = tiku_stm32f411_rtc_bcd_to_u8(bcd_second);

    return TIKU_STM32F411_RTC_OK;
}

static TIKU_STM32F411_RTC_UNUSED uint32_t
tiku_stm32f411_rtc_pack_dr(const tiku_stm32f411_rtc_calendar_t *calendar)
{
    uint8_t bcd_year = tiku_stm32f411_rtc_u8_to_bcd(
        (uint8_t)(calendar->year - TIKU_STM32F411_RTC_EPOCH_YEAR));
    uint8_t bcd_month = tiku_stm32f411_rtc_u8_to_bcd(calendar->month);
    uint8_t bcd_day = tiku_stm32f411_rtc_u8_to_bcd(calendar->day);

    return (((uint32_t)(bcd_year >> 4) & 0xFUL) << RTC_DR_YT_Pos) |
           (((uint32_t)bcd_year & 0xFUL) << RTC_DR_YU_Pos) |
           (((uint32_t)calendar->weekday & 0x7UL) << RTC_DR_WDU_Pos) |
           (((uint32_t)(bcd_month >> 4) & 0x1UL) << RTC_DR_MT_Pos) |
           (((uint32_t)bcd_month & 0xFUL) << RTC_DR_MU_Pos) |
           (((uint32_t)(bcd_day >> 4) & 0x3UL) << RTC_DR_DT_Pos) |
           (((uint32_t)bcd_day & 0xFUL) << RTC_DR_DU_Pos);
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_unpack_dr(uint32_t dr,
                             tiku_stm32f411_rtc_calendar_t *calendar)
{
    if (calendar == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    uint8_t bcd_year = (uint8_t)(
        ((((dr & RTC_DR_YT) >> RTC_DR_YT_Pos) << 4) |
         ((dr & RTC_DR_YU) >> RTC_DR_YU_Pos)));
    uint8_t bcd_month = (uint8_t)(
        ((((dr & RTC_DR_MT) >> RTC_DR_MT_Pos) << 4) |
         ((dr & RTC_DR_MU) >> RTC_DR_MU_Pos)));
    uint8_t bcd_day = (uint8_t)(
        ((((dr & RTC_DR_DT) >> RTC_DR_DT_Pos) << 4) |
         ((dr & RTC_DR_DU) >> RTC_DR_DU_Pos)));

    if (!tiku_stm32f411_rtc_bcd_valid(bcd_year, 99U) ||
        !tiku_stm32f411_rtc_bcd_valid(bcd_month, 12U) ||
        !tiku_stm32f411_rtc_bcd_valid(bcd_day, 31U)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    calendar->year = TIKU_STM32F411_RTC_EPOCH_YEAR +
                     tiku_stm32f411_rtc_bcd_to_u8(bcd_year);
    calendar->month = tiku_stm32f411_rtc_bcd_to_u8(bcd_month);
    calendar->day = tiku_stm32f411_rtc_bcd_to_u8(bcd_day);
    calendar->weekday = (uint8_t)((dr & RTC_DR_WDU) >> RTC_DR_WDU_Pos);

    return tiku_stm32f411_rtc_calendar_validate(calendar);
}

static int
tiku_stm32f411_rtc_pack_alarm_reg(
    const tiku_stm32f411_rtc_alarm_reg_desc_t *desc,
    const tiku_stm32f411_rtc_calendar_t *target,
    tiku_stm32f411_rtc_alarm_mask_t match_mask,
    uint32_t *alarm_reg)
{
    uint32_t reg = 0U;

    if ((desc == 0) || (alarm_reg == 0)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    uint32_t unknown_mask =
        match_mask & ~TIKU_STM32F411_RTC_ALARM_MATCH_ALL;
    if (unknown_mask != 0U) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_DAY) != 0U) &&
        ((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_WEEKDAY) != 0U)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    int ret = tiku_stm32f411_rtc_calendar_validate(target);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    uint8_t bcd_second = tiku_stm32f411_rtc_u8_to_bcd(target->second);
    uint8_t bcd_minute = tiku_stm32f411_rtc_u8_to_bcd(target->minute);
    uint8_t bcd_hour = tiku_stm32f411_rtc_u8_to_bcd(target->hour);

    if ((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_SECOND) == 0U) {
        reg |= desc->msk1;
    }
    if ((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_MINUTE) == 0U) {
        reg |= desc->msk2;
    }
    if ((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_HOUR) == 0U) {
        reg |= desc->msk3;
    }
    if (((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_DAY) == 0U) &&
        ((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_WEEKDAY) == 0U)) {
        reg |= desc->msk4;
    }

    reg |= (((uint32_t)(bcd_second >> 4) & 0x7UL) << desc->st_pos) |
           (((uint32_t)bcd_second & 0xFUL) << desc->su_pos) |
           (((uint32_t)(bcd_minute >> 4) & 0x7UL) << desc->mnt_pos) |
           (((uint32_t)bcd_minute & 0xFUL) << desc->mnu_pos) |
           (((uint32_t)(bcd_hour >> 4) & 0x3UL) << desc->ht_pos) |
           (((uint32_t)bcd_hour & 0xFUL) << desc->hu_pos);

    if ((match_mask & TIKU_STM32F411_RTC_ALARM_MATCH_WEEKDAY) != 0U) {
        reg |= desc->wdsel |
               (((uint32_t)target->weekday & 0xFUL) << desc->du_pos);
    } else {
        uint8_t bcd_day = tiku_stm32f411_rtc_u8_to_bcd(target->day);
        reg |= (((uint32_t)(bcd_day >> 4) & 0x3UL) << desc->dt_pos) |
               (((uint32_t)bcd_day & 0xFUL) << desc->du_pos);
    }

    *alarm_reg = reg;
    return TIKU_STM32F411_RTC_OK;
}

static int
tiku_stm32f411_rtc_unpack_alarm_reg(
    const tiku_stm32f411_rtc_alarm_reg_desc_t *desc,
    uint32_t alarm_reg,
    tiku_stm32f411_rtc_alarm_fields_t *fields)
{
    if ((desc == 0) || (fields == 0)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    uint8_t bcd_second = (uint8_t)(
        ((((alarm_reg >> desc->st_pos) & 0x7UL) << 4) |
         ((alarm_reg >> desc->su_pos) & 0xFUL)));
    uint8_t bcd_minute = (uint8_t)(
        ((((alarm_reg >> desc->mnt_pos) & 0x7UL) << 4) |
         ((alarm_reg >> desc->mnu_pos) & 0xFUL)));
    uint8_t bcd_hour = (uint8_t)(
        ((((alarm_reg >> desc->ht_pos) & 0x3UL) << 4) |
         ((alarm_reg >> desc->hu_pos) & 0xFUL)));

    if (!tiku_stm32f411_rtc_bcd_valid(bcd_second, 59U) ||
        !tiku_stm32f411_rtc_bcd_valid(bcd_minute, 59U) ||
        !tiku_stm32f411_rtc_bcd_valid(bcd_hour, 23U)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    fields->second = tiku_stm32f411_rtc_bcd_to_u8(bcd_second);
    fields->minute = tiku_stm32f411_rtc_bcd_to_u8(bcd_minute);
    fields->hour = tiku_stm32f411_rtc_bcd_to_u8(bcd_hour);
    fields->day = 0U;
    fields->weekday = 0U;
    fields->use_weekday = ((alarm_reg & desc->wdsel) != 0U) ? 1U : 0U;
    fields->match_mask = 0U;

    if ((alarm_reg & desc->msk1) == 0U) {
        fields->match_mask |= TIKU_STM32F411_RTC_ALARM_MATCH_SECOND;
    }
    if ((alarm_reg & desc->msk2) == 0U) {
        fields->match_mask |= TIKU_STM32F411_RTC_ALARM_MATCH_MINUTE;
    }
    if ((alarm_reg & desc->msk3) == 0U) {
        fields->match_mask |= TIKU_STM32F411_RTC_ALARM_MATCH_HOUR;
    }

    if (fields->use_weekday != 0U) {
        fields->weekday = (uint8_t)((alarm_reg >> desc->du_pos) & 0xFUL);
        if ((fields->weekday < 1U) || (fields->weekday > 7U)) {
            return TIKU_STM32F411_RTC_ERR_INVALID;
        }
        if ((alarm_reg & desc->msk4) == 0U) {
            fields->match_mask |= TIKU_STM32F411_RTC_ALARM_MATCH_WEEKDAY;
        }
    } else {
        uint8_t bcd_day = (uint8_t)(
            ((((alarm_reg >> desc->dt_pos) & 0x3UL) << 4) |
             ((alarm_reg >> desc->du_pos) & 0xFUL)));
        if (!tiku_stm32f411_rtc_bcd_valid(bcd_day, 31U)) {
            return TIKU_STM32F411_RTC_ERR_INVALID;
        }
        fields->day = tiku_stm32f411_rtc_bcd_to_u8(bcd_day);
        if ((fields->day < 1U) && ((alarm_reg & desc->msk4) == 0U)) {
            return TIKU_STM32F411_RTC_ERR_INVALID;
        }
        if ((alarm_reg & desc->msk4) == 0U) {
            fields->match_mask |= TIKU_STM32F411_RTC_ALARM_MATCH_DAY;
        }
    }

    return TIKU_STM32F411_RTC_OK;
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_pack_alrmar(
    const tiku_stm32f411_rtc_calendar_t *target,
    tiku_stm32f411_rtc_alarm_mask_t match_mask,
    uint32_t *alarm_reg)
{
    return tiku_stm32f411_rtc_pack_alarm_reg(
        &tiku_stm32f411_rtc_alrmar_desc, target, match_mask, alarm_reg);
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_pack_alrmbr(
    const tiku_stm32f411_rtc_calendar_t *target,
    tiku_stm32f411_rtc_alarm_mask_t match_mask,
    uint32_t *alarm_reg)
{
    return tiku_stm32f411_rtc_pack_alarm_reg(
        &tiku_stm32f411_rtc_alrmbr_desc, target, match_mask, alarm_reg);
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_unpack_alrmar(
    uint32_t alarm_reg,
    tiku_stm32f411_rtc_alarm_fields_t *fields)
{
    return tiku_stm32f411_rtc_unpack_alarm_reg(
        &tiku_stm32f411_rtc_alrmar_desc, alarm_reg, fields);
}

static TIKU_STM32F411_RTC_UNUSED int
tiku_stm32f411_rtc_unpack_alrmbr(
    uint32_t alarm_reg,
    tiku_stm32f411_rtc_alarm_fields_t *fields)
{
    return tiku_stm32f411_rtc_unpack_alarm_reg(
        &tiku_stm32f411_rtc_alrmbr_desc, alarm_reg, fields);
}

static int
tiku_stm32f411_rtc_alarm_valid(tiku_stm32f411_rtc_alarm_id_t alarm_id)
{
    return ((alarm_id == TIKU_STM32F411_RTC_ALARM_A) ||
            (alarm_id == TIKU_STM32F411_RTC_ALARM_B)) ? 1 : 0;
}

static int
tiku_stm32f411_rtc_alarm_wait_write_ready(
    tiku_stm32f411_rtc_alarm_id_t alarm_id)
{
    switch (alarm_id) {
    case TIKU_STM32F411_RTC_ALARM_A:
        return tiku_stm32f411_rtc_spin_until_set(&RTC->ISR, RTC_ISR_ALRAWF);
    case TIKU_STM32F411_RTC_ALARM_B:
        return tiku_stm32f411_rtc_spin_until_set(&RTC->ISR, RTC_ISR_ALRBWF);
    default:
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }
}

static void
tiku_stm32f411_rtc_alarm_exti_enable(void)
{
    EXTI->PR = EXTI_PR_PR17;
    EXTI->RTSR |= EXTI_RTSR_TR17;
    EXTI->IMR |= EXTI_IMR_MR17;

    NVIC_SetPriority(RTC_Alarm_IRQn, TIKU_STM32F411_RTC_NVIC_PRIO);
    NVIC_ClearPendingIRQ(RTC_Alarm_IRQn);
    NVIC_EnableIRQ(RTC_Alarm_IRQn);
}

static void
tiku_stm32f411_rtc_alarm_exti_disable_if_idle(void)
{
    if ((RTC->CR & (RTC_CR_ALRAE | RTC_CR_ALRBE)) != 0U) {
        return;
    }

    EXTI->IMR &= ~EXTI_IMR_MR17;
    EXTI->RTSR &= ~EXTI_RTSR_TR17;
    EXTI->PR = EXTI_PR_PR17;

    NVIC_DisableIRQ(RTC_Alarm_IRQn);
    NVIC_ClearPendingIRQ(RTC_Alarm_IRQn);
}

static void
tiku_stm32f411_rtc_alarm_clear_flag(tiku_stm32f411_rtc_alarm_id_t alarm_id)
{
    switch (alarm_id) {
    case TIKU_STM32F411_RTC_ALARM_A:
        RTC->ISR &= ~RTC_ISR_ALRAF;
        break;
    case TIKU_STM32F411_RTC_ALARM_B:
        RTC->ISR &= ~RTC_ISR_ALRBF;
        break;
    default:
        break;
    }

    EXTI->PR = EXTI_PR_PR17;
}

static int
tiku_stm32f411_rtc_alarm_pack(tiku_stm32f411_rtc_alarm_id_t alarm_id,
                              const tiku_stm32f411_rtc_calendar_t *target,
                              tiku_stm32f411_rtc_alarm_mask_t match_mask,
                              uint32_t *alarm_reg)
{
    switch (alarm_id) {
    case TIKU_STM32F411_RTC_ALARM_A:
        return tiku_stm32f411_rtc_pack_alrmar(target, match_mask, alarm_reg);
    case TIKU_STM32F411_RTC_ALARM_B:
        return tiku_stm32f411_rtc_pack_alrmbr(target, match_mask, alarm_reg);
    default:
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }
}

static int
tiku_stm32f411_rtc_alarm_enabled(tiku_stm32f411_rtc_alarm_id_t alarm_id)
{
    switch (alarm_id) {
    case TIKU_STM32F411_RTC_ALARM_A:
        return ((RTC->CR & RTC_CR_ALRAE) != 0U) ? 1 : 0;
    case TIKU_STM32F411_RTC_ALARM_B:
        return ((RTC->CR & RTC_CR_ALRBE) != 0U) ? 1 : 0;
    default:
        return 0;
    }
}

static int
tiku_stm32f411_rtc_wakeup_clock_valid(
    tiku_stm32f411_rtc_wakeup_clock_t clock_divider)
{
    switch (clock_divider) {
    case TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV16:
    case TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV8:
    case TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV4:
    case TIKU_STM32F411_RTC_WAKEUP_RTCCLK_DIV2:
    case TIKU_STM32F411_RTC_WAKEUP_CK_SPRE_16:
    case TIKU_STM32F411_RTC_WAKEUP_CK_SPRE_17:
        return 1;
    default:
        return 0;
    }
}

static int
tiku_stm32f411_rtc_wakeup_reload(
    uint32_t duration_ticks,
    tiku_stm32f411_rtc_wakeup_clock_t clock_divider,
    uint32_t *wutr)
{
    if ((duration_ticks == 0U) || (wutr == 0)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (!tiku_stm32f411_rtc_wakeup_clock_valid(clock_divider)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    uint32_t reload = duration_ticks - 1U;

    if (clock_divider == TIKU_STM32F411_RTC_WAKEUP_CK_SPRE_17) {
        if ((reload < 0x10000UL) || (reload > 0x1FFFFUL)) {
            return TIKU_STM32F411_RTC_ERR_RANGE;
        }
    } else if (reload > 0xFFFFUL) {
        return TIKU_STM32F411_RTC_ERR_RANGE;
    }

    *wutr = reload & RTC_WUTR_WUT;
    return TIKU_STM32F411_RTC_OK;
}

static void
tiku_stm32f411_rtc_wakeup_exti_enable(void)
{
    EXTI->PR = EXTI_PR_PR22;
    EXTI->RTSR |= EXTI_RTSR_TR22;
    EXTI->IMR |= EXTI_IMR_MR22;

    NVIC_SetPriority(RTC_WKUP_IRQn, TIKU_STM32F411_RTC_NVIC_PRIO);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
    NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

static void
tiku_stm32f411_rtc_wakeup_exti_disable(void)
{
    EXTI->IMR &= ~EXTI_IMR_MR22;
    EXTI->RTSR &= ~EXTI_RTSR_TR22;
    EXTI->PR = EXTI_PR_PR22;

    NVIC_DisableIRQ(RTC_WKUP_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);
}

static void
tiku_stm32f411_rtc_wakeup_clear_flag(void)
{
    RTC->ISR &= ~RTC_ISR_WUTF;
    EXTI->PR = EXTI_PR_PR22;
}

int
tiku_stm32f411_rtc_init(const tiku_stm32f411_rtc_calendar_t *calendar,
                        tiku_stm32f411_rtc_clock_source_t clock_source)
{
    int ret = tiku_stm32f411_rtc_calendar_validate(calendar);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    unsigned long clock_hz = 0UL;
    ret = tiku_stm32f411_rtc_source_hz(clock_source, &clock_hz);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    tiku_stm32f411_rtc_prescaler_t prescaler = { 0U, 0U };
    ret = tiku_stm32f411_rtc_compute_prescaler(clock_hz, &prescaler);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    tiku_atomic_enter();

    tiku_stm32f411_rtc_backup_access_t access = { 0U };
    tiku_stm32f411_rtc_backup_access_begin(&access);

    ret = tiku_stm32f411_rtc_select_clock_source(clock_source);
    if (ret != TIKU_STM32F411_RTC_OK) {
        tiku_stm32f411_rtc_backup_access_end(&access);
        tiku_atomic_exit();
        return ret;
    }

    int preserve_calendar =
        tiku_stm32f411_rtc_clock_matches(clock_source) &&
        ((RTC->ISR & RTC_ISR_INITS) != 0U) &&
        tiku_stm32f411_rtc_backup_magic_valid();

    tiku_stm32f411_rtc_write_unlock();

    if (preserve_calendar) {
        RTC->CR &= ~(RTC_CR_FMT | RTC_CR_BYPSHAD);
        ret = tiku_stm32f411_rtc_shadow_sync();
    } else {
        ret = tiku_stm32f411_rtc_enter_init_mode();
        if (ret == TIKU_STM32F411_RTC_OK) {
            RTC->PRER = tiku_stm32f411_rtc_pack_prer(&prescaler);
            RTC->CR &= ~(RTC_CR_FMT | RTC_CR_BYPSHAD);
            RTC->TR = tiku_stm32f411_rtc_pack_tr(calendar);
            RTC->DR = tiku_stm32f411_rtc_pack_dr(calendar);
            RTC->BKP0R = TIKU_STM32F411_RTC_BKP_MAGIC;
            tiku_stm32f411_rtc_exit_init_mode();
            ret = tiku_stm32f411_rtc_shadow_sync();
        }
    }

    tiku_stm32f411_rtc_write_lock();
    tiku_stm32f411_rtc_backup_access_end(&access);
    tiku_atomic_exit();

    return ret;
}

int
tiku_stm32f411_rtc_set_calendar(
    const tiku_stm32f411_rtc_calendar_t *calendar)
{
    int ret = tiku_stm32f411_rtc_calendar_validate(calendar);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    tiku_atomic_enter();

    tiku_stm32f411_rtc_backup_access_t access = { 0U };
    tiku_stm32f411_rtc_backup_access_begin(&access);
    tiku_stm32f411_rtc_write_unlock();

    ret = tiku_stm32f411_rtc_enter_init_mode();
    if (ret == TIKU_STM32F411_RTC_OK) {
        RTC->TR = tiku_stm32f411_rtc_pack_tr(calendar);
        RTC->DR = tiku_stm32f411_rtc_pack_dr(calendar);
        RTC->BKP0R = TIKU_STM32F411_RTC_BKP_MAGIC;
        tiku_stm32f411_rtc_exit_init_mode();
        ret = tiku_stm32f411_rtc_shadow_sync();
    }

    tiku_stm32f411_rtc_write_lock();
    tiku_stm32f411_rtc_backup_access_end(&access);
    tiku_atomic_exit();

    return ret;
}

int
tiku_stm32f411_rtc_read_calendar(tiku_stm32f411_rtc_calendar_t *calendar)
{
    if (calendar == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    if ((RTC->ISR & RTC_ISR_INIT) != 0U) {
        return TIKU_STM32F411_RTC_ERR_BUSY;
    }

    if (((RTC->CR & RTC_CR_BYPSHAD) == 0U) &&
        ((RTC->ISR & RTC_ISR_RSF) == 0U)) {
        tiku_atomic_enter();

        tiku_stm32f411_rtc_backup_access_t access = { 0U };
        tiku_stm32f411_rtc_backup_access_begin(&access);
        tiku_stm32f411_rtc_write_unlock();

        int ret = tiku_stm32f411_rtc_shadow_sync();

        tiku_stm32f411_rtc_write_lock();
        tiku_stm32f411_rtc_backup_access_end(&access);
        tiku_atomic_exit();

        if (ret != TIKU_STM32F411_RTC_OK) {
            return ret;
        }
    }

    tiku_stm32f411_rtc_calendar_t snapshot = { 0U };
    uint32_t tr = RTC->TR;
    uint32_t dr = RTC->DR;

    int ret = tiku_stm32f411_rtc_unpack_tr(tr, &snapshot);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    ret = tiku_stm32f411_rtc_unpack_dr(dr, &snapshot);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    *calendar = snapshot;
    return TIKU_STM32F411_RTC_OK;
}

int
tiku_stm32f411_rtc_alarm_set(tiku_stm32f411_rtc_alarm_id_t alarm_id,
                             const tiku_stm32f411_rtc_calendar_t *target,
                             tiku_stm32f411_rtc_alarm_mask_t match_mask)
{
    uint32_t alarm_reg = 0U;
    int ret = tiku_stm32f411_rtc_alarm_pack(alarm_id, target, match_mask,
                                            &alarm_reg);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    tiku_atomic_enter();

    tiku_stm32f411_rtc_backup_access_t access = { 0U };
    tiku_stm32f411_rtc_backup_access_begin(&access);
    tiku_stm32f411_rtc_write_unlock();

    switch (alarm_id) {
    case TIKU_STM32F411_RTC_ALARM_A:
        RTC->CR &= ~(RTC_CR_ALRAE | RTC_CR_ALRAIE);
        ret = tiku_stm32f411_rtc_alarm_wait_write_ready(alarm_id);
        if (ret == TIKU_STM32F411_RTC_OK) {
            RTC->ALRMAR = alarm_reg;
            RTC->ALRMASSR = RTC_ALRMASSR_MASKSS;
            tiku_stm32f411_rtc_alarm_clear_flag(alarm_id);
            tiku_stm32f411_rtc_alarm_exti_enable();
            RTC->CR |= RTC_CR_ALRAIE | RTC_CR_ALRAE;
        }
        break;
    case TIKU_STM32F411_RTC_ALARM_B:
        RTC->CR &= ~(RTC_CR_ALRBE | RTC_CR_ALRBIE);
        ret = tiku_stm32f411_rtc_alarm_wait_write_ready(alarm_id);
        if (ret == TIKU_STM32F411_RTC_OK) {
            RTC->ALRMBR = alarm_reg;
            RTC->ALRMBSSR = RTC_ALRMBSSR_MASKSS;
            tiku_stm32f411_rtc_alarm_clear_flag(alarm_id);
            tiku_stm32f411_rtc_alarm_exti_enable();
            RTC->CR |= RTC_CR_ALRBIE | RTC_CR_ALRBE;
        }
        break;
    default:
        ret = TIKU_STM32F411_RTC_ERR_INVALID;
        break;
    }

    tiku_stm32f411_rtc_write_lock();
    tiku_stm32f411_rtc_backup_access_end(&access);
    tiku_atomic_exit();

    return ret;
}

int
tiku_stm32f411_rtc_alarm_cancel(tiku_stm32f411_rtc_alarm_id_t alarm_id)
{
    if (!tiku_stm32f411_rtc_alarm_valid(alarm_id)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    tiku_atomic_enter();

    tiku_stm32f411_rtc_backup_access_t access = { 0U };
    tiku_stm32f411_rtc_backup_access_begin(&access);
    tiku_stm32f411_rtc_write_unlock();

    int was_armed = tiku_stm32f411_rtc_alarm_enabled(alarm_id);
    switch (alarm_id) {
    case TIKU_STM32F411_RTC_ALARM_A:
        RTC->CR &= ~(RTC_CR_ALRAE | RTC_CR_ALRAIE);
        break;
    case TIKU_STM32F411_RTC_ALARM_B:
        RTC->CR &= ~(RTC_CR_ALRBE | RTC_CR_ALRBIE);
        break;
    default:
        break;
    }

    int ret = was_armed ?
        tiku_stm32f411_rtc_alarm_wait_write_ready(alarm_id) :
        TIKU_STM32F411_RTC_ERR_NONE;

    tiku_stm32f411_rtc_alarm_clear_flag(alarm_id);
    tiku_stm32f411_rtc_alarm_exti_disable_if_idle();

    tiku_stm32f411_rtc_write_lock();
    tiku_stm32f411_rtc_backup_access_end(&access);
    tiku_atomic_exit();

    return ret;
}

int
tiku_stm32f411_rtc_alarm_is_armed(tiku_stm32f411_rtc_alarm_id_t alarm_id,
                                  uint8_t *armed)
{
    if (armed == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (!tiku_stm32f411_rtc_alarm_valid(alarm_id)) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    *armed = (uint8_t)tiku_stm32f411_rtc_alarm_enabled(alarm_id);
    return TIKU_STM32F411_RTC_OK;
}

int
tiku_stm32f411_rtc_wakeup_set(
    uint32_t duration_ticks,
    tiku_stm32f411_rtc_wakeup_clock_t clock_divider)
{
    uint32_t wutr = 0U;
    int ret = tiku_stm32f411_rtc_wakeup_reload(duration_ticks,
                                               clock_divider,
                                               &wutr);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    tiku_atomic_enter();

    tiku_stm32f411_rtc_backup_access_t access = { 0U };
    tiku_stm32f411_rtc_backup_access_begin(&access);
    tiku_stm32f411_rtc_write_unlock();

    RTC->CR &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);

    ret = tiku_stm32f411_rtc_spin_until_set(&RTC->ISR, RTC_ISR_WUTWF);
    if (ret == TIKU_STM32F411_RTC_OK) {
        RTC->WUTR = wutr;
        RTC->CR = (RTC->CR & ~RTC_CR_WUCKSEL) |
                  ((uint32_t)clock_divider & RTC_CR_WUCKSEL);
        tiku_stm32f411_rtc_wakeup_clear_flag();
        tiku_stm32f411_rtc_wakeup_exti_enable();
        RTC->CR |= RTC_CR_WUTIE | RTC_CR_WUTE;
    } else {
        tiku_stm32f411_rtc_wakeup_clear_flag();
        tiku_stm32f411_rtc_wakeup_exti_disable();
    }

    tiku_stm32f411_rtc_write_lock();
    tiku_stm32f411_rtc_backup_access_end(&access);
    tiku_atomic_exit();

    return ret;
}

int
tiku_stm32f411_rtc_wakeup_cancel(void)
{
    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    tiku_atomic_enter();

    tiku_stm32f411_rtc_backup_access_t access = { 0U };
    tiku_stm32f411_rtc_backup_access_begin(&access);
    tiku_stm32f411_rtc_write_unlock();

    int was_armed = ((RTC->CR & RTC_CR_WUTE) != 0U) ? 1 : 0;

    RTC->CR &= ~(RTC_CR_WUTE | RTC_CR_WUTIE);

    int ret = was_armed ?
        tiku_stm32f411_rtc_spin_until_set(&RTC->ISR, RTC_ISR_WUTWF) :
        TIKU_STM32F411_RTC_ERR_NONE;

    tiku_stm32f411_rtc_wakeup_clear_flag();
    tiku_stm32f411_rtc_wakeup_exti_disable();

    tiku_stm32f411_rtc_write_lock();
    tiku_stm32f411_rtc_backup_access_end(&access);
    tiku_atomic_exit();

    return ret;
}

int
tiku_stm32f411_rtc_wakeup_is_armed(uint8_t *armed)
{
    if (armed == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (!tiku_stm32f411_rtc_is_initialized()) {
        return TIKU_STM32F411_RTC_ERR_NOT_INIT;
    }

    *armed = ((RTC->CR & RTC_CR_WUTE) != 0U) ? 1U : 0U;
    return TIKU_STM32F411_RTC_OK;
}

int
tiku_stm32f411_rtc_clear_interrupt_flag(
    tiku_stm32f411_rtc_interrupt_source_t source)
{
    switch (source) {
    case TIKU_STM32F411_RTC_IRQ_ALARM_A:
        tiku_stm32f411_rtc_alarm_clear_flag(TIKU_STM32F411_RTC_ALARM_A);
        return TIKU_STM32F411_RTC_OK;
    case TIKU_STM32F411_RTC_IRQ_ALARM_B:
        tiku_stm32f411_rtc_alarm_clear_flag(TIKU_STM32F411_RTC_ALARM_B);
        return TIKU_STM32F411_RTC_OK;
    case TIKU_STM32F411_RTC_IRQ_WAKEUP:
        tiku_stm32f411_rtc_wakeup_clear_flag();
        return TIKU_STM32F411_RTC_OK;
    default:
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }
}

int
tiku_stm32f411_rtc_backup_domain_reset(void)
{
    tiku_atomic_enter();

    NVIC_DisableIRQ(RTC_Alarm_IRQn);
    NVIC_DisableIRQ(RTC_WKUP_IRQn);
    NVIC_ClearPendingIRQ(RTC_Alarm_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);

    EXTI->IMR &= ~(EXTI_IMR_MR17 | EXTI_IMR_MR22);
    EXTI->RTSR &= ~(EXTI_RTSR_TR17 | EXTI_RTSR_TR22);
    EXTI->FTSR &= ~(EXTI_FTSR_TR17 | EXTI_FTSR_TR22);
    EXTI->PR = EXTI_PR_PR17 | EXTI_PR_PR22;

    tiku_stm32f411_rtc_backup_access_t access = { 0U };
    tiku_stm32f411_rtc_backup_access_begin(&access);

    RCC->BDCR |= RCC_BDCR_BDRST;
    (void)RCC->BDCR;
    RCC->BDCR &= ~RCC_BDCR_BDRST;
    (void)RCC->BDCR;

    tiku_stm32f411_rtc_backup_access_end(&access);
    tiku_atomic_exit();

    return TIKU_STM32F411_RTC_OK;
}

void
tiku_stm32f411_exti17_rtc_alarm_irq_handler(void)
{
    uint32_t flags = RTC->ISR & (RTC_ISR_ALRAF | RTC_ISR_ALRBF);

    if (flags != 0U) {
        RTC->ISR &= ~flags;
    }

    EXTI->PR = EXTI_PR_PR17;
}

void
tiku_stm32f411_exti22_rtc_wkup_irq_handler(void)
{
    if ((RTC->ISR & RTC_ISR_WUTF) != 0U) {
        RTC->ISR &= ~RTC_ISR_WUTF;
    }

    EXTI->PR = EXTI_PR_PR22;
}

int
tiku_stm32f411_rtc_calendar_to_ticks(
    const tiku_stm32f411_rtc_calendar_t *calendar,
    tiku_stm32f411_rtc_ticks_t *ticks)
{
    if (ticks == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    int ret = tiku_stm32f411_rtc_calendar_validate(calendar);
    if (ret != TIKU_STM32F411_RTC_OK) {
        return ret;
    }

    tiku_stm32f411_rtc_ticks_t value =
        (tiku_stm32f411_rtc_ticks_t)tiku_stm32f411_rtc_days_since_epoch(
            calendar->year, calendar->month, calendar->day) *
        TIKU_STM32F411_RTC_SECONDS_DAY;

    value += ((tiku_stm32f411_rtc_ticks_t)calendar->hour * 3600ULL) +
             ((tiku_stm32f411_rtc_ticks_t)calendar->minute * 60ULL) +
             calendar->second;

    *ticks = value;
    return TIKU_STM32F411_RTC_OK;
}

int
tiku_stm32f411_rtc_ticks_to_calendar(
    tiku_stm32f411_rtc_ticks_t ticks,
    tiku_stm32f411_rtc_calendar_t *calendar)
{
    if (calendar == 0) {
        return TIKU_STM32F411_RTC_ERR_INVALID;
    }

    if (ticks > tiku_stm32f411_rtc_max_ticks()) {
        return TIKU_STM32F411_RTC_ERR_RANGE;
    }

    uint32_t days = (uint32_t)(ticks / TIKU_STM32F411_RTC_SECONDS_DAY);
    uint32_t seconds = (uint32_t)(ticks % TIKU_STM32F411_RTC_SECONDS_DAY);

    uint16_t year = TIKU_STM32F411_RTC_EPOCH_YEAR;
    while (days >= (uint32_t)(tiku_stm32f411_rtc_is_leap_year(year) ?
                              366U : 365U)) {
        days -= tiku_stm32f411_rtc_is_leap_year(year) ? 366U : 365U;
        year++;
    }

    uint8_t month = 1U;
    while (days >= tiku_stm32f411_rtc_days_in_month(year, month)) {
        days -= tiku_stm32f411_rtc_days_in_month(year, month);
        month++;
    }

    calendar->year = year;
    calendar->month = month;
    calendar->day = (uint8_t)(days + 1U);
    calendar->weekday = tiku_stm32f411_rtc_weekday(calendar->year,
                                                   calendar->month,
                                                   calendar->day);
    calendar->hour = (uint8_t)(seconds / 3600U);
    seconds %= 3600U;
    calendar->minute = (uint8_t)(seconds / 60U);
    calendar->second = (uint8_t)(seconds % 60U);

    return TIKU_STM32F411_RTC_OK;
}
