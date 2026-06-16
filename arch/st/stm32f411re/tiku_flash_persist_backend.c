/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_flash_persist_backend.c - STM32F411 flash persistence backend
 *
 * Implements the STM32F411 slot-scan / CRC-verify / replay / commit
 * logic for the wear-levelled flash-backed persistence design.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_flash_persist_backend.h"
#include "tiku_crc_arch_stm32f411.h"
#include "tiku_device_select.h"
#include <stdint.h>
#include <stm32f411xe.h>
#include <string.h>

#define TIKU_STM32F411_PERSIST_HDR_BYTES \
    ((uint32_t)sizeof(tiku_stm32f411_persist_slot_header_t))
#define TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES \
    ((uint32_t)sizeof(tiku_stm32f411_persist_payload_header_t))
#define TIKU_STM32F411_PERSIST_ZERO_GAP_THRESHOLD  4U
#define TIKU_STM32F411_PERSIST_FLASH_TIMEOUT       10000000UL
#define TIKU_STM32F411_PERSIST_FLASH_PSIZE_WORD    FLASH_CR_PSIZE_1
#define TIKU_STM32F411_PERSIST_FLASH_ERR_MASK      \
    (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)

typedef struct {
    uint16_t       addr_lo16;
    uint16_t       data_len;
    const uint8_t *data;
    uint32_t       next_off;
} tiku_stm32f411_payload_record_t;

typedef struct {
    uint16_t used_bytes;
    uint16_t record_count;
} tiku_stm32f411_payload_summary_t;

static uint32_t persist_align4(uint32_t value)
{
    return (value + 3U) & ~3U;
}

static uint16_t persist_read_u16(const uint8_t *src)
{
    uint16_t value;

    memcpy(&value, src, sizeof(value));
    return value;
}

static void persist_scan_result_reset(tiku_stm32f411_persist_scan_result_t *out)
{
    if (out == (tiku_stm32f411_persist_scan_result_t *)0) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->best_slot_index = TIKU_STM32F411_PERSIST_INVALID_SLOT;
    out->next_free_slot_index = TIKU_STM32F411_PERSIST_INVALID_SLOT;
}

static void persist_commit_result_reset(
    tiku_stm32f411_persist_commit_result_t *out)
{
    if (out == (tiku_stm32f411_persist_commit_result_t *)0) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->committed_slot_index = TIKU_STM32F411_PERSIST_INVALID_SLOT;
    out->next_free_slot_index = TIKU_STM32F411_PERSIST_INVALID_SLOT;
}

static const uint8_t *persist_slot_addr(const uint8_t *flash_base,
                                        uint32_t slot_stride,
                                        uint16_t slot_index)
{
    return flash_base + ((uint32_t)slot_index * slot_stride);
}

static uint8_t persist_slot_is_erased(
    const tiku_stm32f411_persist_slot_header_t *hdr)
{
    return (hdr->commit_marker == TIKU_STM32F411_PERSIST_ERASED_WORD &&
            hdr->sequence == 0xFFFFU &&
            hdr->payload_crc16 == 0xFFFFU) ? 1U : 0U;
}

static uint8_t persist_ranges_overlap(uintptr_t a_base, uint32_t a_len,
                                      uintptr_t b_base, uint32_t b_len)
{
    uintptr_t a_end;
    uintptr_t b_end;

    if (a_len == 0U || b_len == 0U) {
        return 0U;
    }

    a_end = a_base + (uintptr_t)a_len;
    b_end = b_base + (uintptr_t)b_len;

    return (a_base < b_end && b_base < a_end) ? 1U : 0U;
}

static tiku_stm32f411_persist_status_t
persist_payload_record_parse(const uint8_t *payload,
                             uint32_t used_bytes,
                             uint32_t off,
                             tiku_stm32f411_payload_record_t *out)
{
    uint16_t data_len;
    uint32_t data_off;
    uint32_t next_off;

    if (payload == (const uint8_t *)0 ||
        out == (tiku_stm32f411_payload_record_t *)0) {
        return TIKU_STM32F411_PERSIST_ERR_INVALID;
    }

    if (off > used_bytes || used_bytes - off < 4U) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    data_len = persist_read_u16(payload + off + 2U);
    if (data_len == 0U) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    data_off = off + 4U;
    if (data_off > used_bytes || (uint32_t)data_len > (used_bytes - data_off)) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    next_off = persist_align4(data_off + (uint32_t)data_len);
    if (next_off > used_bytes) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    out->addr_lo16 = persist_read_u16(payload + off);
    out->data_len  = data_len;
    out->data      = payload + data_off;
    out->next_off  = next_off;

    return TIKU_STM32F411_PERSIST_OK;
}

