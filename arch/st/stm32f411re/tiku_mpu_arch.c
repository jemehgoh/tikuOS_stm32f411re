/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_mpu_arch.c - STM32F411RE MPU bookkeeping shim
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_mpu_arch.h"
#include <stm32f411xe.h>
#include <stdint.h>

struct tiku_stm32f411_mpu_diag {
    uint32_t magic;
    uint32_t violation_count;
    uint32_t last_fault_addr;
    uint16_t violation_flags;
    uint16_t reserved;
};

#define TIKU_STM32F411_MPU_MAGIC  0x53544D50UL

static uint16_t g_mpu_sam = TIKU_MPU_DEFAULT_SAM;
static uint16_t g_mpu_ctl;
static uint32_t g_saved_primask;

__attribute__((section(".mpu_diag")))
static volatile struct tiku_stm32f411_mpu_diag g_mpu_diag;

static void stm32f411_mpu_diag_init(void)
{
    if (g_mpu_diag.magic != TIKU_STM32F411_MPU_MAGIC) {
        g_mpu_diag.magic = TIKU_STM32F411_MPU_MAGIC;
        g_mpu_diag.violation_count = 0U;
        g_mpu_diag.last_fault_addr = 0U;
        g_mpu_diag.violation_flags = 0U;
    }
}

uint16_t tiku_mpu_arch_get_sam(void)
{
    return g_mpu_sam;
}

void tiku_mpu_arch_set_sam(uint16_t sam)
{
    g_mpu_sam = sam;
}

uint16_t tiku_mpu_arch_get_ctl(void)
{
    return g_mpu_ctl;
}

void tiku_mpu_arch_disable_irq(void)
{
    g_saved_primask = __get_PRIMASK();
    __disable_irq();
}

void tiku_mpu_arch_enable_irq(void)
{
    __set_PRIMASK(g_saved_primask);
}

void tiku_mpu_arch_init_segments(void)
{
    stm32f411_mpu_diag_init();
}

void tiku_mpu_arch_set_default_protection(void)
{
    g_mpu_sam = TIKU_MPU_DEFAULT_SAM;
    g_mpu_ctl = 1U;
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
}

void tiku_mpu_arch_set_seg_perm(uint8_t seg, uint8_t perm)
{
    uint16_t shift = (uint16_t)(seg * 3U);
    uint16_t mask = (uint16_t)(0x7U << shift);

    g_mpu_sam = (uint16_t)((g_mpu_sam & ~mask)
              | (((uint16_t)perm & 0x7U) << shift));
}

uint16_t tiku_mpu_arch_unlock_nvm(void)
{
    uint16_t saved = g_mpu_sam;
    g_mpu_sam |= (uint16_t)(0x2U << 6);
    return saved;
}

void tiku_mpu_arch_lock_nvm(uint16_t saved_state)
{
    g_mpu_sam = saved_state;
}

uint16_t tiku_mpu_arch_get_violation_flags(void)
{
    stm32f411_mpu_diag_init();
    return g_mpu_diag.violation_flags;
}

void tiku_mpu_arch_clear_violation_flags(void)
{
    stm32f411_mpu_diag_init();
    g_mpu_diag.violation_flags = 0U;
    SCB->CFSR = SCB->CFSR;
}

void tiku_mpu_arch_enable_violation_nmi(void)
{
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
}

uint32_t tiku_mpu_arch_violation_count(void)
{
    stm32f411_mpu_diag_init();
    return g_mpu_diag.violation_count;
}

uint32_t tiku_mpu_arch_last_fault_addr(void)
{
    stm32f411_mpu_diag_init();
    return g_mpu_diag.last_fault_addr;
}

void tiku_stm32f411_mem_manage_handler(void)
{
    uint32_t cfsr = SCB->CFSR;

    stm32f411_mpu_diag_init();
    g_mpu_diag.violation_count++;
    g_mpu_diag.violation_flags = (uint16_t)(cfsr & 0x00FFU);
    if (cfsr & SCB_CFSR_MMARVALID_Msk) {
        g_mpu_diag.last_fault_addr = SCB->MMFAR;
    } else {
        g_mpu_diag.last_fault_addr = 0U;
    }

    SCB->CFSR = cfsr;
    SCB->AIRCR =
        (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;

    for (;;) {
        __WFE();
    }
}
