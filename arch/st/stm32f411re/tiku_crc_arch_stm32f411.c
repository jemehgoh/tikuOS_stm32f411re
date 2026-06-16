/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_crc_arch_stm32f411.c - STM32F411 hardware CRC helper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_crc_arch_stm32f411.h"
#include <stm32f411xe.h>

void tiku_stm32f411_crc_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
    (void)RCC->AHB1ENR;
}

void tiku_stm32f411_crc_reset(void)
{
    tiku_stm32f411_crc_init();
    CRC->CR = CRC_CR_RESET;
}

void tiku_stm32f411_crc_feed_word(uint32_t word)
{
    CRC->DR = word;
}

uint32_t tiku_stm32f411_crc_value(void)
{
    return CRC->DR;
}

uint32_t tiku_stm32f411_crc32_payload(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (data == (const uint8_t *)0) {
        return 0U;
    }

    tiku_stm32f411_crc_reset();

    for (i = 0U; i < len; i += 4U) {
        uint32_t word = 0U;
        uint32_t rem = len - i;

        word |= (uint32_t)data[i];
        if (rem > 1U) {
            word |= (uint32_t)data[i + 1U] << 8;
        }
        if (rem > 2U) {
            word |= (uint32_t)data[i + 2U] << 16;
        }
        if (rem > 3U) {
            word |= (uint32_t)data[i + 3U] << 24;
        }

        tiku_stm32f411_crc_feed_word(word);
    }

    return tiku_stm32f411_crc_value();
}