static uint8_t persist_find_next_record_span(const uint8_t *working_copy,
                                             uint32_t working_copy_size,
                                             uint32_t cursor,
                                             uint32_t *out_start,
                                             uint32_t *out_end)
{
    uint32_t start = cursor;
    uint32_t i;
    uint32_t last_nonzero;
    uint32_t zero_run = 0U;

    if (working_copy == (const uint8_t *)0 ||
        out_start == (uint32_t *)0 ||
        out_end == (uint32_t *)0) {
        return 0U;
    }

    while (start < working_copy_size && working_copy[start] == 0U) {
        start++;
    }
    if (start >= working_copy_size) {
        return 0U;
    }

    last_nonzero = start;
    for (i = start + 1U; i < working_copy_size; i++) {
        if (working_copy[i] != 0U) {
            last_nonzero = i;
            zero_run = 0U;
        } else {
            zero_run++;
            if (zero_run >= TIKU_STM32F411_PERSIST_ZERO_GAP_THRESHOLD) {
                break;
            }
        }
    }

    *out_start = start;
    *out_end   = last_nonzero + 1U;
    return 1U;
}

static tiku_stm32f411_persist_status_t
persist_payload_measure(const uint8_t *working_copy,
                        uint32_t working_copy_size,
                        tiku_stm32f411_payload_summary_t *out)
{
    uint32_t cursor = 0U;
    uint32_t used_bytes = TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES;
    uint32_t record_count = 0U;

    if (working_copy == (const uint8_t *)0 ||
        out == (tiku_stm32f411_payload_summary_t *)0) {
        return TIKU_STM32F411_PERSIST_ERR_INVALID;
    }
    if (working_copy_size < TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES ||
        working_copy_size > 0xFFFFU) {
        return TIKU_STM32F411_PERSIST_ERR_RANGE;
    }

    while (cursor < working_copy_size) {
        uint32_t start;
        uint32_t end;

        if (!persist_find_next_record_span(working_copy, working_copy_size,
                                           cursor, &start, &end)) {
            break;
        }

        while (start < end) {
            uint32_t chunk_len = end - start;
            uint32_t record_bytes;

            if (chunk_len > 0xFFFFU) {
                chunk_len = 0xFFFFU;
            }

            record_bytes = persist_align4(4U + chunk_len);
            used_bytes += record_bytes;
            record_count++;

            if (used_bytes > working_copy_size || record_count > 0xFFFFU) {
                return TIKU_STM32F411_PERSIST_ERR_FULL;
            }

            start += chunk_len;
        }

        cursor = end;
    }

    out->used_bytes   = (uint16_t)used_bytes;
    out->record_count = (uint16_t)record_count;
    return TIKU_STM32F411_PERSIST_OK;
}

static uint32_t persist_pack_data_word(const uint8_t *src,
                                       uint16_t data_len,
                                       uint32_t byte_off)
{
    uint32_t word = 0xFFFFFFFFUL;
    uint32_t i;

    for (i = 0U; i < 4U; i++) {
        uint32_t idx = byte_off + i;

        if (idx < (uint32_t)data_len) {
            word &= ~(0xFFUL << (i * 8U));
            word |= (uint32_t)src[idx] << (i * 8U);
        }
    }

    return word;
}

static tiku_stm32f411_persist_status_t persist_flash_wait_ready(void)
{
    uint32_t timeout = TIKU_STM32F411_PERSIST_FLASH_TIMEOUT;

    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
        if (timeout-- == 0U) {
            return TIKU_STM32F411_PERSIST_ERR_FLASH;
        }
    }

    if ((FLASH->SR & TIKU_STM32F411_PERSIST_FLASH_ERR_MASK) != 0U) {
        FLASH->SR = TIKU_STM32F411_PERSIST_FLASH_ERR_MASK;
        return TIKU_STM32F411_PERSIST_ERR_FLASH;
    }

    if ((FLASH->SR & FLASH_SR_EOP) != 0U) {
        FLASH->SR = FLASH_SR_EOP;
    }

    return TIKU_STM32F411_PERSIST_OK;
}

