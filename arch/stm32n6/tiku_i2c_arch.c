/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tiku_i2c_arch.c - STM32N6 polled I2C master.
 *
 * The board header selects I2C1 or I2C2 and its SCL/SDA pins. The peripheral
 * kernel clock is fixed to HSI so timing remains valid when CPU/APB clocks
 * change. Transfers are synchronous and use bounded status polling.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_i2c_arch.h"

#include <stddef.h>
#include <arch/stm32n6/tiku_device_select.h>
#include <arch/stm32n6/tiku_gpio_arch.h>
#include <arch/stm32n6/tiku_stm32n6_regs.h>

#ifdef TIKU_STM32N6_I2C_HOST_TEST
extern uint32_t tiku_i2c_host_read(uintptr_t address);
extern void tiku_i2c_host_write(uintptr_t address, uint32_t value);
extern void tiku_i2c_host_gpio_alt_od(uint8_t port, uint8_t pin, uint8_t af);
#define I2C_REG_READ(address)       tiku_i2c_host_read((uintptr_t)(address))
#define I2C_REG_WRITE(address, val) tiku_i2c_host_write((uintptr_t)(address), \
                                                         (uint32_t)(val))
#else
#define I2C_REG_READ(address)       TIKU_REG32(address)
#define I2C_REG_WRITE(address, val) (TIKU_REG32(address) = (uint32_t)(val))
#endif

#if TIKU_BOARD_I2C_INSTANCE == 1U
#define I2C_BASE                    STM32N6_I2C1_BASE
#define I2C_RCC_MASK                STM32N6_RCC_APB1_I2C1
#define I2C_RCC_SEL_MASK            STM32N6_RCC_CCIPR4_I2C1SEL
#define I2C_RCC_SEL_POS             0U
#elif TIKU_BOARD_I2C_INSTANCE == 2U
#define I2C_BASE                    STM32N6_I2C2_BASE
#define I2C_RCC_MASK                STM32N6_RCC_APB1_I2C2
#define I2C_RCC_SEL_MASK            STM32N6_RCC_CCIPR4_I2C2SEL
#define I2C_RCC_SEL_POS             4U
#else
#error "STM32N6 I2C backend supports I2C1 and I2C2 board mappings"
#endif

#ifndef I2C_POLL_SPINS
#define I2C_POLL_SPINS              2000000UL
#endif
#define I2C_TIMING_RISE_NS          300UL
#define I2C_TIMING_FALL_NS          100UL
#define I2C_TIMING_FILTER_NS        50UL

static uint8_t i2c_up;
static uint32_t i2c_timingr;

/** @brief Ceiling division for positive integer timing calculations. */
static uint32_t i2c_ceil_div(uint32_t numerator, uint32_t denominator)
{
    return (numerator + denominator - 1UL) / denominator;
}

/** @brief Ceiling division where the nanosecond numerator needs 64 bits. */
static uint32_t i2c_ceil_div64(uint64_t numerator, uint32_t denominator)
{
    return (uint32_t)((numerator + denominator - 1ULL) / denominator);
}

/** @brief Return the I2C kernel rate after selecting HSI. */
static uint32_t i2c_hsi_hz(void)
{
    uint32_t div = (I2C_REG_READ(STM32N6_RCC_HSICFGR) &
                    STM32N6_RCC_HSICFGR_DIV_MSK) >>
                   STM32N6_RCC_HSICFGR_DIV_POS;
    return (uint32_t)(STM32N6_HSI_HZ >> div);
}

/**
 * @brief Build a conservative TIMINGR value for Standard or Fast mode.
 *
 * SCL high and low targets include margin over the I2C minimums. The HSI
 * source is divided to a tick near 100 ns; the analog filter remains enabled
 * and the digital filter is disabled.
 */
