/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 *
 * tiku_npu_arch.c - STM32N6 Neural-ART bring-up scaffolding.
 *
 * The NPU tier is a linker-owned tail of AXISRAM.  Keeping it out of the
 * general SRAM and NVM tiers prevents a future runtime from making model
 * workspace compete with OS allocations or persistent storage.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <kernel/memory/tiku_mem.h>

#include "tiku_npu_arch.h"
#include "tiku_sram_arch.h"
#include "tiku_stm32n6_regs.h"

/* Smallest generated Neural-ART epoch blob: header followed by a stop
 * opcode. It performs no stream-engine or model work and is kept in ordinary
 * image read-only storage, never in the reserved NPU tier. */
static const uint64_t npu_epoch_selftest_blob[8]
    __attribute__((aligned(8), used)) = {
    0x00000003ca057a7aULL,
    0x0000035d0000033dULL,
    0x000000000000000cULL,
};

typedef struct {
    volatile uint32_t control;
    volatile uint32_t version;
    volatile uint32_t address;
    volatile uint32_t interrupt;
} tiku_npu_epoch_controller_t;

extern uint8_t __axisram_start;
extern uint8_t __tier_sram_start;
extern uint8_t __tier_sram_end;
extern uint8_t __tier_npu_start;
extern uint8_t __tier_npu_end;
extern uint8_t __uninit_start;
extern uint8_t __uninit_end;

static volatile tiku_npu_epoch_controller_t *epoch_controller;
static uint32_t npu_clock_readback;
static uint8_t npu_initialized;
static volatile uint32_t npu_epoch_irq_count;
static volatile uint32_t npu_epoch_trigger_cycle;
static volatile uint32_t npu_epoch_last_latency_cycles;

#define STM32N6_NPU_IRQ_PRIORITY       1U
#define STM32N6_NPU_IRQ_TIMEOUT_CYCLES 150000U
#define STM32N6_NPU_IRQ_SETTLE_CYCLES  2000U

/* STM32N657 NPU power-up registers not yet represented by the compact local
 * register header. These are the register-level equivalents of ST's
 * NPU_Config() sequence. */
#define STM32N6_NPU_RCC_BASE           0x56028000UL
#define STM32N6_NPU_RCC_AHB5ENR       (STM32N6_NPU_RCC_BASE + 0x260U)
#define STM32N6_RCC_AHB5RSTR           (STM32N6_NPU_RCC_BASE + 0x220U)
#define STM32N6_RCC_AHB5RSTR_NPU       (1UL << 31)
#define STM32N6_RCC_AHB5ENR_CACHEAXI   (1UL << 30)
#define STM32N6_RCC_AHB5RSTR_CACHEAXI  (1UL << 30)
#define STM32N6_RCC_AHB5LPENR          (STM32N6_NPU_RCC_BASE + 0x2a0U)
#define STM32N6_RCC_AHB5LPENR_CACHEAXI (1UL << 30)
#define STM32N6_CACHEAXI_BASE          (STM32N6_NPU_BASE - 0x400UL)
#define STM32N6_CACHEAXI_CR1           (STM32N6_CACHEAXI_BASE + 0x00U)
#define STM32N6_CACHEAXI_CR1_EN        (1UL << 0)

/* Secure RIFSC: RISC register 3, NPU bit 10; RIMC master 1. */
#define STM32N6_RIFSC_BASE             0x54024000UL
#define STM32N6_RIFSC_RISC_SECCFGR3   (STM32N6_RIFSC_BASE + 0x01cU)
#define STM32N6_RIFSC_RISC_PRIVCFGR3  (STM32N6_RIFSC_BASE + 0x03cU)
#define STM32N6_RIFSC_RIMC_ATTR1      (STM32N6_RIFSC_BASE + 0xc14U)
#define STM32N6_RIFSC_NPU_BIT         (1UL << 10)
#define STM32N6_RIFSC_RIMC_ATTR_MCID  (7UL << 4)
#define STM32N6_RIFSC_RIMC_ATTR_SEC   (1UL << 8)
#define STM32N6_RIFSC_RIMC_ATTR_PRIV  (1UL << 9)
#define STM32N6_RIFSC_RIMC_NPU_CID1   (1UL << 4)

static uint32_t npu_cycles(void)
{
    return TIKU_REG32(STM32N6_DWT_CYCCNT);
}