static void persist_flash_clear_status(void)
{
    FLASH->SR = FLASH_SR_EOP | TIKU_STM32F411_PERSIST_FLASH_ERR_MASK;
}

static tiku_stm32f411_persist_status_t persist_flash_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) == 0U) {
        return TIKU_STM32F411_PERSIST_OK;
    }

    FLASH->KEYR = 0x45670123UL;
    FLASH->KEYR = 0xCDEF89ABUL;

    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        return TIKU_STM32F411_PERSIST_ERR_FLASH;
    }

    return TIKU_STM32F411_PERSIST_OK;
}

static void persist_flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static tiku_stm32f411_persist_status_t persist_flash_program_word(
    uintptr_t addr, uint32_t word)
{
    tiku_stm32f411_persist_status_t status;
    uint32_t cr;

    if ((addr & 0x3U) != 0U) {
        return TIKU_STM32F411_PERSIST_ERR_RANGE;
    }

    status = persist_flash_wait_ready();
    if (status != TIKU_STM32F411_PERSIST_OK) {
        return status;
    }

    persist_flash_clear_status();

    cr = FLASH->CR;
    cr &= ~(FLASH_CR_SER | FLASH_CR_SNB_Msk | FLASH_CR_PSIZE_Msk);
    cr |= FLASH_CR_PG | TIKU_STM32F411_PERSIST_FLASH_PSIZE_WORD;
    FLASH->CR = cr;

    *(volatile uint32_t *)addr = word;

    status = persist_flash_wait_ready();
    FLASH->CR &= ~FLASH_CR_PG;
    if (status != TIKU_STM32F411_PERSIST_OK) {
        return status;
    }

    if (*(const volatile uint32_t *)addr != word) {
        return TIKU_STM32F411_PERSIST_ERR_FLASH;
    }

    return TIKU_STM32F411_PERSIST_OK;
}

static tiku_stm32f411_persist_status_t persist_flash_erase_sector(void)
{
    tiku_stm32f411_persist_status_t status;
    uint32_t cr;

    status = persist_flash_wait_ready();
    if (status != TIKU_STM32F411_PERSIST_OK) {
        return status;
    }

    persist_flash_clear_status();

    cr = FLASH->CR;
    cr &= ~(FLASH_CR_PG | FLASH_CR_SNB_Msk | FLASH_CR_PSIZE_Msk);
    cr |= FLASH_CR_SER
       | TIKU_STM32F411_PERSIST_FLASH_PSIZE_WORD
       | ((uint32_t)TIKU_DEVICE_FLASH_PERSIST_SECTOR << FLASH_CR_SNB_Pos);
    FLASH->CR = cr;
    FLASH->CR |= FLASH_CR_STRT;

    status = persist_flash_wait_ready();
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB_Msk);
    return status;
}

static tiku_stm32f411_persist_status_t
persist_program_payload(uintptr_t payload_addr,
                        const uint8_t *working_copy_base,
                        uint32_t working_copy_size,
                        const tiku_stm32f411_payload_summary_t *summary)
{
    tiku_stm32f411_persist_status_t status;
    uintptr_t flash_addr = payload_addr;
    uint32_t cursor = 0U;
    uint32_t expected = 0U;
    uint32_t payload_word;

    if (working_copy_base == (const uint8_t *)0 ||
        summary == (const tiku_stm32f411_payload_summary_t *)0) {
        return TIKU_STM32F411_PERSIST_ERR_INVALID;
    }

    payload_word = (uint32_t)summary->used_bytes
                 | ((uint32_t)summary->record_count << 16);
    status = persist_flash_program_word(flash_addr, payload_word);
    if (status != TIKU_STM32F411_PERSIST_OK) {
        return status;
    }

    flash_addr += 4U;
    expected += 4U;

    while (cursor < working_copy_size) {
        uint32_t start;
        uint32_t end;

        if (!persist_find_next_record_span(working_copy_base,
                                           working_copy_size,
                                           cursor,
                                           &start,
                                           &end)) {
            break;
        }

        while (start < end) {
            uint32_t chunk_len = end - start;
            uint16_t data_len;
            uint32_t data_off;
            uint32_t header_word;

            if (chunk_len > 0xFFFFU) {
                chunk_len = 0xFFFFU;
            }

            data_len = (uint16_t)chunk_len;
            header_word =
                (((uint32_t)data_len) << 16)
                | ((uint32_t)(((uintptr_t)(working_copy_base + start)) & 0xFFFFU));

            status = persist_flash_program_word(flash_addr, header_word);
            if (status != TIKU_STM32F411_PERSIST_OK) {
                return status;
            }

            flash_addr += 4U;
            expected += 4U;

            for (data_off = 0U;
                 data_off < persist_align4((uint32_t)data_len);
                 data_off += 4U) {
                uint32_t word =
                    persist_pack_data_word(working_copy_base + start,
                                           data_len,
                                           data_off);

                status = persist_flash_program_word(flash_addr, word);
                if (status != TIKU_STM32F411_PERSIST_OK) {
                    return status;
                }

                flash_addr += 4U;
                expected += 4U;
            }

            start += (uint32_t)data_len;
        }

        cursor = end;
    }

    if (expected != (uint32_t)summary->used_bytes) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    return TIKU_STM32F411_PERSIST_OK;
}