static uint32_t i2c_timing(uint32_t kernel_hz, uint8_t speed)
{
    uint32_t presc_div = i2c_ceil_div(kernel_hz, 10000000UL);
    uint32_t tick_ns;
    uint32_t scldel_ns;
    uint32_t high_ns;
    uint32_t low_ns;
    uint32_t scldel;
    uint32_t sclh;
    uint32_t scll;
    uint32_t presc;

    if (presc_div < 1UL) {
        presc_div = 1UL;
    } else if (presc_div > 16UL) {
        presc_div = 16UL;
    }
    presc = presc_div - 1UL;
    tick_ns = i2c_ceil_div64((uint64_t)presc_div * 1000000000ULL,
                             kernel_hz);

    if (speed == TIKU_I2C_SPEED_FAST) {
        scldel_ns = I2C_TIMING_RISE_NS + 100UL; /* tSU;DAT */
        high_ns = 650UL;
        low_ns = 1450UL;
    } else {
        scldel_ns = I2C_TIMING_RISE_NS + 250UL; /* tSU;DAT */
        high_ns = 4500UL;
        low_ns = 5500UL;
    }

    scldel = i2c_ceil_div(scldel_ns, tick_ns);
    sclh = i2c_ceil_div(high_ns + I2C_TIMING_FILTER_NS, tick_ns);
    scll = i2c_ceil_div(low_ns + I2C_TIMING_FILTER_NS +
                        I2C_TIMING_FALL_NS, tick_ns);

    /* Register fields encode count minus one and are four/eight bits wide. */
    if (scldel < 1UL) scldel = 1UL;
    if (scldel > 16UL) scldel = 16UL;
    if (sclh < 1UL) sclh = 1UL;
    if (sclh > 256UL) sclh = 256UL;
    if (scll < 1UL) scll = 1UL;
    if (scll > 256UL) scll = 256UL;

    return ((presc & 0xFUL) << 28U) |
           (((scldel - 1UL) & 0xFUL) << 20U) |
           (0UL << 16U) | /* SDADEL: external pull-ups; analog filter active */
           (((sclh - 1UL) & 0xFFUL) << 8U) |
           ((scll - 1UL) & 0xFFUL);
}

/** @brief Configure the board's SCL and SDA pads as AF open-drain. */
static void i2c_pins_init(void)
{
#ifdef TIKU_STM32N6_I2C_HOST_TEST
    tiku_i2c_host_gpio_alt_od(TIKU_BOARD_I2C_SCL_PORT,
                              TIKU_BOARD_I2C_SCL_PIN, TIKU_BOARD_I2C_AF);
    tiku_i2c_host_gpio_alt_od(TIKU_BOARD_I2C_SDA_PORT,
                              TIKU_BOARD_I2C_SDA_PIN, TIKU_BOARD_I2C_AF);
#else
    uint32_t shift;

    tiku_stm32n6_gpio_init_alt(TIKU_BOARD_I2C_SCL_PORT,
                               TIKU_BOARD_I2C_SCL_PIN, TIKU_BOARD_I2C_AF);
    tiku_stm32n6_gpio_init_alt(TIKU_BOARD_I2C_SDA_PORT,
                               TIKU_BOARD_I2C_SDA_PIN, TIKU_BOARD_I2C_AF);
    I2C_REG_WRITE(STM32N6_GPIO_OTYPER(TIKU_BOARD_I2C_SCL_PORT),
                  I2C_REG_READ(STM32N6_GPIO_OTYPER(TIKU_BOARD_I2C_SCL_PORT)) |
                  (1UL << TIKU_BOARD_I2C_SCL_PIN));
    I2C_REG_WRITE(STM32N6_GPIO_OTYPER(TIKU_BOARD_I2C_SDA_PORT),
                  I2C_REG_READ(STM32N6_GPIO_OTYPER(TIKU_BOARD_I2C_SDA_PORT)) |
                  (1UL << TIKU_BOARD_I2C_SDA_PIN));

    shift = (uint32_t)TIKU_BOARD_I2C_SCL_PIN * 2UL;
    I2C_REG_WRITE(STM32N6_GPIO_PUPDR(TIKU_BOARD_I2C_SCL_PORT),
                  I2C_REG_READ(STM32N6_GPIO_PUPDR(TIKU_BOARD_I2C_SCL_PORT)) &
                  ~(3UL << shift));
    shift = (uint32_t)TIKU_BOARD_I2C_SDA_PIN * 2UL;
    I2C_REG_WRITE(STM32N6_GPIO_PUPDR(TIKU_BOARD_I2C_SDA_PORT),
                  I2C_REG_READ(STM32N6_GPIO_PUPDR(TIKU_BOARD_I2C_SDA_PORT)) &
                  ~(3UL << shift));
#endif
}

/** @brief Clear latched error/stop flags using the ICR write-one-to-clear bits. */
static void i2c_clear_flags(void)
{
    I2C_REG_WRITE(STM32N6_I2C_ICR(I2C_BASE), STM32N6_I2C_ICR_ALL);
}

