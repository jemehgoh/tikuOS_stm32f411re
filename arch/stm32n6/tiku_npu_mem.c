/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_npu_mem.c - STM32N6 boot-owned Neural-ART memory.
 *
 * The linker describes one SRAM tier and its high-end Neural-ART window.
 * This module claims that window as the first SRAM-tier reservation during
 * boot, then provides a small aligned bump allocator for NPU children.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_npu_mem.h"

#include <stdint.h>
#include <string.h>

extern uint8_t __npu_reserved_start;
extern uint8_t __npu_reserved_end;

#define TIKU_STM32N6_NPU_MEM_ALIGNMENT 32U
#define TIKU_STM32N6_NPU_MEM_ID        0x4EU

static tiku_arena_t npu_root;

// Returns 1 if value is a power of two, 0 otherwise.
// Used for validating the alignment for tiku_stm32n6_npu_mem_alloc().
static int npu_mem_power_of_two(tiku_mem_arch_size_t value)
{
    return value != 0U && ((value & (value - 1U)) == 0U);
}

static int npu_mem_expected_extent(uintptr_t *start, uintptr_t *end)
{
    uintptr_t expected_start = (uintptr_t)&__npu_reserved_start;
    uintptr_t expected_end = (uintptr_t)&__npu_reserved_end;

    if (expected_end <= expected_start ||
        expected_end - expected_start !=
            (uintptr_t)TIKU_NPU_SRAM_RESERVE_SIZE ||
        ((expected_start | expected_end) &
            (TIKU_STM32N6_NPU_MEM_ALIGNMENT - 1U)) != 0U) {
        return 0;
    }

    if (start != NULL) {
        *start = expected_start;
    }
    if (end != NULL) {
        *end = expected_end;
    }
    return 1;
}

static int npu_mem_root_valid(void)
{
    tiku_mem_stats_t stats;
    uintptr_t expected_start;
    uintptr_t expected_end;

    if (tiku_arena_stats(&npu_root, &stats) != TIKU_MEM_OK ||
        !npu_mem_expected_extent(&expected_start, &expected_end)) {
        return 0;
    }

    // Compute actual bounds
    uintptr_t root_start = (uintptr_t)npu_root.buf;
    if (npu_root.capacity > UINTPTR_MAX - root_start) {
        return 0;
    }
    uintptr_t root_end = root_start + (uintptr_t)npu_root.capacity;

    // Validate actual bounds against expected ones
    return root_start == expected_start && root_end == expected_end &&
           (npu_root.capacity == (tiku_mem_arch_size_t)
               TIKU_NPU_SRAM_RESERVE_SIZE);
}

tiku_mem_err_t tiku_stm32n6_npu_mem_reserve(void)
{
    tiku_mem_request_t request = TIKU_MEM_REQUEST_DEFAULT;
    tiku_mem_err_t err;

    if (npu_root.active) {
        /* tiku_tier_reset() invalidates a tracked arena without clearing its
         * public control block.  Treat that test-only teardown as stale and
         * allow a subsequent boot-style reservation to start cleanly. */
        if (npu_mem_root_valid()) {
            return TIKU_MEM_OK;
        }
        memset(&npu_root, 0, sizeof npu_root);
    }

    if (!npu_mem_expected_extent(NULL, NULL) ||
        TIKU_NPU_SRAM_RESERVE_SIZE == 0U) {
        return TIKU_MEM_ERR_INVALID;
    }

    request.alignment = TIKU_STM32N6_NPU_MEM_ALIGNMENT;
    request.allocation_class = TIKU_MEM_TRANSIENT;

    err = tiku_tier_arena_create_opts(
        &npu_root, TIKU_MEM_SRAM,
        (tiku_mem_arch_size_t)TIKU_NPU_SRAM_RESERVE_SIZE,
        TIKU_STM32N6_NPU_MEM_ID, &request);
    if (err != TIKU_MEM_OK) {
        memset(&npu_root, 0, sizeof npu_root);
        return err;
    }

    if (!npu_mem_root_valid()) {
        (void)tiku_arena_destroy(&npu_root);
        return TIKU_MEM_ERR_INVALID;
    }

    return TIKU_MEM_OK;
}