uint16_t tiku_stm32f411_persist_crc16_fold(uint32_t crc32)
{
    return (uint16_t)(crc32 ^ (crc32 >> 16));
}

uint16_t tiku_stm32f411_persist_crc16_payload(const uint8_t *data,
                                              uint32_t len)
{
    return tiku_stm32f411_persist_crc16_fold(
        tiku_stm32f411_crc32_payload(data, len));
}

uint32_t tiku_stm32f411_persist_slot_stride(uint32_t working_copy_size)
{
    return persist_align4(TIKU_STM32F411_PERSIST_HDR_BYTES + working_copy_size);
}

uint16_t tiku_stm32f411_persist_slot_count(uint32_t flash_size_bytes,
                                           uint32_t working_copy_size)
{
    uint32_t stride = tiku_stm32f411_persist_slot_stride(working_copy_size);
    uint32_t count;

    if (stride == 0U || flash_size_bytes < stride) {
        return 0U;
    }

    count = flash_size_bytes / stride;
    if (count > 0xFFFFU) {
        count = 0xFFFFU;
    }

    return (uint16_t)count;
}

uint8_t tiku_stm32f411_persist_sequence_is_newer(uint16_t lhs, uint16_t rhs)
{
    return ((int16_t)(lhs - rhs) > 0) ? 1U : 0U;
}

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_scan(const uint8_t *flash_base,
                            uint32_t flash_size_bytes,
                            uint32_t working_copy_size,
                            tiku_stm32f411_persist_scan_result_t *out)
{
    tiku_stm32f411_persist_status_t status =
        TIKU_STM32F411_PERSIST_NO_VALID;
    uint32_t stride;
    uint16_t slot_count;
    uint16_t i;

    persist_scan_result_reset(out);

    if (flash_base == (const uint8_t *)0 ||
        out == (tiku_stm32f411_persist_scan_result_t *)0 ||
        working_copy_size < TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES) {
        return TIKU_STM32F411_PERSIST_ERR_INVALID;
    }
    if (working_copy_size > 0xFFFFU) {
        return TIKU_STM32F411_PERSIST_ERR_RANGE;
    }

    stride = tiku_stm32f411_persist_slot_stride(working_copy_size);
    slot_count = tiku_stm32f411_persist_slot_count(flash_size_bytes,
                                                   working_copy_size);
    if (slot_count == 0U) {
        return TIKU_STM32F411_PERSIST_ERR_RANGE;
    }

    out->slot_stride = stride;
    out->slot_count  = slot_count;
    out->sector_full = 1U;

    for (i = 0U; i < slot_count; i++) {
        const uint8_t *slot = persist_slot_addr(flash_base, stride, i);
        const tiku_stm32f411_persist_slot_header_t *hdr =
            (const tiku_stm32f411_persist_slot_header_t *)slot;

        if (persist_slot_is_erased(hdr)) {
            if (out->next_free_slot_index ==
                TIKU_STM32F411_PERSIST_INVALID_SLOT) {
                out->next_free_slot_index = i;
            }
            out->sector_full = 0U;
            continue;
        }

        if (hdr->commit_marker != TIKU_STM32F411_PERSIST_COMMIT_MARKER) {
            continue;
        }

        out->committed_slots++;

        if (tiku_stm32f411_persist_crc16_payload(
                slot + TIKU_STM32F411_PERSIST_HDR_BYTES,
                working_copy_size) !=
            hdr->payload_crc16) {
            out->crc_fail_slots++;
            continue;
        }

        out->valid_slots++;

        if (!out->have_valid_slot ||
            tiku_stm32f411_persist_sequence_is_newer(hdr->sequence,
                                                     out->best_sequence)) {
            out->have_valid_slot = 1U;
            out->best_slot_index = i;
            out->best_sequence   = hdr->sequence;
            out->best_payload    = slot + TIKU_STM32F411_PERSIST_HDR_BYTES;
        }
    }

    if (out->have_valid_slot) {
        status = TIKU_STM32F411_PERSIST_OK;
    }

    if (out->next_free_slot_index == TIKU_STM32F411_PERSIST_INVALID_SLOT &&
        !out->sector_full) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    return status;
}

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_replay_payload(const uint8_t *payload,
                                      uint32_t payload_bytes,
                                      uint8_t *working_copy_base,
                                      uint32_t working_copy_size)
{
    tiku_stm32f411_payload_record_t rec;
    uintptr_t working_base;
    uintptr_t working_end;
    uintptr_t working_hi;
    uint16_t  used_bytes;
    uint16_t  record_count;
    uint16_t  i;
    uint32_t  off;

    if (payload == (const uint8_t *)0 || working_copy_base == (uint8_t *)0) {
        return TIKU_STM32F411_PERSIST_ERR_INVALID;
    }
    if (payload_bytes < TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES ||
        working_copy_size == 0U ||
        working_copy_size > 0xFFFFU) {
        return TIKU_STM32F411_PERSIST_ERR_RANGE;
    }

    working_base = (uintptr_t)working_copy_base;
    working_end  = working_base + (uintptr_t)working_copy_size;
    working_hi   = working_base & ~(uintptr_t)0xFFFFU;

    if (((working_base ^ (working_end - 1U)) & ~(uintptr_t)0xFFFFU) != 0U) {
        return TIKU_STM32F411_PERSIST_ERR_RANGE;
    }

    used_bytes   = persist_read_u16(payload);
    record_count = persist_read_u16(payload + 2U);

    if (used_bytes < TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES ||
        used_bytes > payload_bytes) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    off = TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES;
    for (i = 0U; i < record_count; i++) {
        tiku_stm32f411_persist_status_t status;
        uintptr_t dst;
        uint16_t j;
        uint32_t prev_off;

        status = persist_payload_record_parse(payload, used_bytes, off, &rec);
        if (status != TIKU_STM32F411_PERSIST_OK) {
            return status;
        }

        dst = working_hi | (uintptr_t)rec.addr_lo16;
        if (dst < working_base || dst + (uintptr_t)rec.data_len > working_end) {
            return TIKU_STM32F411_PERSIST_ERR_RANGE;
        }

        prev_off = TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES;
        for (j = 0U; j < i; j++) {
            tiku_stm32f411_payload_record_t prev;
            uintptr_t prev_dst;

            status = persist_payload_record_parse(payload, used_bytes,
                                                  prev_off, &prev);
            if (status != TIKU_STM32F411_PERSIST_OK) {
                return status;
            }

            prev_dst = working_hi | (uintptr_t)prev.addr_lo16;
            if (persist_ranges_overlap(dst, (uint32_t)rec.data_len,
                                       prev_dst, (uint32_t)prev.data_len)) {
                return TIKU_STM32F411_PERSIST_ERR_FORMAT;
            }

            prev_off = prev.next_off;
        }

        memcpy((void *)dst, rec.data, rec.data_len);
        off = rec.next_off;
    }

    if (off > used_bytes) {
        return TIKU_STM32F411_PERSIST_ERR_FORMAT;
    }

    return TIKU_STM32F411_PERSIST_OK;
}

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_restore_best(const uint8_t *flash_base,
                                    uint32_t flash_size_bytes,
                                    uint8_t *working_copy_base,
                                    uint32_t working_copy_size,
                                    tiku_stm32f411_persist_scan_result_t *out)
{
    tiku_stm32f411_persist_scan_result_t local;
    tiku_stm32f411_persist_scan_result_t *scan_result = out;
    tiku_stm32f411_persist_status_t status;

    if (scan_result == (tiku_stm32f411_persist_scan_result_t *)0) {
        scan_result = &local;
    }

    status = tiku_stm32f411_persist_scan(flash_base, flash_size_bytes,
                                         working_copy_size, scan_result);
    if (status != TIKU_STM32F411_PERSIST_OK) {
        return status;
    }

    return tiku_stm32f411_persist_replay_payload(scan_result->best_payload,
                                                 working_copy_size,
                                                 working_copy_base,
                                                 working_copy_size);
}

