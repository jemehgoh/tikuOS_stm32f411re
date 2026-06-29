/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_gpio_arch.h - STM32F411RE GPIO data access
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32F411_GPIO_ARCH_H_
#define TIKU_STM32F411_GPIO_ARCH_H_

#include <stdint.h>

/*---------------------------------------------------------------------------*/
/* Virtual GPIO mapping helpers                                              */
/*---------------------------------------------------------------------------*/

/**
 * Translate a Tiku virtual GPIO port/pin pair into a native STM32 bank/pin.
 *
 * The STM32 shell/VFS GPIO model exposes the first four 16-bit banks as eight
 * virtual 8-pin ports:
 *   P1/P2 -> PA0..PA15
 *   P3/P4 -> PB0..PB15
 *   P5/P6 -> PC0..PC15
 *   P7/P8 -> PD0..PD15
 *
 * @param port      Tiku virtual port number (1..8)
 * @param pin       Pin within the virtual port (0..7)
 * @param phys_port Output native STM32 port index (A=1, B=2, ...)
 * @param phys_pin  Output native STM32 pin number (0..15)
 *
 * @return 0 on success, non-zero on invalid inputs.
 */
static inline int
tiku_stm32f411_gpio_virtual_resolve(uint8_t port, uint8_t pin,
                                    uint8_t *phys_port, uint8_t *phys_pin)
{
    if (pin > 7U || phys_port == (uint8_t *)0 || phys_pin == (uint8_t *)0) {
        return -1;
    }

    switch (port) {
    case 1U:
    case 3U:
    case 5U:
    case 7U:
        *phys_port = (uint8_t)(((port - 1U) / 2U) + 1U);
        *phys_pin = pin;
        return 0;
    case 2U:
    case 4U:
    case 6U:
    case 8U:
        *phys_port = (uint8_t)(((port - 2U) / 2U) + 1U);
        *phys_pin = (uint8_t)(pin + 8U);
        return 0;
    default:
        return -1;
    }
}

/*---------------------------------------------------------------------------*/
/* Board-facing GPIO write helpers                                           */
/*---------------------------------------------------------------------------*/

void tiku_stm32f411_gpio_set(uint8_t port, uint8_t pin, uint8_t value);
void tiku_stm32f411_gpio_toggle(uint8_t port, uint8_t pin);

/*---------------------------------------------------------------------------*/
/* Platform-agnostic GPIO HAL entry points                                   */
/*---------------------------------------------------------------------------*/

int8_t tiku_gpio_arch_set_output(uint8_t port, uint8_t pin);
int8_t tiku_gpio_arch_set_input(uint8_t port, uint8_t pin);
int8_t tiku_gpio_arch_write(uint8_t port, uint8_t pin, uint8_t val);
int8_t tiku_gpio_arch_toggle(uint8_t port, uint8_t pin);
int8_t tiku_gpio_arch_read(uint8_t port, uint8_t pin);
int8_t tiku_gpio_arch_get_dir(uint8_t port, uint8_t pin);

#endif /* TIKU_STM32F411_GPIO_ARCH_H_ */
