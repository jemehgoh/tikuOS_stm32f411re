/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_pinmux_arch.c - STM32F411RE pinmux and pad helpers
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_pinmux_arch.h"
#include <stm32f411xe.h>
#include <stdint.h>

static GPIO_TypeDef *stm32f411_gpio_from_base(uint32_t gpio_base)
{
    return (GPIO_TypeDef *)(uintptr_t)gpio_base;
}

static void stm32f411_rcc_enable_gpio(uint32_t rcc_bit)
{
    RCC->AHB1ENR |= rcc_bit;
    (void)RCC->AHB1ENR;
}

int
tiku_stm32f411_pinmux_resolve(uint8_t port, uint8_t pin,
                              uint32_t *gpio_base, uint32_t *rcc_bit)
{
    if (pin > 15U || gpio_base == (uint32_t *)0 || rcc_bit == (uint32_t *)0) {
        return -1;
    }

    switch (port) {
    case 1U:
        *gpio_base = GPIOA_BASE;
        *rcc_bit   = RCC_AHB1ENR_GPIOAEN;
        return 0;
    case 2U:
        *gpio_base = GPIOB_BASE;
        *rcc_bit   = RCC_AHB1ENR_GPIOBEN;
        return 0;
    case 3U:
        *gpio_base = GPIOC_BASE;
        *rcc_bit   = RCC_AHB1ENR_GPIOCEN;
        return 0;
    case 4U:
        *gpio_base = GPIOD_BASE;
        *rcc_bit   = RCC_AHB1ENR_GPIODEN;
        return 0;
    default:
        return -1;
    }
}

int
tiku_stm32f411_pinmux_config(uint8_t port, uint8_t pin,
                             uint32_t mode, uint32_t pupd, uint32_t speed)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    GPIO_TypeDef *gpio;
    uint32_t moder;
    uint32_t ospeedr;
    uint32_t pull;

    if (tiku_stm32f411_pinmux_resolve(port, pin, &gpio_base, &rcc_bit) != 0) {
        return -1;
    }

    stm32f411_rcc_enable_gpio(rcc_bit);
    gpio = stm32f411_gpio_from_base(gpio_base);

    moder = gpio->MODER;
    moder &= ~(0x3UL << (pin * 2U));
    moder |= ((mode & 0x3UL) << (pin * 2U));
    gpio->MODER = moder;

    ospeedr = gpio->OSPEEDR;
    ospeedr &= ~(0x3UL << (pin * 2U));
    ospeedr |= ((speed & 0x3UL) << (pin * 2U));
    gpio->OSPEEDR = ospeedr;

    pull = gpio->PUPDR;
    pull &= ~(0x3UL << (pin * 2U));
    pull |= ((pupd & 0x3UL) << (pin * 2U));
    gpio->PUPDR = pull;

    return 0;
}

int
tiku_stm32f411_pinmux_set_drive(uint8_t port, uint8_t pin, uint8_t open_drain)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    GPIO_TypeDef *gpio;
    uint32_t otyper;

    if (tiku_stm32f411_pinmux_resolve(port, pin, &gpio_base, &rcc_bit) != 0) {
        return -1;
    }

    stm32f411_rcc_enable_gpio(rcc_bit);
    gpio = stm32f411_gpio_from_base(gpio_base);

    otyper = gpio->OTYPER;
    if (open_drain != 0U) {
        otyper |= (1UL << pin);
    } else {
        otyper &= ~(1UL << pin);
    }
    gpio->OTYPER = otyper;

    return 0;
}

int
tiku_stm32f411_pinmux_set_af(uint8_t port, uint8_t pin, uint8_t af)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    GPIO_TypeDef *gpio;
    uint32_t shift;
    uint32_t index;
    uint32_t afr;

    if (tiku_stm32f411_pinmux_resolve(port, pin, &gpio_base, &rcc_bit) != 0) {
        return -1;
    }

    stm32f411_rcc_enable_gpio(rcc_bit);
    gpio = stm32f411_gpio_from_base(gpio_base);
    index = (uint32_t)pin >> 3U;
    shift = ((uint32_t)pin & 0x7UL) * 4U;
    afr = gpio->AFR[index];
    afr &= ~(0xFUL << shift);
    afr |= ((uint32_t)af & 0xFUL) << shift;
    gpio->AFR[index] = afr;

    return 0;
}

int
tiku_stm32f411_pinmux_init_output(uint8_t port, uint8_t pin)
{
    return tiku_stm32f411_pinmux_config(port, pin,
                                        1U,
                                        0U,
                                        3U)
        || tiku_stm32f411_pinmux_set_drive(port, pin, 0U);
}

int
tiku_stm32f411_pinmux_init_input(uint8_t port, uint8_t pin, uint32_t pupd)
{
    return tiku_stm32f411_pinmux_config(port, pin,
                                        0U,
                                        pupd,
                                        3U);
}