void *tiku_stm32n6_npu_mem_alloc(tiku_mem_arch_size_t size,
                                 tiku_mem_arch_size_t alignment)
{
    TIKU_MEM_KERNEL_ONLY(NULL);

    if (!npu_mem_root_valid() || size == 0U) {
        return NULL;
    }

    if (alignment == 0U) {
        alignment = TIKU_MEM_ARCH_ALIGNMENT;
    }
    if (alignment < TIKU_MEM_ARCH_ALIGNMENT ||
        !npu_mem_power_of_two(alignment)) {
        npu_root.fail++;
        return NULL;
    }

    uintptr_t base = (uintptr_t)npu_root.buf;
    if (npu_root.offset > UINTPTR_MAX - base) {
        npu_root.fail++;
        return NULL;
    }
    uintptr_t raw = base + (uintptr_t)npu_root.offset;
    tiku_mem_arch_size_t mask = alignment - 1U;
    if (raw > UINTPTR_MAX - (uintptr_t)mask) {
        npu_root.fail++;
        return NULL;
    }
    uintptr_t aligned = (raw + (uintptr_t)mask) & ~(uintptr_t)mask;
    tiku_mem_arch_size_t adjustment = (tiku_mem_arch_size_t)(aligned - raw);

    if (adjustment > (npu_root.capacity - npu_root.offset) ||
        size > (npu_root.capacity - npu_root.offset - adjustment)) {
        npu_root.fail++;
        return NULL;
    }

    tiku_mem_arch_size_t rounded = size;
    if (rounded > (tiku_mem_arch_size_t)~(tiku_mem_arch_size_t)0 - mask) {
        npu_root.fail++;
        return NULL;
    }
    rounded = (rounded + mask) & ~mask;
    if (rounded > npu_root.capacity - npu_root.offset - adjustment) {
        npu_root.fail++;
        return NULL;
    }

    npu_root.offset += adjustment;
    {
        void *result = &npu_root.buf[npu_root.offset];
        npu_root.offset += rounded;
        npu_root.count++;
        if (npu_root.offset > npu_root.peak) npu_root.peak = npu_root.offset;
        return result;
    }
}

void *tiku_stm32n6_npu_mem_alloc_at(uintptr_t address,
                                    tiku_mem_arch_size_t size,
                                    tiku_mem_arch_size_t alignment)
{
    uintptr_t base;
    uintptr_t cursor;
    uintptr_t end;
    tiku_mem_arch_size_t mask;
    tiku_mem_arch_size_t rounded;
    tiku_mem_arch_size_t new_offset;

    TIKU_MEM_KERNEL_ONLY(NULL);

    if (!npu_mem_root_valid() || size == 0U ||
        alignment < TIKU_STM32N6_NPU_MEM_ALIGNMENT ||
        !npu_mem_power_of_two(alignment)) {
        if (npu_root.active) npu_root.fail++;
        return NULL;
    }

    mask = alignment - 1U;
    if ((address & (uintptr_t)mask) != 0U ||
        size > (tiku_mem_arch_size_t)~(tiku_mem_arch_size_t)0 - mask) {
        npu_root.fail++;
        return NULL;
    }
    rounded = (size + mask) & ~mask;

    base = (uintptr_t)npu_root.buf;
    if (npu_root.offset > UINTPTR_MAX - base) {
        npu_root.fail++;
        return NULL;
    }
    cursor = base + (uintptr_t)npu_root.offset;
    if (address < cursor || address < base ||
        address - base > (uintptr_t)npu_root.capacity ||
        rounded > (tiku_mem_arch_size_t)(npu_root.capacity -
            (tiku_mem_arch_size_t)(address - base)) ||
        address > UINTPTR_MAX - (uintptr_t)rounded) {
        npu_root.fail++;
        return NULL;
    }
    end = address + (uintptr_t)rounded;
    new_offset = (tiku_mem_arch_size_t)(end - base);

    /* Publish the new cursor only after every range calculation succeeds. */
    npu_root.offset = new_offset;
    npu_root.count++;
    if (npu_root.offset > npu_root.peak) npu_root.peak = npu_root.offset;
    return (void *)address;
}

tiku_mem_err_t tiku_stm32n6_npu_mem_reset(void)
{
    if (!npu_mem_root_valid()) {
        return TIKU_MEM_ERR_INVALID;
    }
    return tiku_arena_reset(&npu_root);
}

const tiku_arena_t *tiku_stm32n6_npu_mem_root(void)
{
    return npu_mem_root_valid() ? &npu_root : NULL;
}
