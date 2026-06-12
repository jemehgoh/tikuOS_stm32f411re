/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_adc_arch.c - ADC driver for STM32F411RE ADC1
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_adc_arch.h"
#include "tiku_pinmux_arch.h"
#include <stm32f411xe.h>
#include "tiku.h"

#ifdef TIKU_BOARD_ADC_AVAILABLE  /* Board supports ADC1 */

/*---------------------------------------------------------------------------*/
/* CONSTANTS                                                                 */
/*---------------------------------------------------------------------------*/

/** Busy-wait loop iteration limit to prevent infinite hangs. */
#define ADC_TIMEOUT                   10000U
#define STM32F411_ADC_CHANNEL_COUNT   16U
#define TIKU_STM32_GPIO_MODE_ANALOG   3U
#define TIKU_STM32_GPIO_PUPD_NONE     0U
#define TIKU_STM32_GPIO_SPEED_HIGH    3U
#define TIKU_STM32_ADC_SAMPLE_84      4U
#define TIKU_STM32_ADC_SAMPLE_480     7U
#define TIKU_STM32_ADC_ADCPRE_DIV6    ADC_CCR_ADCPRE_1

/*---------------------------------------------------------------------------*/
/* PRIVATE TYPES                                                             */
/*---------------------------------------------------------------------------*/

struct stm32f411_adc_pin {
    uint8_t port;
    uint8_t pin;
};

/*---------------------------------------------------------------------------*/
/* PRIVATE STATE                                                             */
/*---------------------------------------------------------------------------*/

/** Saved reference configuration for use during reads. */
static uint8_t adc_reference;

// GPIO pin mappings for ADC channels 0-15
static const struct stm32f411_adc_pin g_adc_channel_pins[] = {
    {1U, 0U}, {1U, 1U}, {1U, 2U}, {1U, 3U},
    {1U, 4U}, {1U, 5U}, {1U, 6U}, {1U, 7U},
    {2U, 0U}, {2U, 1U}, {3U, 0U}, {3U, 1U},
    {3U, 2U}, {3U, 3U}, {3U, 4U}, {3U, 5U}
};

static tiku_adc_config_t g_adc_cfg;
static uint8_t g_adc_ready;

/*---------------------------------------------------------------------------*/
/* PRIVATE HELPERS                                                           */
/*---------------------------------------------------------------------------*/

static int
adc_resolution_bits(uint8_t resolution, uint32_t *bits)
{
    if (bits == (uint32_t *)0) {
        return TIKU_ADC_ERR_PARAM;
    }

    switch (resolution) {
    case TIKU_ADC_RES_8BIT:
        *bits = ADC_CR1_RES_1;
        return TIKU_ADC_OK;
    case TIKU_ADC_RES_10BIT:
        *bits = ADC_CR1_RES_0;
        return TIKU_ADC_OK;
    case TIKU_ADC_RES_12BIT:
        *bits = 0U;
        return TIKU_ADC_OK;
    default:
        return TIKU_ADC_ERR_PARAM;
    }
}

static uint8_t
adc_external_channel_valid(uint8_t channel)
{
    return channel < STM32F411_ADC_CHANNEL_COUNT
        && g_adc_channel_pins[channel].port != 0U;
}

static uint8_t
adc_hw_channel(uint8_t channel)
{
    if (channel == TIKU_ADC_CH_TEMP) {
        return 16U;
    }
    if (channel == TIKU_ADC_CH_BATTERY) {
        return 18U;
    }

    return channel;
}

static void
adc_set_sample_time(uint8_t hw_channel, uint32_t sample_bits)
{
    uint32_t reg;
    uint32_t shift;

    shift = 3U * ((uint32_t)hw_channel % 10U);
    if (hw_channel < 10U) {
        reg = ADC1->SMPR2;
        reg &= ~(0x07U << shift);
        reg |= (sample_bits & 0x07U) << shift;
        ADC1->SMPR2 = reg;
    } else {
        reg = ADC1->SMPR1;
        reg &= ~(0x07U << shift);
        reg |= (sample_bits & 0x07U) << shift;
        ADC1->SMPR1 = reg;
    }
}

static void
adc_select_channel(uint8_t hw_channel)
{
    ADC1->SQR1 &= ~ADC_SQR1_L_Msk;
    ADC1->SQR2 = 0U;
    ADC1->SQR3 = (uint32_t)hw_channel & ADC_SQR3_SQ1_Msk;
}

