/* Host-side register-sequence tests for the STM32N6 I2C master. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <arch/stm32n6/tiku_device_select.h>
#include <arch/stm32n6/tiku_i2c_arch.h>
#include <arch/stm32n6/tiku_stm32n6_regs.h>

typedef struct {
    uintptr_t address;
    uint32_t value;
} mock_reg_t;

static mock_reg_t mock_regs[64];
static size_t mock_reg_count;
static uint8_t fail_mode;
static uint32_t starts;
static uint32_t stops;
static uint32_t phase_count;
static uint32_t phase_tx;
static uint32_t phase_rx;
static uint32_t total_tx;
static uint32_t current_nbytes;
static uint32_t current_read;
static uint32_t current_reload;
static uint32_t current_autoend;
static uint32_t stop_reported;
static uint32_t standard_timing;
static uint8_t gpio_count;
static uint8_t input_count;

#define MOCK_FAIL_NONE       0U
#define MOCK_FAIL_NACK       1U
#define MOCK_FAIL_TIMEOUT    2U
#define MOCK_FAIL_BUSY       3U

static mock_reg_t *mock_find(uintptr_t address)
{
    size_t i;
    for (i = 0U; i < mock_reg_count; ++i) {
        if (mock_regs[i].address == address) return &mock_regs[i];
    }
    assert(mock_reg_count < (sizeof(mock_regs) / sizeof(mock_regs[0])));
    mock_regs[mock_reg_count].address = address;
    mock_regs[mock_reg_count].value = 0U;
    return &mock_regs[mock_reg_count++];
}

uint32_t tiku_i2c_host_read(uintptr_t address)
{
#if TIKU_BOARD_I2C_INSTANCE == 1U
    const uintptr_t i2c_base = STM32N6_I2C1_BASE;
#else
    const uintptr_t i2c_base = STM32N6_I2C2_BASE;
#endif
    if (address == STM32N6_RCC_SR) return STM32N6_RCC_SR_HSIRDY;
    if (address == STM32N6_I2C_RXDR(i2c_base)) {
        uint32_t value = 0xA0UL + phase_rx;
        ++phase_rx;
        return value;
    }
    if (address == STM32N6_I2C_ISR(i2c_base)) {
        if (fail_mode == MOCK_FAIL_BUSY) return STM32N6_I2C_ISR_BUSY;
        if (fail_mode == MOCK_FAIL_NACK && starts != 0U) {
            return STM32N6_I2C_ISR_NACKF;
        }
        if (fail_mode == MOCK_FAIL_TIMEOUT && starts != 0U) return 0U;
        if (current_nbytes == 0U && starts != 0U) {
            if (stop_reported == 0U) {
                ++stops;
                stop_reported = 1U;
            }
            return STM32N6_I2C_ISR_STOPF;
        }
        if (current_read != 0U) {
            if (phase_rx < current_nbytes) return STM32N6_I2C_ISR_RXNE;
        } else if (phase_tx < current_nbytes) {
            return STM32N6_I2C_ISR_TXIS;
        }
        if (current_reload != 0U) return STM32N6_I2C_ISR_TCR;
        if (current_autoend != 0U) {
            if (stop_reported == 0U) {
                ++stops;
                stop_reported = 1U;
            }
            return STM32N6_I2C_ISR_STOPF;
        }
        return STM32N6_I2C_ISR_TC;
    }
    return mock_find(address)->value;
}

void tiku_i2c_host_write(uintptr_t address, uint32_t value)
{
    mock_find(address)->value = value;
#if TIKU_BOARD_I2C_INSTANCE == 1U
    const uintptr_t i2c_base = STM32N6_I2C1_BASE;
#else
    const uintptr_t i2c_base = STM32N6_I2C2_BASE;
#endif
    if (address == STM32N6_I2C_CR2(i2c_base)) {
        current_nbytes = (value & STM32N6_I2C_CR2_NBYTES_MSK) >>
                         STM32N6_I2C_CR2_NBYTES_POS;
        current_read = (value & STM32N6_I2C_CR2_RD_WRN) != 0U;
        current_reload = (value & STM32N6_I2C_CR2_RELOAD) != 0U;
        current_autoend = (value & STM32N6_I2C_CR2_AUTOEND) != 0U;
        phase_tx = 0U;
        phase_rx = 0U;
        stop_reported = 0U;
        ++phase_count;
        if ((value & STM32N6_I2C_CR2_START) != 0U) ++starts;
    } else if (address == STM32N6_I2C_TXDR(i2c_base)) {
        ++phase_tx;
        ++total_tx;
    }
}

void tiku_i2c_host_gpio_alt_od(uint8_t port, uint8_t pin, uint8_t af)
{
    assert(af == TIKU_BOARD_I2C_AF);
    if (gpio_count == 0U) {
        assert(port == TIKU_BOARD_I2C_SCL_PORT);
        assert(pin == TIKU_BOARD_I2C_SCL_PIN);
    } else {
        assert(port == TIKU_BOARD_I2C_SDA_PORT);
        assert(pin == TIKU_BOARD_I2C_SDA_PIN);
    }
    ++gpio_count;
}

int8_t tiku_gpio_arch_set_input(uint8_t port, uint8_t pin)
{
    (void)port;
    (void)pin;
    ++input_count;
    return 0;
}

static void mock_reset(void)
{
    mock_reg_count = 0U;
    fail_mode = MOCK_FAIL_NONE;
    starts = 0U;
    stops = 0U;
    phase_count = 0U;
    phase_tx = 0U;
    phase_rx = 0U;
    total_tx = 0U;
    current_nbytes = 0U;
    current_read = 0U;
    current_reload = 0U;
    current_autoend = 0U;
    stop_reported = 0U;
    gpio_count = 0U;
    input_count = 0U;
}

static void test_init_and_mapping(void)
{
    tiku_i2c_config_t config = { TIKU_I2C_SPEED_STANDARD };
    uint32_t expected_sel = STM32N6_RCC_I2C_SEL_HSI <<
#if TIKU_BOARD_I2C_INSTANCE == 1U
                            0U;
    assert((mock_find(STM32N6_RCC_CCIPR4)->value &
            STM32N6_RCC_CCIPR4_I2C1SEL) == expected_sel);
#else
                            4U;
    assert((mock_find(STM32N6_RCC_CCIPR4)->value &
            STM32N6_RCC_CCIPR4_I2C2SEL) == expected_sel);
#endif
    mock_reset();
    assert(tiku_i2c_arch_init(&config) == TIKU_I2C_OK);
    assert(gpio_count == 2U);
    standard_timing = mock_find(STM32N6_I2C_TIMINGR(
#if TIKU_BOARD_I2C_INSTANCE == 1U
                     STM32N6_I2C1_BASE
#else
                     STM32N6_I2C2_BASE
#endif
                     ))->value;
    assert(standard_timing != 0U);
    assert((mock_find(STM32N6_RCC_CCIPR4)->value &
#if TIKU_BOARD_I2C_INSTANCE == 1U
            STM32N6_RCC_CCIPR4_I2C1SEL) == expected_sel);
#else
            STM32N6_RCC_CCIPR4_I2C2SEL) == expected_sel);
#endif
}

static void test_write_read_probe_and_reload(void)
{
    uint8_t tx[] = { 0x10U, 0x20U, 0x30U };
    uint8_t rx[2] = { 0U, 0U };
    uint8_t long_tx[300];
    size_t i;

    assert(tiku_i2c_arch_write(0x50U, tx, sizeof(tx)) == TIKU_I2C_OK);
    assert(total_tx == sizeof(tx));
    assert(starts == 1U && stops == 1U);

    starts = stops = phase_count = 0U;
    stop_reported = 0U;
    assert(tiku_i2c_arch_read(0x50U, rx, sizeof(rx)) == TIKU_I2C_OK);
    assert(starts == 1U && stops == 1U);

    starts = stops = phase_count = 0U;
    stop_reported = 0U;
    assert(tiku_i2c_arch_write_read(0x50U, tx, sizeof(tx), rx,
                                    sizeof(rx)) == TIKU_I2C_OK);
    assert(starts == 2U && stops == 1U);
    assert(rx[0] == 0xA0U && rx[1] == 0xA1U);

    starts = stops = phase_count = 0U;
    stop_reported = 0U;
    assert(tiku_i2c_arch_probe(0x50U) == TIKU_I2C_OK);
    assert(starts == 1U && stops == 1U);

    for (i = 0U; i < sizeof(long_tx); ++i) long_tx[i] = (uint8_t)i;
    starts = stops = phase_count = 0U;
    stop_reported = 0U;
    total_tx = 0U;
    assert(tiku_i2c_arch_write(0x50U, long_tx, sizeof(long_tx)) ==
           TIKU_I2C_OK);
    assert(total_tx == sizeof(long_tx));
    assert(starts == 1U && stops == 1U && phase_count == 2U);
}

static void test_errors_and_parameters(void)
{
    uint8_t byte = 0xA5U;
    assert(tiku_i2c_arch_write(0x80U, &byte, 1U) == TIKU_I2C_ERR_PARAM);
    assert(tiku_i2c_arch_write(0x50U, NULL, 1U) == TIKU_I2C_ERR_PARAM);
    assert(tiku_i2c_arch_write(0x50U, &byte, 0U) == TIKU_I2C_ERR_PARAM);

    fail_mode = MOCK_FAIL_BUSY;
    assert(tiku_i2c_arch_write(0x50U, &byte, 1U) == TIKU_I2C_ERR_BUSY);
    starts = 0U;
    fail_mode = MOCK_FAIL_NACK;
    assert(tiku_i2c_arch_write(0x50U, &byte, 1U) == TIKU_I2C_ERR_NACK);
    starts = 0U;
    fail_mode = MOCK_FAIL_TIMEOUT;
    assert(tiku_i2c_arch_write(0x50U, &byte, 1U) == TIKU_I2C_ERR_TIMEOUT);
    fail_mode = MOCK_FAIL_NONE;
    assert(tiku_i2c_arch_write(0x50U, &byte, 1U) == TIKU_I2C_OK);
}

static void test_fast_timing(void)
{
    tiku_i2c_config_t config = { TIKU_I2C_SPEED_FAST };
    uint32_t fast_timing;
    tiku_i2c_arch_close();
    assert(tiku_i2c_arch_init(&config) == TIKU_I2C_OK);
    fast_timing = mock_find(STM32N6_I2C_TIMINGR(
#if TIKU_BOARD_I2C_INSTANCE == 1U
                            STM32N6_I2C1_BASE
#else
                            STM32N6_I2C2_BASE
#endif
                            ))->value;
    assert(fast_timing != 0U && fast_timing != standard_timing);
}

int main(void)
{
    tiku_i2c_config_t config = { TIKU_I2C_SPEED_FAST };
    mock_reset();
    assert(tiku_i2c_arch_write(0x50U, (const uint8_t *)"x", 1U) ==
           TIKU_I2C_ERR_PARAM);
    assert(tiku_i2c_arch_init(NULL) == TIKU_I2C_ERR_PARAM);
    config.speed = 9U;
    assert(tiku_i2c_arch_init(&config) == TIKU_I2C_ERR_PARAM);
    test_init_and_mapping();
    test_write_read_probe_and_reload();
    test_errors_and_parameters();
    test_fast_timing();
    tiku_i2c_arch_close();
    assert(input_count == 4U);
    puts("stm32n6 I2C host tests passed");
    return 0;
}