static void npu_cycle_counter_enable(void)
{
    TIKU_REG32(STM32N6_SCB_DEMCR) |= STM32N6_SCB_DEMCR_TRCENA;
    TIKU_REG32(STM32N6_DWT_LAR) = STM32N6_DWT_LAR_KEY;
    TIKU_REG32(STM32N6_DWT_CTRL) |= STM32N6_DWT_CTRL_CYCCNTENA;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_internal_clock_enable(void)
{
    /* Mirror the ordering used by ST's LL_ATON_Init(): clear the ATON
     * pipeline, enable the clock controller, then open the accelerator and
     * block gates.  In particular, BGATES bit 25 clocks EPOCHCTRL0. */
    TIKU_REG32(STM32N6_ATON_CLKCTRL_CTRL) = STM32N6_ATON_CLKCTRL_CTRL_CLR;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");

    TIKU_REG32(STM32N6_ATON_CLKCTRL_CTRL) = STM32N6_ATON_CLKCTRL_CTRL_EN;
    TIKU_REG32(STM32N6_ATON_CLKCTRL_AGATES0) = 0xffffffffUL;
    TIKU_REG32(STM32N6_ATON_CLKCTRL_AGATES1) = 0xffffffffUL;
    TIKU_REG32(STM32N6_ATON_CLKCTRL_BGATES) = 0xffffffffUL;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_reset_release(void)
{
    /* The boot ROM leaves the NPU domain in reset on this SRAM-boot path.
     * Keep the outer RCC clock on while asserting and releasing reset, as the
     * STM32N6 reference configuration does. */
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) |= STM32N6_RCC_AHB5RSTR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) &= ~STM32N6_RCC_AHB5RSTR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_cacheaxi_enable(void)
{
    /* ST's NPU_Config() enables CACHEAXI, resets it, disables sleep gating,
     * and HAL_CACHEAXI_Init() finally sets CR1.EN. */
    TIKU_REG32(STM32N6_NPU_RCC_AHB5ENR) |= STM32N6_RCC_AHB5ENR_CACHEAXI;
    TIKU_REG32(STM32N6_RCC_AHB5LPENR) &= ~STM32N6_RCC_AHB5LPENR_CACHEAXI;
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) |= STM32N6_RCC_AHB5RSTR_CACHEAXI;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) &= ~STM32N6_RCC_AHB5RSTR_CACHEAXI;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(STM32N6_CACHEAXI_CR1) |= STM32N6_CACHEAXI_CR1_EN;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_rif_configure(void)
{
    /* Match ST's secure NPU_Validation configuration: NPU bus transactions
     * use trusted CID 1 and secure privileged attributes, while NPU control
     * registers remain secure privileged. */
    uint32_t attr = TIKU_REG32(STM32N6_RIFSC_RIMC_ATTR1);

    attr &= ~(STM32N6_RIFSC_RIMC_ATTR_MCID |
              STM32N6_RIFSC_RIMC_ATTR_SEC |
              STM32N6_RIFSC_RIMC_ATTR_PRIV);
    attr |= STM32N6_RIFSC_RIMC_NPU_CID1 |
            STM32N6_RIFSC_RIMC_ATTR_SEC |
            STM32N6_RIFSC_RIMC_ATTR_PRIV;
    TIKU_REG32(STM32N6_RIFSC_RIMC_ATTR1) = attr;

    TIKU_REG32(STM32N6_RIFSC_RISC_SECCFGR3) |= STM32N6_RIFSC_NPU_BIT;
    TIKU_REG32(STM32N6_RIFSC_RISC_PRIVCFGR3) |= STM32N6_RIFSC_NPU_BIT;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

void tiku_stm32n6_npu_end_of_epoch_isr(void)
{
    uint32_t status = TIKU_REG32(STM32N6_EPOCHCTRL_IRQ);

    if ((status & STM32N6_EPOCHCTRL_IRQ_DONE) != 0U) {
        npu_epoch_last_latency_cycles = npu_cycles() - npu_epoch_trigger_cycle;
        npu_epoch_irq_count++;
    }

    /* Neural-ART source first, then its interrupt-controller latch. */
    if (status != 0U) {
        TIKU_REG32(STM32N6_EPOCHCTRL_IRQ) = status;
    }
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTCLR) = STM32N6_NPU_INTCTRL_EPOCH0_INT;
    __asm__ volatile ("dsb" ::: "memory");
}