static void
adc_disable_special_channels(void)
{
    ADC->CCR &= ~(ADC_CCR_TSVREFE | ADC_CCR_VBATE);
}

static void
adc_enable_temp_channel(void)
{
    adc_disable_special_channels();
    ADC->CCR |= ADC_CCR_TSVREFE;
}

static void
adc_enable_battery_channel(void)
{
    adc_disable_special_channels();
    ADC->CCR |= ADC_CCR_VBATE;
}

static void
adc_reset_pin_if_analog(uint8_t channel)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    GPIO_TypeDef *gpio;
    uint32_t moder;
    uint8_t port;
    uint8_t pin;

    if (!adc_external_channel_valid(channel)) {
        return;
    }

    port = g_adc_channel_pins[channel].port;
    pin  = g_adc_channel_pins[channel].pin;
    if (tiku_stm32f411_pinmux_resolve(port, pin, &gpio_base, &rcc_bit) != 0) {
        return;
    }

    RCC->AHB1ENR |= rcc_bit;
    (void)RCC->AHB1ENR;
    gpio = (GPIO_TypeDef *)(uintptr_t)gpio_base;

    moder = gpio->MODER;
    if (((moder >> (2U * (uint32_t)pin)) & 0x03U)
        == TIKU_STM32_GPIO_MODE_ANALOG) {
        (void)tiku_stm32f411_pinmux_init_input(port, pin,
                                               TIKU_STM32_GPIO_PUPD_NONE);
    }
}

/*---------------------------------------------------------------------------*/
/* PUBLIC FUNCTIONS                                                          */
/*---------------------------------------------------------------------------*/

int
tiku_adc_arch_init(const tiku_adc_config_t *config)
{
    uint32_t cr1;
    uint32_t cr2;
    uint32_t ccr;
    uint32_t resolution_bits;
    int status;

    if (config == (const tiku_adc_config_t *)0) {
        return TIKU_ADC_ERR_PARAM;
    }
    if (config->reference != TIKU_ADC_REF_AVCC) {
        // Only one reference option (AVCC) supported on the STM32F411xE
        return TIKU_ADC_ERR_PARAM;
    }

    status = adc_resolution_bits(config->resolution, &resolution_bits);
    if (status != TIKU_ADC_OK) {
        return status;
    }

    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;

    /*
    * CCR register default configuration:
    *   - Analog clock frequency: PCLK2 divided by 6
    *   - VBATE and TSVREFE (battery and temp sensor channels) disabled
    */
    ccr = ADC->CCR;
    ccr &= ~ADC_CCR_ADCPRE_Msk;
    ccr |= TIKU_STM32_ADC_ADCPRE_DIV6;
    ADC->CCR = ccr;
    adc_disable_special_channels();

    /*
    * CR1 register default configuration:
    * - Scan mode disabled (SCAN=0)
    * - Resolution: set according to config
    */
    cr1 = ADC1->CR1;
    cr1 &= ~(ADC_CR1_SCAN | ADC_CR1_RES_Msk);
    cr1 |= resolution_bits;
    ADC1->CR1 = cr1;

    /*
    * CR2 register default configuration:
    *  - Single conversion mode (CONT=0)
    *  - No hardware trigger (EXTEN=00)
    *  - Data right-aligned (ALIGN=0)
    *  - EOC flag set at end of each conversion (EOCS=1)
    *  - DMA disabled (DMA=0)
    */
    cr2 = ADC1->CR2;
    cr2 &= ~(ADC_CR2_CONT
          | ADC_CR2_DMA
          | ADC_CR2_DDS
          | ADC_CR2_ALIGN
          | ADC_CR2_SWSTART);
    cr2 |= ADC_CR2_EOCS;
    ADC1->CR2 = cr2;

    // Reset conversion sequences upon startup
    ADC1->SQR1 &= ~ADC_SQR1_L_Msk;
    ADC1->SQR2 = 0U;
    ADC1->SQR3 = 0U;

    // Start ADC
    ADC1->CR2 |= ADC_CR2_ADON;

    adc_reference = config->reference;
    g_adc_cfg = *config;
    g_adc_ready = 1U;

    return TIKU_ADC_OK;
}

