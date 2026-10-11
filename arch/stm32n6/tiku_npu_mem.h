/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * STM32N6 Neural-ART boot-owned memory reservation.
 *
 * The reserved window is intentionally absent from the generic region table.
 * NPU child arenas and pools may therefore use raw constructors only over
 * slices returned by this root allocator; checked caller-buffer APIs must not
 * be used for those child objects.  This allocator boundary prevents
 * accidental generic allocation, but does not prevent privileged code from
 * writing arbitrary addresses.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32N6_NPU_MEM_H_
#define TIKU_STM32N6_NPU_MEM_H_

#include <kernel/memory/tiku_mem.h>

#if (TIKU_HAS_NPU + 0) && !defined(TIKU_NPU_SRAM_RESERVE_SIZE)
#error "STM32N6 NPU build requires TIKU_NPU_SRAM_RESERVE_SIZE"
#endif

/**
 * @brief Reserve the STM32N6 Neural-ART window from the SRAM tier.
 *
 * The reservation is boot-owned and idempotent.  It remains live until the
 * tier allocator itself is reset; callers must not destroy the returned root.
 */
tiku_mem_err_t tiku_stm32n6_npu_mem_reserve(void);

/**
 * @brief Allocate an aligned slice from the boot-owned NPU root arena.
 *
 * @param size      Number of bytes requested; must be non-zero.
 * @param alignment Power-of-two alignment, or zero for the platform default.
 * @return Aligned slice, or NULL when the root is unavailable or full.
 */
void *tiku_stm32n6_npu_mem_alloc(tiku_mem_arch_size_t size,
                                 tiku_mem_arch_size_t alignment);

/**
 * @brief Claim an aligned slice at an exact address in the NPU root.
 *
 * This is the fixed-destination companion to
 * tiku_stm32n6_npu_mem_alloc().  It may only advance the monotonic root
 * cursor: any gap before @p address is charged to the current model layout.
 * Failed claims do not change the cursor.
 *
 * @param address   Required backing address.
 * @param size      Number of bytes requested; must be non-zero.
 * @param alignment Power-of-two alignment of at least 32 bytes.
 * @return @p address on success, otherwise NULL.
 */
void *tiku_stm32n6_npu_mem_alloc_at(uintptr_t address,
                                    tiku_mem_arch_size_t size,
                                    tiku_mem_arch_size_t alignment);

/**
 * @brief Rewind child allocations while retaining the root reservation.
 */
tiku_mem_err_t tiku_stm32n6_npu_mem_reset(void);

/**
 * @brief Return the boot-owned root arena for read-only inspection.
 *
 * @return Root arena when reserved, otherwise NULL.
 */
const tiku_arena_t *tiku_stm32n6_npu_mem_root(void);

#endif /* TIKU_STM32N6_NPU_MEM_H_ */
