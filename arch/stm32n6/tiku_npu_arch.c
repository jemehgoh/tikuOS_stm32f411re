/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 *
 * Author: Jeremy Goh
 *
 * tiku_npu_arch.c - STM32N6 board bring-up for the LL-ATON backend.
 *
 * LL-ATON owns the ATON clock gates, ATON interrupt controller, epoch
 * controller, relocation, and inference execution. This file retains only
 * board-level power/security setup and the linker-owned NPU tier.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <kernel/memory/tiku_mem.h>
#include <interfaces/npu/tiku_npu.h>
#include "tiku_npu_arch.h"
#include "tiku_sram_arch.h"
#include "tiku_stm32n6_regs.h"

/* Model lifecycle and the ST interrupt callback live in the LL-ATON adapter. */
extern void tiku_npu_llaton_irq_bridge(void);
extern int tiku_npu_llaton_runtime_init(void);

extern uint8_t __axisram_start;
extern uint8_t __tier_sram_start;
extern uint8_t __tier_sram_end;
extern uint8_t __tier_npu_start;
extern uint8_t __tier_npu_end;
extern uint8_t __uninit_start;
extern uint8_t __uninit_end;

/* Keep the non-secure NPU alias selection local to this board backend. The
 * generic STM32N6 register header is also used by unrelated drivers. */
#define TIKU_STM32N6_NPU_RCC_NS_AHB5ENR   (STM32N6_RCC_BASE + 0x260U)
#define TIKU_STM32N6_NPU_RCC_NS_AHB5RSTR  (STM32N6_RCC_BASE + 0x220U)
#define TIKU_STM32N6_NPU_RCC_NS_AHB5LPENR (STM32N6_RCC_BASE + 0x2A0U)
#define TIKU_STM32N6_CACHEAXI_NS_CR1      (STM32N6_CACHEAXI_BASE_NS + 0x00U)

static uint8_t npu_initialized;

/* The boot ROM leaves the outer NPU domain in reset on this SRAM-boot path. */
static void npu_reset_release(void)
{
    TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5RSTR) |=
        STM32N6_RCC_AHB5RSTR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5RSTR) &=
        ~STM32N6_RCC_AHB5RSTR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

/* CACHEAXI is outside the ATON block and is not initialized by LL-ATON. */
static void npu_cacheaxi_enable(void)
{
    TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5ENR) |=
        STM32N6_RCC_AHB5ENR_CACHEAXI;
    TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5LPENR) &=
        ~STM32N6_RCC_AHB5LPENR_CACHEAXI;
    TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5RSTR) |=
        STM32N6_RCC_AHB5RSTR_CACHEAXI;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5RSTR) &=
        ~STM32N6_RCC_AHB5RSTR_CACHEAXI;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(TIKU_STM32N6_CACHEAXI_NS_CR1) |= STM32N6_CACHEAXI_CR1_EN;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

/* RIFSC is secure-firmware-owned board policy, not an NS runtime register.
 * Secure boot must assign NPU control/access and the NPU IRQ to NS before
 * handing control to this image. Attempting the former secure writes here is
 * an immediate security fault on a genuinely non-secure image. */
static void npu_rif_configure(void)
{
    /* Deliberately empty. RIFSC is secure-only; the NS image cannot turn a
     * secure resource into an NS resource at runtime. */
}

/* The vector entry is retained as a thin bridge to ATON_STD_IRQHandler. */
void tiku_stm32n6_npu_end_of_epoch_isr(void)
{
    tiku_npu_llaton_irq_bridge();
}

static int extents_overlap(uintptr_t a_start, uintptr_t a_end,
                           uintptr_t b_start, uintptr_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

static int npu_extent_is_valid(void)
{
    const uintptr_t axisram_start = (uintptr_t)&__axisram_start;
    const uintptr_t sram_start = (uintptr_t)&__tier_sram_start;
    const uintptr_t sram_end = (uintptr_t)&__tier_sram_end;
    const uintptr_t npu_start = (uintptr_t)&__tier_npu_start;
    const uintptr_t npu_end = (uintptr_t)&__tier_npu_end;
    const uintptr_t nvm_start = (uintptr_t)&__uninit_start;
    const uintptr_t nvm_end = (uintptr_t)&__uninit_end;

    if (npu_start < axisram_start || npu_end <= npu_start ||
        (npu_end - npu_start) != (uintptr_t)TIKU_TIER_NPU_SIZE ||
        ((npu_start | npu_end) & 31U) != 0U) {
        return 0;
    }
    if (sram_end < sram_start ||
        extents_overlap(sram_start, sram_end, npu_start, npu_end) ||
        extents_overlap(nvm_start, nvm_end, npu_start, npu_end)) {
        return 0;
    }
    return 1;
}

uint32_t tiku_npu_clock_readback(void)
{
    return TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5ENR);
}

int tiku_npu_power_enabled(void)
{
    return tiku_stm32n6_sram_npu_powered();
}

int tiku_npu_init(void)
{
    tiku_mem_err_t attach_result;
    uint32_t clock_readback;

    if (npu_initialized) {
        return TIKU_NPU_INIT_ALREADY;
    }
    if (!npu_extent_is_valid()) {
        return TIKU_NPU_INIT_ERR_CONFIG;
    }
    if (!tiku_npu_power_enabled()) {
        return TIKU_NPU_INIT_ERR_POWER;
    }

    /* Outer RCC and security setup must precede LL_ATON_Init(). */
    TIKU_REG32(TIKU_STM32N6_NPU_RCC_NS_AHB5ENR) |=
        STM32N6_RCC_AHB5ENR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    clock_readback = tiku_npu_clock_readback();
    if ((clock_readback & STM32N6_RCC_AHB5ENR_NPU) == 0U) {
        return TIKU_NPU_INIT_ERR_CLOCK;
    }

    npu_reset_release();
    npu_cacheaxi_enable();
    npu_rif_configure();

    attach_result = tiku_tier_attach_npu(
        (void *)(uintptr_t)&__tier_npu_start,
        (tiku_mem_arch_size_t)TIKU_TIER_NPU_SIZE);
    if (attach_result != TIKU_MEM_OK) {
        return TIKU_NPU_INIT_ERR_ALLOC;
    }

    /* LL_ATON_RT_RuntimeInit() now owns ATON clocks and ATON interrupts. */
    if (tiku_npu_llaton_runtime_init() != TIKU_NPU_OK) {
        (void)tiku_tier_npu_reset();
        return TIKU_NPU_INIT_ERR_CONFIG;
    }

    npu_initialized = 1U;
    return TIKU_NPU_INIT_OK;
}
