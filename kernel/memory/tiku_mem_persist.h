/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: OpenAI Codex
 *
 * tiku_mem_persist.h - Shared flash-backed persistence status types
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_MEM_PERSIST_H_
#define TIKU_MEM_PERSIST_H_

#include <stdint.h>

/*---------------------------------------------------------------------------*/
/* FLASH-BACKED PERSISTENCE CONTROL                                          */
/*---------------------------------------------------------------------------*/

/**
 * @brief Platform-neutral snapshot of flash-backed persistence state.
 */
typedef struct tiku_mem_persist_status {
    uint8_t  supported;           /**< Non-zero if platform exposes API */
    uint8_t  have_valid_slot;     /**< Non-zero if boot restore found one */
    uint8_t  dirty;               /**< Non-zero if SRAM differs from flash */
    uint8_t  sector_full;         /**< Non-zero if next commit needs erase */
    uint16_t current_slot_index;  /**< Most recent valid slot, or 0xFFFF */
    uint16_t next_slot_index;     /**< Next erased slot, or 0xFFFF */
    uint16_t current_sequence;    /**< Sequence of current_slot_index */
    int32_t  last_status;         /**< Last backend status / error code */
} tiku_mem_persist_status_t;

#endif /* TIKU_MEM_PERSIST_H_ */
