/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_mem_arch.c - STM32F411RE memory operations
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_mem_arch.h"
#include "tiku_flash_persist_backend.h"
#include "kernel/timers/tiku_clock.h"
#include "tiku_timer_arch.h"
#include <stdint.h>
#include <string.h>

/*---------------------------------------------------------------------------*/
/* Linker symbols                                                            */
/*---------------------------------------------------------------------------*/

extern uint8_t  __uninit_start;
extern uint8_t  __uninit_end;
extern uint32_t __tiku_nvm_flash_start;
extern uint32_t __tiku_nvm_flash_size;

/*---------------------------------------------------------------------------*/
/* STM32 flash-backed persistence state                                      */
/*---------------------------------------------------------------------------*/

static uint8_t  g_persist_have_valid_slot;
static uint8_t  g_persist_dirty;
static uint8_t  g_persist_sector_full;
static uint8_t  g_persist_due_armed;
static uint16_t g_persist_current_slot_index =
    TIKU_STM32F411_PERSIST_INVALID_SLOT;
static uint16_t g_persist_next_slot_index =
    TIKU_STM32F411_PERSIST_INVALID_SLOT;
static uint16_t g_persist_current_sequence;
static tiku_clock_time_t g_persist_due_tick;
static int32_t  g_persist_last_status =
    TIKU_STM32F411_PERSIST_NO_VALID;

#ifndef TIKU_STM32F411_PERSIST_BACKUP_INTERVAL_TICKS
#define TIKU_STM32F411_PERSIST_BACKUP_INTERVAL_TICKS \
    ((tiku_clock_time_t)(5U * TIKU_CLOCK_SECOND))
#endif

static void persist_schedule_commit(tiku_clock_time_t now_ticks)
{
    g_persist_dirty = 1U;

    if (!g_persist_due_armed) {
        g_persist_due_tick = (tiku_clock_time_t)(
            now_ticks + TIKU_STM32F411_PERSIST_BACKUP_INTERVAL_TICKS);
        g_persist_due_armed = 1U;
    }
}

static int32_t persist_commit_working_copy(void)
{
    tiku_stm32f411_persist_commit_result_t commit;
    const uint8_t *flash_base =
        (const uint8_t *)(uintptr_t)&__tiku_nvm_flash_start;
    uint8_t *working_copy = &__uninit_start;
    tiku_mem_arch_size_t flash_size =
        (tiku_mem_arch_size_t)(uintptr_t)&__tiku_nvm_flash_size;
    tiku_mem_arch_size_t working_copy_size =
        (tiku_mem_arch_size_t)((uintptr_t)&__uninit_end
                             - (uintptr_t)&__uninit_start);
    tiku_stm32f411_persist_status_t status;

    if (working_copy_size == 0U || flash_size == 0U) {
        g_persist_last_status = TIKU_STM32F411_PERSIST_ERR_RANGE;
        g_persist_dirty = 1U;
        g_persist_due_armed = 1U;
        g_persist_due_tick = (tiku_clock_time_t)(
            tiku_clock_time() + TIKU_STM32F411_PERSIST_BACKUP_INTERVAL_TICKS);
        return g_persist_last_status;
    }

    status = tiku_stm32f411_persist_commit(flash_base,
                                           flash_size,
                                           working_copy,
                                           working_copy_size,
                                           &commit);
    g_persist_last_status = status;
    if (status != TIKU_STM32F411_PERSIST_OK) {
        g_persist_dirty = 1U;
        g_persist_due_armed = 1U;
        g_persist_due_tick = (tiku_clock_time_t)(
            tiku_clock_time() + TIKU_STM32F411_PERSIST_BACKUP_INTERVAL_TICKS);
        return g_persist_last_status;
    }

    g_persist_have_valid_slot    = 1U;
    g_persist_dirty              = 0U;
    g_persist_sector_full        = commit.sector_full;
    g_persist_due_armed          = 0U;
    g_persist_current_slot_index = commit.committed_slot_index;
    g_persist_next_slot_index    = commit.next_free_slot_index;
    g_persist_current_sequence   = commit.committed_sequence;

    return g_persist_last_status;
}

