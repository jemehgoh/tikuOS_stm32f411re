/* Host-side checks for the STM32N6570-DK IMX335 setup path. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <interfaces/bus/tiku_i2c_bus.h>

#include "../applications/tiku_stm32n6570_imx335.h"

typedef struct {
    uint16_t reg;
    uint8_t value;
} write_t;

static write_t writes[128];
static size_t write_count;
static size_t fail_write_at = (size_t)-1;
static int read_result;
static uint8_t sensor_id;
static uint32_t delay_count;

int tiku_i2c_write(uint8_t addr, const uint8_t *buf, uint16_t len)
{
    assert(addr == 0x1AU);
    assert(buf != NULL && len == 3U);
    if (write_count == fail_write_at) {
        return TIKU_I2C_ERR_NACK;
    }
    assert(write_count < sizeof(writes) / sizeof(writes[0]));
    writes[write_count].reg = (uint16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
    writes[write_count].value = buf[2];
    ++write_count;
    return TIKU_I2C_OK;
}

int tiku_i2c_write_read(uint8_t addr, const uint8_t *tx_buf,
                        uint16_t tx_len, uint8_t *rx_buf, uint16_t rx_len)
{
    assert(addr == 0x1AU);
    assert(tx_len == 2U && tx_buf[0] == 0x39U && tx_buf[1] == 0x12U);
    assert(rx_len == 1U && rx_buf != NULL);
    if (read_result != TIKU_I2C_OK) {
        return read_result;
    }
    rx_buf[0] = sensor_id;
    return TIKU_I2C_OK;
}

void tiku_cpu_stm32n6_delay_ms(unsigned int ms)
{
    assert(ms == 20U);
    ++delay_count;
}

static void reset_mock(void)
{
    write_count = 0U;
    fail_write_at = (size_t)-1;
    read_result = TIKU_I2C_OK;
    sensor_id = 0U;
    delay_count = 0U;
}

static void test_profile_and_stream_order(void)
{
    reset_mock();
    assert(tiku_stm32n6570_imx335_start() == TIKU_IMX335_ERR_STATE);
    assert(tiku_stm32n6570_imx335_init() == TIKU_IMX335_OK);
    assert(write_count > 5U);
    assert(writes[0].reg == 0x3000U && writes[0].value == 0x01U);
    assert(writes[write_count - 5U].reg == 0x3050U &&
           writes[write_count - 5U].value == 0x00U);
    assert(writes[write_count - 1U].reg == 0x3A01U &&
           writes[write_count - 1U].value == 0x01U);

    assert(tiku_stm32n6570_imx335_start() == TIKU_IMX335_OK);
    assert(writes[write_count - 1U].reg == 0x3000U &&
           writes[write_count - 1U].value == 0x00U);
    assert(delay_count == 1U);
    assert(tiku_stm32n6570_imx335_stop() == TIKU_IMX335_OK);
    assert(writes[write_count - 1U].reg == 0x3000U &&
           writes[write_count - 1U].value == 0x01U);
    assert(tiku_stm32n6570_imx335_stop() == TIKU_IMX335_ERR_STATE);
}

static void test_wrong_sensor_and_read_failure(void)
{
    reset_mock();
    sensor_id = 0x5AU;
    assert(tiku_stm32n6570_imx335_init() == TIKU_IMX335_ERR_ID);
    assert(write_count == 0U);

    reset_mock();
    read_result = TIKU_I2C_ERR_NACK;
    assert(tiku_stm32n6570_imx335_init() == TIKU_IMX335_ERR_BUS);
    assert(write_count == 0U);
}

static void test_mode_write_failure(void)
{
    reset_mock();
    fail_write_at = 4U;
    assert(tiku_stm32n6570_imx335_init() == TIKU_IMX335_ERR_BUS);
    assert(write_count == 4U);
    assert(tiku_stm32n6570_imx335_start() == TIKU_IMX335_ERR_STATE);
}

int main(void)
{
    test_profile_and_stream_order();
    test_wrong_sensor_and_read_failure();
    test_mode_write_failure();
    puts("STM32N6570-DK IMX335 host tests: OK");
    return 0;
}