/** @brief Convert an ISR error condition to the portable bus status. */
static int i2c_error(uint32_t status)
{
    if ((status & STM32N6_I2C_ISR_NACKF) != 0UL) {
        I2C_REG_WRITE(STM32N6_I2C_ICR(I2C_BASE), STM32N6_I2C_ICR_NACKCF);
        return TIKU_I2C_ERR_NACK;
    }
    if ((status & STM32N6_I2C_ISR_ARLO) != 0UL) {
        I2C_REG_WRITE(STM32N6_I2C_ICR(I2C_BASE), STM32N6_I2C_ICR_ARLOCF);
        return TIKU_I2C_ERR_BUSY;
    }
    if ((status & (STM32N6_I2C_ISR_BERR | STM32N6_I2C_ISR_OVR |
                   STM32N6_I2C_ISR_TIMEOUT)) != 0UL) {
        I2C_REG_WRITE(STM32N6_I2C_ICR(I2C_BASE), STM32N6_I2C_ICR_ALL);
        return TIKU_I2C_ERR_TIMEOUT;
    }
    return TIKU_I2C_OK;
}

/** @brief Wait for an ISR condition, stopping early on NACK or bus error. */
static int i2c_wait(uint32_t mask)
{
    unsigned long spins;
    for (spins = 0UL; spins < I2C_POLL_SPINS; ++spins) {
        uint32_t status = I2C_REG_READ(STM32N6_I2C_ISR(I2C_BASE));
        int error = i2c_error(status);
        if (error != TIKU_I2C_OK) {
            return error;
        }
        if ((status & mask) != 0UL) {
            return TIKU_I2C_OK;
        }
    }
    return TIKU_I2C_ERR_TIMEOUT;
}

/** @brief Wait until STOPF is raised, then clear it. */
static int i2c_wait_stop(void)
{
    int rc = i2c_wait(STM32N6_I2C_ISR_STOPF);
    if (rc == TIKU_I2C_OK) {
        I2C_REG_WRITE(STM32N6_I2C_ICR(I2C_BASE), STM32N6_I2C_ICR_STOPCF);
    }
    return rc;
}

/** @brief Request STOP and restore a known enabled/idle peripheral state. */
static void i2c_abort(void)
{
    uint32_t status = I2C_REG_READ(STM32N6_I2C_ISR(I2C_BASE));
    if ((status & STM32N6_I2C_ISR_BUSY) != 0UL) {
        I2C_REG_WRITE(STM32N6_I2C_CR2(I2C_BASE),
                      I2C_REG_READ(STM32N6_I2C_CR2(I2C_BASE)) |
                      STM32N6_I2C_CR2_STOP);
        (void)i2c_wait_stop();
    }
    i2c_clear_flags();
    I2C_REG_WRITE(STM32N6_RCC_APB1LRSTR,
                  I2C_REG_READ(STM32N6_RCC_APB1LRSTR) | I2C_RCC_MASK);
    I2C_REG_WRITE(STM32N6_RCC_APB1LRSTR,
                  I2C_REG_READ(STM32N6_RCC_APB1LRSTR) & ~I2C_RCC_MASK);
    I2C_REG_WRITE(STM32N6_I2C_TIMINGR(I2C_BASE), i2c_timingr);
    I2C_REG_WRITE(STM32N6_I2C_CR1(I2C_BASE), 0UL);
    I2C_REG_WRITE(STM32N6_I2C_CR1(I2C_BASE), STM32N6_I2C_CR1_PE);
}

/** @brief Program one message phase, with at most 255 bytes per reload. */
static void i2c_set_phase(uint8_t addr, uint8_t read, uint16_t count,
                          uint8_t reload, uint8_t autoend, uint8_t start)
{
    uint32_t value = ((uint32_t)count << STM32N6_I2C_CR2_NBYTES_POS);
    if (start != 0U) {
        value |= ((uint32_t)addr << STM32N6_I2C_CR2_SADD_POS);
        if (read != 0U) value |= STM32N6_I2C_CR2_RD_WRN;
    }
    if (reload != 0U) value |= STM32N6_I2C_CR2_RELOAD;
    if (autoend != 0U) value |= STM32N6_I2C_CR2_AUTOEND;
    if (start != 0U) value |= STM32N6_I2C_CR2_START;
    I2C_REG_WRITE(STM32N6_I2C_CR2(I2C_BASE), value);
}