static void npu_epoch_ack(void)
{
    uint32_t status = TIKU_REG32(STM32N6_EPOCHCTRL_IRQ);

    if (status != 0U) {
        TIKU_REG32(STM32N6_EPOCHCTRL_IRQ) = status;
    }
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTCLR) = STM32N6_NPU_INTCTRL_EPOCH0_INT;
    TIKU_REG32(STM32N6_NVIC_ICPR(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static int npu_epoch_reset(void)
{
    uint32_t start = npu_cycles();

    TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_CLR;
    while ((TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) & STM32N6_EPOCHCTRL_CTRL_CLR) != 0U) {
        if ((uint32_t)(npu_cycles() - start) > STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
            return TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
        }
    }
    TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_CONFCLR;
    while ((TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) & STM32N6_EPOCHCTRL_CTRL_CONFCLR) != 0U) {
        if ((uint32_t)(npu_cycles() - start) > STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
            return TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
        }
    }
    TIKU_REG32(STM32N6_EPOCHCTRL_ADDR) =
        (uint32_t)(uintptr_t)npu_epoch_selftest_blob;
    TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = 0U;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    return TIKU_NPU_EPOCH_TEST_OK;
}

static int npu_epoch_wait_stopped(uint32_t *stopped)
{
    uint32_t start = npu_cycles();

    *stopped = 0U;
    while ((TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) & STM32N6_EPOCHCTRL_CTRL_RUNNING) != 0U) {
        if ((uint32_t)(npu_cycles() - start) > STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
            return TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
        }
    }
    *stopped = 1U;
    return TIKU_NPU_EPOCH_TEST_OK;
}

