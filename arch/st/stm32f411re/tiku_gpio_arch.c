/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_gpio_arch.c - STM32F411RE GPIO backend
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_gpio_arch.h"
#include "tiku_pinmux_arch.h"
#include <stm32f411xe.h>
#include <stdint.h>

#define TIKU_STM32_GPIO_MODE_OUTPUT 0x1U
#define TIKU_STM32_GPIO_PUPD_UP     0x1U

/*---------------------------------------------------------------------------*/
/* Per-pin direct helpers                                                    */
/*---------------------------------------------------------------------------*/

static GPIO_TypeDef *stm32f411_gpio_from_base(uint32_t gpio_base)
{
    return (GPIO_TypeDef *)(uintptr_t)gpio_base;
}

static void stm32f411_gpio_enable_clock(uint32_t rcc_bit)
{
    RCC->AHB1ENR |= rcc_bit;
    (void)RCC->AHB1ENR;
}

static uint32_t stm32f411_gpio_is_output(uint32_t gpio_base, uint8_t pin)
{
    GPIO_TypeDef *gpio = stm32f411_gpio_from_base(gpio_base);
    uint32_t moder = gpio->MODER;
    return (((moder >> (pin * 2U)) & 0x3U) == TIKU_STM32_GPIO_MODE_OUTPUT);
}

static void stm32f411_gpio_set_raw(uint32_t gpio_base, uint8_t pin,
                                   uint8_t value)
{
    GPIO_TypeDef *gpio = stm32f411_gpio_from_base(gpio_base);
    gpio->BSRR = value ? (1UL << pin) : (1UL << (pin + 16U));
}

static void stm32f411_gpio_toggle_raw(uint32_t gpio_base, uint8_t pin)
{
    GPIO_TypeDef *gpio = stm32f411_gpio_from_base(gpio_base);
    uint32_t odr = gpio->ODR;
    gpio->BSRR = (odr & (1UL << pin))
        ? (1UL << (pin + 16U))
        : (1UL << pin);
}

void tiku_stm32f411_gpio_set(uint8_t port, uint8_t pin, uint8_t value)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;

    if (tiku_stm32f411_pinmux_resolve(port, pin, &gpio_base, &rcc_bit) != 0) {
        return;
    }
    stm32f411_gpio_enable_clock(rcc_bit);
    if (!stm32f411_gpio_is_output(gpio_base, pin)) {
        if (tiku_stm32f411_pinmux_init_output(port, pin) != 0) {
            return;
        }
    }
    stm32f411_gpio_set_raw(gpio_base, pin, value);
}

void tiku_stm32f411_gpio_toggle(uint8_t port, uint8_t pin)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;

    if (tiku_stm32f411_pinmux_resolve(port, pin, &gpio_base, &rcc_bit) != 0) {
        return;
    }
    stm32f411_gpio_enable_clock(rcc_bit);
    if (!stm32f411_gpio_is_output(gpio_base, pin)) {
        if (tiku_stm32f411_pinmux_init_output(port, pin) != 0) {
            return;
        }
    }
    stm32f411_gpio_toggle_raw(gpio_base, pin);
}

/*---------------------------------------------------------------------------*/
/* HAL entry points                                                          */
/*---------------------------------------------------------------------------*/

int8_t tiku_gpio_arch_set_output(uint8_t port, uint8_t pin)
{
    uint8_t phys_port;
    uint8_t phys_pin;

    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return -1;
    }
    return tiku_stm32f411_pinmux_init_output(phys_port, phys_pin);
}

int8_t tiku_gpio_arch_set_input(uint8_t port, uint8_t pin)
{
    uint8_t phys_port;
    uint8_t phys_pin;

    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return -1;
    }
    return tiku_stm32f411_pinmux_init_input(phys_port, phys_pin,
                                            TIKU_STM32_GPIO_PUPD_UP);
}

int8_t tiku_gpio_arch_write(uint8_t port, uint8_t pin, uint8_t val)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    uint8_t phys_port;
    uint8_t phys_pin;

    // Get actual port mappings (from virtual ports) and resolve that to GPIO base and RCC bit
    // The mappings to the actual ports are needed to support other peripherals (which use these 
    // mappings for configuration)
    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return -1;
    }
    if (tiku_stm32f411_pinmux_resolve(phys_port, phys_pin,
                                      &gpio_base, &rcc_bit) != 0) {
        return -1;
    }
    stm32f411_gpio_enable_clock(rcc_bit);
    if (!stm32f411_gpio_is_output(gpio_base, phys_pin)) {
        if (tiku_stm32f411_pinmux_init_output(phys_port, phys_pin) != 0) {
            return -1;
        }
    }
    stm32f411_gpio_set_raw(gpio_base, phys_pin, val);
    return 0;
}

int8_t tiku_gpio_arch_toggle(uint8_t port, uint8_t pin)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    uint8_t phys_port;
    uint8_t phys_pin;

    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return -1;
    }
    if (tiku_stm32f411_pinmux_resolve(phys_port, phys_pin,
                                      &gpio_base, &rcc_bit) != 0) {
        return -1;
    }
    stm32f411_gpio_enable_clock(rcc_bit);
    if (!stm32f411_gpio_is_output(gpio_base, phys_pin)) {
        if (tiku_stm32f411_pinmux_init_output(phys_port, phys_pin) != 0) {
            return -1;
        }
    }
    stm32f411_gpio_toggle_raw(gpio_base, phys_pin);
    return 0;
}

int8_t tiku_gpio_arch_read(uint8_t port, uint8_t pin)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    uint8_t phys_port;
    uint8_t phys_pin;

    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return -1;
    }
    if (tiku_stm32f411_pinmux_resolve(phys_port, phys_pin,
                                      &gpio_base, &rcc_bit) != 0) {
        return -1;
    }
    stm32f411_gpio_enable_clock(rcc_bit);
    return (stm32f411_gpio_from_base(gpio_base)->IDR & (1UL << phys_pin))
        ? 1 : 0;
}

int8_t tiku_gpio_arch_get_dir(uint8_t port, uint8_t pin)
{
    uint32_t gpio_base;
    uint32_t rcc_bit;
    uint32_t moder;
    uint8_t phys_port;
    uint8_t phys_pin;

    if (tiku_stm32f411_gpio_virtual_resolve(port, pin,
                                            &phys_port, &phys_pin) != 0) {
        return -1;
    }
    if (tiku_stm32f411_pinmux_resolve(phys_port, phys_pin,
                                      &gpio_base, &rcc_bit) != 0) {
        return -1;
    }
    stm32f411_gpio_enable_clock(rcc_bit);
    moder = stm32f411_gpio_from_base(gpio_base)->MODER;
    return (((moder >> (phys_pin * 2U)) & 0x3U) == TIKU_STM32_GPIO_MODE_OUTPUT)
        ? 1 : 0;
}