void tiku_mem_arch_init(void)
{
    tiku_stm32f411_persist_scan_result_t scan;
    const uint8_t *flash_base =
        (const uint8_t *)(uintptr_t)&__tiku_nvm_flash_start;
    uint8_t *working_copy = &__uninit_start;
    tiku_mem_arch_size_t flash_size =
        (tiku_mem_arch_size_t)(uintptr_t)&__tiku_nvm_flash_size;
    tiku_mem_arch_size_t working_copy_size =
        (tiku_mem_arch_size_t)((uintptr_t)&__uninit_end
                             - (uintptr_t)&__uninit_start);
    tiku_stm32f411_persist_status_t status;

    g_persist_have_valid_slot  = 0U;
    g_persist_dirty            = 0U;
    g_persist_sector_full      = 0U;
    g_persist_due_armed        = 0U;
    g_persist_current_slot_index = TIKU_STM32F411_PERSIST_INVALID_SLOT;
    g_persist_next_slot_index    = TIKU_STM32F411_PERSIST_INVALID_SLOT;
    g_persist_current_sequence   = 0U;
    g_persist_due_tick           = 0U;
    g_persist_last_status        = TIKU_STM32F411_PERSIST_NO_VALID;

    if (working_copy_size == 0U || flash_size == 0U) {
        g_persist_last_status = TIKU_STM32F411_PERSIST_ERR_RANGE;
        return;
    }

    status = tiku_stm32f411_persist_scan(flash_base,
                                         flash_size,
                                         working_copy_size,
                                         &scan);
    g_persist_sector_full    = scan.sector_full;
    g_persist_next_slot_index = scan.next_free_slot_index;
    g_persist_last_status    = status;

    if (status != TIKU_STM32F411_PERSIST_OK || !scan.have_valid_slot) {
        /* No committed flash snapshot yet: preserve the existing STM32
         * behaviour where .uninit survives warm reset via NOLOAD
         * placement, and let higher-level magic/default logic handle a
         * true cold boot. */
        return;
    }

    /* A committed slot exists, so rebuild the SRAM working copy from
     * flash deterministically instead of merging onto stale NOLOAD data. */
    memset(working_copy, 0, working_copy_size);
    status = tiku_stm32f411_persist_replay_payload(scan.best_payload,
                                                   working_copy_size,
                                                   working_copy,
                                                   working_copy_size);
    g_persist_last_status = status;
    if (status == TIKU_STM32F411_PERSIST_OK) {
        g_persist_have_valid_slot    = 1U;
        g_persist_sector_full        = scan.sector_full;
        g_persist_current_slot_index = scan.best_slot_index;
        g_persist_next_slot_index    = scan.next_free_slot_index;
        g_persist_current_sequence   = scan.best_sequence;
    }
}

void tiku_mem_arch_secure_wipe(uint8_t *buf, tiku_mem_arch_size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    tiku_mem_arch_size_t i;

    for (i = 0; i < len; i++) {
        p[i] = 0U;
    }
}

void tiku_mem_arch_nvm_read(uint8_t *dst, const uint8_t *src,
                            tiku_mem_arch_size_t len)
{
    tiku_mem_arch_size_t i;
    for (i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

void tiku_mem_arch_nvm_write(uint8_t *dst, const uint8_t *src,
                             tiku_mem_arch_size_t len)
{
    tiku_mem_arch_size_t i;
    for (i = 0; i < len; i++) {
        dst[i] = src[i];
    }

    if (len != 0U) {
        persist_schedule_commit(tiku_clock_time());
    }
}

void tiku_mem_arch_nvm_flush(void)
{
    persist_schedule_commit(tiku_clock_time());
}

void tiku_mem_arch_persist_service(uint32_t now_ticks)
{
    tiku_clock_time_t now = (tiku_clock_time_t)now_ticks;

    if (g_persist_dirty) {
        if (!g_persist_due_armed) {
            persist_schedule_commit(now);
            return;
        }
        if (TIKU_CLOCK_LT(now, g_persist_due_tick)) {
            return;
        }

        (void)persist_commit_working_copy();
    }
}

int tiku_mem_arch_persist_commit_now(void)
{
    (void)persist_commit_working_copy();
    return (int)g_persist_last_status;
}

void tiku_mem_arch_persist_status(tiku_mem_persist_status_t *out)
{
    if (out == (tiku_mem_persist_status_t *)0) {
        return;
    }

    out->supported          = 1U;
    out->have_valid_slot    = g_persist_have_valid_slot;
    out->dirty              = g_persist_dirty;
    out->sector_full        = g_persist_sector_full;
    out->current_slot_index = g_persist_current_slot_index;
    out->next_slot_index    = g_persist_next_slot_index;
    out->current_sequence   = g_persist_current_sequence;
    out->last_status        = g_persist_last_status;
}