tiku_stm32f411_persist_status_t
tiku_stm32f411_persist_commit(const uint8_t *flash_base,
                              uint32_t flash_size_bytes,
                              uint8_t *working_copy_base,
                              uint32_t working_copy_size,
                              tiku_stm32f411_persist_commit_result_t *out)
{
    tiku_stm32f411_persist_scan_result_t scan;
    tiku_stm32f411_payload_summary_t summary;
    tiku_stm32f411_persist_commit_result_t local;
    tiku_stm32f411_persist_commit_result_t *result = out;
    tiku_stm32f411_persist_status_t status;
    uint16_t slot_index;
    uint16_t sequence;
    uintptr_t slot_addr;
    uintptr_t payload_addr;
    uint16_t payload_crc16;
    uint32_t primask;

    if (result == (tiku_stm32f411_persist_commit_result_t *)0) {
        result = &local;
    }
    persist_commit_result_reset(result);

    if (flash_base == (const uint8_t *)0 || working_copy_base == (uint8_t *)0) {
        return TIKU_STM32F411_PERSIST_ERR_INVALID;
    }
    if (working_copy_size < TIKU_STM32F411_PERSIST_PAYLOAD_HDR_BYTES ||
        working_copy_size > 0xFFFFU) {
        return TIKU_STM32F411_PERSIST_ERR_RANGE;
    }

    status = tiku_stm32f411_persist_scan(flash_base,
                                         flash_size_bytes,
                                         working_copy_size,
                                         &scan);
    if (status < 0) {
        return status;
    }

    status = persist_payload_measure(working_copy_base,
                                     working_copy_size,
                                     &summary);
    if (status != TIKU_STM32F411_PERSIST_OK) {
        return status;
    }

    if (scan.sector_full) {
        slot_index = 0U;
    } else {
        slot_index = scan.next_free_slot_index;
        if (slot_index == TIKU_STM32F411_PERSIST_INVALID_SLOT) {
            return TIKU_STM32F411_PERSIST_ERR_FORMAT;
        }
    }

    sequence = scan.have_valid_slot ? (uint16_t)(scan.best_sequence + 1U) : 0U;
    slot_addr = (uintptr_t)persist_slot_addr(flash_base, scan.slot_stride, slot_index);
    payload_addr = slot_addr + TIKU_STM32F411_PERSIST_HDR_BYTES;

    primask = __get_PRIMASK();
    __disable_irq();

    status = persist_flash_unlock();
    if (status == TIKU_STM32F411_PERSIST_OK && scan.sector_full) {
        status = persist_flash_erase_sector();
    }
    if (status == TIKU_STM32F411_PERSIST_OK) {
        const tiku_stm32f411_persist_slot_header_t *slot_hdr =
            (const tiku_stm32f411_persist_slot_header_t *)slot_addr;

        if (!persist_slot_is_erased(slot_hdr)) {
            status = TIKU_STM32F411_PERSIST_ERR_FLASH;
        }
    }
    if (status == TIKU_STM32F411_PERSIST_OK) {
        status = persist_program_payload(payload_addr,
                                         working_copy_base,
                                         working_copy_size,
                                         &summary);
    }
    if (status == TIKU_STM32F411_PERSIST_OK) {
        payload_crc16 = tiku_stm32f411_persist_crc16_payload(
            (const uint8_t *)payload_addr, working_copy_size);
        status = persist_flash_program_word(
            slot_addr + 4U,
            (uint32_t)sequence | ((uint32_t)payload_crc16 << 16));
    }
    if (status == TIKU_STM32F411_PERSIST_OK) {
        status = persist_flash_program_word(slot_addr,
                                            TIKU_STM32F411_PERSIST_COMMIT_MARKER);
    }

    persist_flash_lock();
    __set_PRIMASK(primask);

    if (status != TIKU_STM32F411_PERSIST_OK) {
        return status;
    }

    result->committed_slot_index = slot_index;
    result->committed_sequence   = sequence;
    if ((uint16_t)(slot_index + 1U) < scan.slot_count) {
        result->next_free_slot_index = (uint16_t)(slot_index + 1U);
        result->sector_full = 0U;
    } else {
        result->next_free_slot_index = TIKU_STM32F411_PERSIST_INVALID_SLOT;
        result->sector_full = 1U;
    }

    return TIKU_STM32F411_PERSIST_OK;
}