/**
 * @brief Send or receive one message phase.
 *
 * @param addr       7-bit target address
 * @param read       Nonzero for receive direction
 * @param tx         Transmit buffer, ignored for receive
 * @param rx         Receive buffer, ignored for transmit
 * @param len        Number of bytes in this phase
 * @param start      Nonzero to issue START (also creates repeated START)
 * @param autoend    Nonzero to issue STOP after the final byte
 */
static int i2c_phase(uint8_t addr, uint8_t read, const uint8_t *tx,
                     uint8_t *rx, uint16_t len, uint8_t start,
                     uint8_t autoend)
{
    uint16_t offset = 0U;
    uint8_t first = 1U;

    while (offset < len) {
        uint16_t left = (uint16_t)(len - offset);
        uint16_t chunk = (left > 255U) ? 255U : left;
        uint8_t more = (uint8_t)(left > chunk);
        i2c_set_phase(addr, read, chunk, more, (uint8_t)(!more && autoend),
                      (uint8_t)(first && start));

        if (read != 0U) {
            uint16_t i;
            for (i = 0U; i < chunk; ++i) {
                int rc = i2c_wait(STM32N6_I2C_ISR_RXNE);
                if (rc != TIKU_I2C_OK) return rc;
                rx[offset + i] = (uint8_t)I2C_REG_READ(
                    STM32N6_I2C_RXDR(I2C_BASE));
            }
        } else {
            uint16_t i;
            for (i = 0U; i < chunk; ++i) {
                int rc = i2c_wait(STM32N6_I2C_ISR_TXIS);
                if (rc != TIKU_I2C_OK) return rc;
                I2C_REG_WRITE(STM32N6_I2C_TXDR(I2C_BASE), tx[offset + i]);
            }
        }

        offset = (uint16_t)(offset + chunk);
        if (more != 0U) {
            int rc = i2c_wait(STM32N6_I2C_ISR_TCR);
            if (rc != TIKU_I2C_OK) return rc;
        } else if (autoend == 0U) {
            int rc = i2c_wait(STM32N6_I2C_ISR_TC);
            if (rc != TIKU_I2C_OK) return rc;
        }
        first = 0U;
    }

    if (autoend != 0U) {
        return i2c_wait_stop();
    }
    return TIKU_I2C_OK;
}

/** @brief Address-only transaction used by probe(). */
static int i2c_probe_transfer(uint8_t addr)
{
    i2c_set_phase(addr, 0U, 0U, 0U, 1U, 1U);
    return i2c_wait_stop();
}

/** @brief Verify state and 7-bit address before touching the peripheral. */
static int i2c_check(uint8_t addr)
{
    uint32_t status;
    if (i2c_up == 0U || addr > 0x7FU) {
        return TIKU_I2C_ERR_PARAM;
    }
    status = I2C_REG_READ(STM32N6_I2C_ISR(I2C_BASE));
    if ((status & STM32N6_I2C_ISR_BUSY) != 0UL) {
        return TIKU_I2C_ERR_BUSY;
    }
    i2c_clear_flags();
    return TIKU_I2C_OK;
}