static void npu_epoch_irq_configure(void)
{
    npu_cycle_counter_enable();
    TIKU_REG32(STM32N6_NPU_INTCTRL_CTRL) = STM32N6_NPU_INTCTRL_CTRL_CLR;
    /* INTORMSK bits are masks: unmask only EPOCHCTRL0 completion. */
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTORMSK0) =
        ~STM32N6_NPU_INTCTRL_EPOCH0_INT;
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTANDMSK0) = 0xffffffffUL;
    TIKU_REG32(STM32N6_NPU_INTCTRL_CTRL) = STM32N6_NPU_INTCTRL_CTRL_EN;
    /* STM32N6 CMSIS defines four implemented priority bits. */
    TIKU_REG8(STM32N6_NVIC_IPR(STM32N6_IRQ_NPU_END_OF_EPOCH)) =
        (uint8_t)(STM32N6_NPU_IRQ_PRIORITY << 4);
    TIKU_REG32(STM32N6_NVIC_ICPR(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    TIKU_REG32(STM32N6_NVIC_ISER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
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
    return TIKU_REG32(STM32N6_NPU_RCC_AHB5ENR);
}

int tiku_npu_power_enabled(void)
{
    return tiku_stm32n6_sram_npu_powered();
}

uintptr_t tiku_npu_epoch_controller_base(void)
{
    return (uintptr_t)epoch_controller;
}

int tiku_npu_init(void)
{
    tiku_mem_err_t attach_result;

    if (npu_initialized) {
        return TIKU_NPU_INIT_ALREADY;
    }

    /* The linker and this runtime check are intentionally redundant: the
     * latter catches a mismatched object/linker configuration before the
     * extent can be handed to the allocator. */
    if (!npu_extent_is_valid()) {
        return TIKU_NPU_INIT_ERR_CONFIG;
    }

    /* AXISRAM3..6 are the NPU memory banks.  tiku_sram_init() powers them up
     * before tiku_mem_init(); this is the boot-time power-domain readback. */
    if (!tiku_npu_power_enabled()) {
        return TIKU_NPU_INIT_ERR_POWER;
    }

    /* AHB5ENR.NPU is the Neural-ART/epoch-controller clock gate. */
    TIKU_REG32(STM32N6_NPU_RCC_AHB5ENR) |= STM32N6_RCC_AHB5ENR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    npu_clock_readback = tiku_npu_clock_readback();
    if ((npu_clock_readback & STM32N6_RCC_AHB5ENR_NPU) == 0U) {
        return TIKU_NPU_INIT_ERR_CLOCK;
    }

    npu_reset_release();
    npu_cacheaxi_enable();
    npu_rif_configure();
    npu_internal_clock_enable();

    /* On this memory-mapped target, mapping establishes the typed view of the
     * fixed register window. */
    epoch_controller = (volatile tiku_npu_epoch_controller_t *)(uintptr_t)
        STM32N6_EPOCHCTRL_BASE;
    (void)epoch_controller->control;
    (void)epoch_controller->version;

    attach_result = tiku_tier_attach_npu(
        (void *)(uintptr_t)&__tier_npu_start,
        (tiku_mem_arch_size_t)TIKU_TIER_NPU_SIZE);
    if (attach_result != TIKU_MEM_OK) {
        return TIKU_NPU_INIT_ERR_ALLOC;
    }

    npu_epoch_irq_configure();

    npu_initialized = 1U;
    return TIKU_NPU_INIT_OK;
}

int tiku_npu_epoch_irq_selftest(uint32_t iterations,
                                tiku_npu_epoch_test_result_t *result)
{
    int rc = TIKU_NPU_EPOCH_TEST_OK;

    if (result == NULL || iterations == 0U) {
        return TIKU_NPU_EPOCH_TEST_ERR_ARG;
    }
    if (!npu_initialized || epoch_controller == NULL) {
        return TIKU_NPU_EPOCH_TEST_ERR_INIT;
    }

    *result = (tiku_npu_epoch_test_result_t){0};
    result->requested = iterations;
    result->timeout_cycles = STM32N6_NPU_IRQ_TIMEOUT_CYCLES;
    result->nvic_priority = STM32N6_NPU_IRQ_PRIORITY;
    result->latency_min_cycles = 0xffffffffUL;

    npu_cycle_counter_enable();
    npu_epoch_ack();

    for (uint32_t i = 0U; i < iterations; i++) {
        uint32_t before;
        uint32_t start;
        uint32_t stopped;

        npu_epoch_ack();
        rc = npu_epoch_reset();
        if (rc != TIKU_NPU_EPOCH_TEST_OK) {
            break;
        }

        before = npu_epoch_irq_count;
        start = npu_cycles();
        npu_epoch_trigger_cycle = start;
        TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_EN;
        __asm__ volatile ("dsb" ::: "memory");

        rc = npu_epoch_wait_stopped(&stopped);
        (void)stopped;
        if (rc != TIKU_NPU_EPOCH_TEST_OK) {
            break;
        }
        while (npu_epoch_irq_count < before + 1U) {
            if ((uint32_t)(npu_cycles() - start) >
                STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
                rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
                break;
            }
        }
        if (rc != TIKU_NPU_EPOCH_TEST_OK) {
            break;
        }
        {
            uint32_t quiet_start = npu_cycles();
            while ((uint32_t)(npu_cycles() - quiet_start) <
                   STM32N6_NPU_IRQ_SETTLE_CYCLES) {
                if (npu_epoch_irq_count != before + 1U) {
                    break;
                }
            }
        }
        if (npu_epoch_irq_count != before + 1U) {
            result->extra++;
        }
        result->completed++;
        if (npu_epoch_last_latency_cycles < result->latency_min_cycles) {
            result->latency_min_cycles = npu_epoch_last_latency_cycles;
        }
        if (npu_epoch_last_latency_cycles > result->latency_max_cycles) {
            result->latency_max_cycles = npu_epoch_last_latency_cycles;
        }
        result->latency_total_cycles += npu_epoch_last_latency_cycles;
    }

    result->missed = iterations - result->completed;
    if (result->completed != iterations || result->extra != 0U) {
        rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
    }

    /* Negative path: the epoch must execute and stop, but the disabled NVIC
     * line must not change the ISR counter. BC is only an execution check;
     * completion is observed exclusively through the ISR counter. */
    TIKU_REG32(STM32N6_NVIC_ICER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    result->negative_before = npu_epoch_irq_count;
    if (npu_epoch_reset() == TIKU_NPU_EPOCH_TEST_OK) {
        uint32_t negative_bc_before = TIKU_REG32(STM32N6_EPOCHCTRL_BC);
        TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_EN;
        __asm__ volatile ("dsb" ::: "memory");
        uint32_t stopped = 0U;
        if (npu_epoch_wait_stopped(&stopped) == TIKU_NPU_EPOCH_TEST_OK) {
            result->negative_epoch_ran =
                (stopped != 0U &&
                 TIKU_REG32(STM32N6_EPOCHCTRL_BC) != negative_bc_before);
        }
    } else {
        rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
    }
    result->negative_after = npu_epoch_irq_count;
    if (result->negative_after != result->negative_before ||
        result->negative_epoch_ran == 0U) {
        result->extra++;
        rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
    }
    npu_epoch_ack();
    TIKU_REG32(STM32N6_NVIC_ISER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");

    return rc;
}
