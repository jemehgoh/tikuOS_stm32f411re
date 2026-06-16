/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_flash_persist_backend.h - STM32F411 flash persistence backend
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32F411_FLASH_PERSIST_BACKEND_H_
#define TIKU_STM32F411_FLASH_PERSIST_BACKEND_H_

#include <stdint.h>

#define TIKU_STM32F411_PERSIST_COMMIT_MARKER   0x54494B55UL
#define TIKU_STM32F411_PERSIST_ERASED_WORD     0xFFFFFFFFUL
#define TIKU_STM32F411_PERSIST_INVALID_SLOT    0xFFFFU

typedef enum {
    TIKU_STM32F411_PERSIST_OK       = 0,
    TIKU_STM32F411_PERSIST_NO_VALID = 1,
    TIKU_STM32F411_PERSIST_ERR_INVALID = -1,
    TIKU_STM32F411_PERSIST_ERR_RANGE   = -2,
    TIKU_STM32F411_PERSIST_ERR_FORMAT  = -3,
    TIKU_STM32F411_PERSIST_ERR_FLASH   = -4,
    TIKU_STM32F411_PERSIST_ERR_FULL    = -5
} tiku_stm32f411_persist_status_t;

typedef struct {
    uint32_t commit_marker;
    uint16_t sequence;
    uint16_t payload_crc16;
} tiku_stm32f411_persist_slot_header_t;

typedef struct {
    uint16_t used_bytes;
    uint16_t record_count;
} tiku_stm32f411_persist_payload_header_t;

typedef struct {
    uint32_t       slot_stride;
    uint16_t       slot_count;
    uint16_t       committed_slots;
    uint16_t       valid_slots;
    uint16_t       crc_fail_slots;
    uint16_t       best_slot_index;
    uint16_t       next_free_slot_index;
    uint16_t       best_sequence;
    uint8_t        have_valid_slot;
    uint8_t        sector_full;
    const uint8_t *best_payload;
} tiku_stm32f411_persist_scan_result_t;

typedef struct {
    uint16_t committed_slot_index;
    uint16_t next_free_slot_index;
    uint16_t committed_sequence;
    uint8_t  sector_full;
} tiku_stm32f411_persist_commit_result_t;

uint32_t tiku_stm32f411_persist_slot_stride(uint32_t working_copy_size);
uint16_t tiku_stm32f411_persist_slot_count(uint32_t flash_size_bytes,
                                           uint32_t working_copy_size);
uint8_t  tiku_stm32f411_persist_sequence_is_newer(uint16_t lhs,
                                                  uint16_t rhs);
uint16_t tiku_stm32f411_persist_crc16_fold(uint32_t crc32);
uint16_t tiku_stm32f411_persist_crc16_payload(const uint8_t *data,
                                              uint32_t len);

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_scan(const uint8_t *flash_base,
                            uint32_t flash_size_bytes,
                            uint32_t working_copy_size,
                            tiku_stm32f411_persist_scan_result_t *out);

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_replay_payload(const uint8_t *payload,
                                      uint32_t payload_bytes,
                                      uint8_t *working_copy_base,
                                      uint32_t working_copy_size);

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_restore_best(const uint8_t *flash_base,
                                    uint32_t flash_size_bytes,
                                    uint8_t *working_copy_base,
                                    uint32_t working_copy_size,
                                    tiku_stm32f411_persist_scan_result_t *out);

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_commit(const uint8_t *flash_base,
                              uint32_t flash_size_bytes,
                              uint8_t *working_copy_base,
                              uint32_t working_copy_size,
                              tiku_stm32f411_persist_commit_result_t *out);

#endif /* TIKU_STM32F411_FLASH_PERSIST_BACKEND_H_ */