int tiku_i2c_arch_init(const tiku_i2c_config_t *config)
{
    unsigned long spins;
    uint32_t ccipr;
    uint32_t timing;

    if (i2c_up != 0U) tiku_i2c_arch_close();
    if (config == NULL ||
        (config->speed != TIKU_I2C_SPEED_STANDARD &&
         config->speed != TIKU_I2C_SPEED_FAST)) {
        return TIKU_I2C_ERR_PARAM;
    }

    /* Ensure HSI is ready before selecting it as the independent kernel clock. */
    I2C_REG_WRITE(STM32N6_RCC_CR,
                  I2C_REG_READ(STM32N6_RCC_CR) | STM32N6_RCC_CR_HSION);
    for (spins = 0UL; spins < I2C_POLL_SPINS; ++spins) {
        if ((I2C_REG_READ(STM32N6_RCC_SR) & STM32N6_RCC_SR_HSIRDY) != 0UL) {
            break;
        }
    }
    if (spins == I2C_POLL_SPINS) {
        return TIKU_I2C_ERR_TIMEOUT;
    }

    i2c_pins_init();
    I2C_REG_WRITE(STM32N6_RCC_APB1LENR,
                  I2C_REG_READ(STM32N6_RCC_APB1LENR) | I2C_RCC_MASK);
    I2C_REG_WRITE(STM32N6_RCC_APB1LRSTR,
                  I2C_REG_READ(STM32N6_RCC_APB1LRSTR) | I2C_RCC_MASK);
    I2C_REG_WRITE(STM32N6_RCC_APB1LRSTR,
                  I2C_REG_READ(STM32N6_RCC_APB1LRSTR) & ~I2C_RCC_MASK);

    ccipr = I2C_REG_READ(STM32N6_RCC_CCIPR4);
    ccipr &= ~I2C_RCC_SEL_MASK;
    ccipr |= (STM32N6_RCC_I2C_SEL_HSI << I2C_RCC_SEL_POS);
    I2C_REG_WRITE(STM32N6_RCC_CCIPR4, ccipr);

    I2C_REG_WRITE(STM32N6_I2C_CR1(I2C_BASE), 0UL);
    timing = i2c_timing(i2c_hsi_hz(), config->speed);
    i2c_timingr = timing;
    I2C_REG_WRITE(STM32N6_I2C_TIMINGR(I2C_BASE), i2c_timingr);
    i2c_clear_flags();
    I2C_REG_WRITE(STM32N6_I2C_CR1(I2C_BASE), STM32N6_I2C_CR1_PE);
    i2c_up = 1U;
    return TIKU_I2C_OK;
}

void tiku_i2c_arch_close(void)
{
    if (i2c_up != 0U) {
        I2C_REG_WRITE(STM32N6_I2C_CR1(I2C_BASE), 0UL);
        (void)tiku_gpio_arch_set_input(TIKU_BOARD_I2C_SCL_PORT,
                                       TIKU_BOARD_I2C_SCL_PIN);
        (void)tiku_gpio_arch_set_input(TIKU_BOARD_I2C_SDA_PORT,
                                       TIKU_BOARD_I2C_SDA_PIN);
        I2C_REG_WRITE(STM32N6_RCC_APB1LENR,
                      I2C_REG_READ(STM32N6_RCC_APB1LENR) & ~I2C_RCC_MASK);
    }
    i2c_up = 0U;
}

int tiku_i2c_arch_write(uint8_t addr, const uint8_t *buf, uint16_t len)
{
    int rc = i2c_check(addr);
    if (rc != TIKU_I2C_OK) return rc;
    if (buf == NULL || len == 0U) {
        return TIKU_I2C_ERR_PARAM;
    }
    rc = i2c_phase(addr, 0U, buf, NULL, len, 1U, 1U);
    if (rc != TIKU_I2C_OK) i2c_abort();
    return rc;
}

int tiku_i2c_arch_read(uint8_t addr, uint8_t *buf, uint16_t len)
{
    int rc = i2c_check(addr);
    if (rc != TIKU_I2C_OK) return rc;
    if (buf == NULL || len == 0U) {
        return TIKU_I2C_ERR_PARAM;
    }
    rc = i2c_phase(addr, 1U, NULL, buf, len, 1U, 1U);
    if (rc != TIKU_I2C_OK) i2c_abort();
    return rc;
}

int tiku_i2c_arch_probe(uint8_t addr)
{
    int rc = i2c_check(addr);
    if (rc != TIKU_I2C_OK) return rc;
    rc = i2c_probe_transfer(addr);
    if (rc != TIKU_I2C_OK) i2c_abort();
    return rc;
}

int tiku_i2c_arch_write_read(uint8_t addr,
                             const uint8_t *tx_buf, uint16_t tx_len,
                             uint8_t *rx_buf, uint16_t rx_len)
{
    int rc = i2c_check(addr);
    if (rc != TIKU_I2C_OK) return rc;
    if (tx_buf == NULL || rx_buf == NULL || tx_len == 0U || rx_len == 0U) {
        return TIKU_I2C_ERR_PARAM;
    }

    rc = i2c_phase(addr, 0U, tx_buf, NULL, tx_len, 1U, 0U);
    if (rc == TIKU_I2C_OK) {
        rc = i2c_phase(addr, 1U, NULL, rx_buf, rx_len, 1U, 1U);
    }
    if (rc != TIKU_I2C_OK) i2c_abort();
    return rc;
}
