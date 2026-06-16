/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: OpenAI Codex
 *
 * tiku_crc_arch_stm32f411.h - STM32F411 hardware CRC helper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32F411_CRC_ARCH_H_
#define TIKU_STM32F411_CRC_ARCH_H_

#include <stdint.h>

void     tiku_stm32f411_crc_init(void);
void     tiku_stm32f411_crc_reset(void);
void     tiku_stm32f411_crc_feed_word(uint32_t word);
uint32_t tiku_stm32f411_crc_value(void);
uint32_t tiku_stm32f411_crc32_payload(const uint8_t *data, uint32_t len);

#endif /* TIKU_STM32F411_CRC_ARCH_H_ */