void
tiku_adc_arch_close(void)
{
    uint8_t channel;

    // Disable ADC
    ADC1->CR2 &= ~ADC_CR2_ADON;

    // Disable battery monitor and temperature sensor channels
    adc_disable_special_channels();
    RCC->APB2ENR &= ~RCC_APB2ENR_ADC1EN;

    // Reset any external channel pins that are in analog mode
    // This assumes that the ADC driver is the only user of those pins, which is true for the current board configuration
    for (channel = 0U; channel < STM32F411_ADC_CHANNEL_COUNT; ++channel) {
        adc_reset_pin_if_analog(channel);
    }

    adc_reference = TIKU_ADC_REF_AVCC;
    g_adc_ready = 0U;
}

int
tiku_adc_arch_channel_init(uint8_t channel)
{
    uint8_t hw_channel;

    if (g_adc_ready == 0U) {
        return TIKU_ADC_ERR_TIMEOUT;
    }

    if (channel == TIKU_ADC_CH_TEMP) {
        /* Enable the internal temperature sensor path on ADC channel 16. */
        adc_enable_temp_channel();
        hw_channel = adc_hw_channel(channel);
        adc_select_channel(hw_channel);
        adc_set_sample_time(hw_channel, TIKU_STM32_ADC_SAMPLE_480);
        return TIKU_ADC_OK;
    }

    if (channel == TIKU_ADC_CH_BATTERY) {
        /* Enable the internal battery monitor path on ADC channel 18. */
        adc_enable_battery_channel();
        hw_channel = adc_hw_channel(channel);
        adc_select_channel(hw_channel);
        adc_set_sample_time(hw_channel, TIKU_STM32_ADC_SAMPLE_480);
        return TIKU_ADC_OK;
    }

    if (!adc_external_channel_valid(channel)) {
        return TIKU_ADC_ERR_PARAM;
    }

    adc_disable_special_channels();

    if (tiku_stm32f411_pinmux_config(g_adc_channel_pins[channel].port,
                                     g_adc_channel_pins[channel].pin,
                                     TIKU_STM32_GPIO_MODE_ANALOG,
                                     TIKU_STM32_GPIO_PUPD_NONE,
                                     TIKU_STM32_GPIO_SPEED_HIGH) != 0) {
        return TIKU_ADC_ERR_PARAM;
    }

    hw_channel = adc_hw_channel(channel);
    adc_select_channel(hw_channel);
    adc_set_sample_time(hw_channel, TIKU_STM32_ADC_SAMPLE_84);

    return TIKU_ADC_OK;
}

int
tiku_adc_arch_read(uint8_t channel, uint16_t *value)
{
    uint32_t cr2;
    uint8_t hw_channel;
    uint32_t timeout;

    if (value == (uint16_t *)0) {
        return TIKU_ADC_ERR_PARAM;
    }

    if (channel == TIKU_ADC_CH_TEMP) {
        adc_enable_temp_channel();
    } else if (channel == TIKU_ADC_CH_BATTERY) {
        adc_enable_battery_channel();
    } else if (adc_external_channel_valid(channel)) {
        adc_disable_special_channels();
    } else {
        return TIKU_ADC_ERR_PARAM;
    }

    hw_channel = adc_hw_channel(channel);
    adc_select_channel(hw_channel);

    if (channel >= TIKU_ADC_CH_TEMP) {
        adc_set_sample_time(hw_channel, TIKU_STM32_ADC_SAMPLE_480);
    } else {
        adc_set_sample_time(hw_channel, TIKU_STM32_ADC_SAMPLE_84);
    }

    ADC1->SR = 0U;

    cr2 = ADC1->CR2;
    cr2 |= ADC_CR2_ADON;
    ADC1->CR2 = cr2;
    ADC1->CR2 = cr2 | ADC_CR2_SWSTART;

    timeout = ADC_TIMEOUT;
    while ((ADC1->SR & ADC_SR_EOC) == 0U) {
        if (--timeout == 0U) {
            return TIKU_ADC_ERR_TIMEOUT;
        }
    }

    *value = (uint16_t)ADC1->DR;
    (void)adc_reference;
    (void)g_adc_cfg;
    return TIKU_ADC_OK;
}

#endif /* TIKU_BOARD_ADC_AVAILABLE */
