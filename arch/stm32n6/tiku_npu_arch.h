/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 *
 * Author: Jeremy Goh
 *
 * tiku_npu_arch.h - STM32N6 Neural-ART fixed embedded-model backend.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32N6_NPU_ARCH_H_
#define TIKU_STM32N6_NPU_ARCH_H_

#include <stdint.h>

/* Init is deliberately status-returning so a duplicate call is observable. */
#define TIKU_NPU_INIT_OK             0
#define TIKU_NPU_INIT_ALREADY        1
#define TIKU_NPU_INIT_ERR_CONFIG    -1
#define TIKU_NPU_INIT_ERR_POWER     -2
#define TIKU_NPU_INIT_ERR_CLOCK     -3
#define TIKU_NPU_INIT_ERR_ALLOC     -4

#define TIKU_NPU_EPOCH_TEST_OK          0
#define TIKU_NPU_EPOCH_TEST_ERR_ARG    -1
#define TIKU_NPU_EPOCH_TEST_ERR_INIT   -2
#define TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT -3

typedef struct {
    uint32_t requested;
    uint32_t completed;
    uint32_t missed;
    uint32_t extra;
    uint32_t negative_before;
    uint32_t negative_after;
    uint32_t negative_epoch_ran;
    uint32_t latency_min_cycles;
    uint32_t latency_max_cycles;
    uint64_t latency_total_cycles;
    uint32_t timeout_cycles;
    uint8_t  nvic_priority;
} tiku_npu_epoch_test_result_t;

/**
 * @brief Enable the STM32N6 NPU domain and register its reserved tier.
 *
 * This enables the NPU domain and installs the isolated epoch-completion IRQ
 * path. Models are submitted through interfaces/npu/tiku_npu.h. A second
 * call returns TIKU_NPU_INIT_ALREADY.
 */
int tiku_npu_init(void);

/** @brief Live RCC readback captured/used by the bring-up check. */
uint32_t tiku_npu_clock_readback(void);

/** @brief Non-zero when all SRAM banks reserved for NPU memory are powered. */
int tiku_npu_power_enabled(void);

/** @brief Epoch-controller register window mapped by tiku_npu_init(). */
uintptr_t tiku_npu_epoch_controller_base(void);

/** @brief Run completion-IRQ-only self-test; no process event is posted. */
int tiku_npu_epoch_irq_selftest(uint32_t iterations,
                                tiku_npu_epoch_test_result_t *result);

#endif /* TIKU_STM32N6_NPU_ARCH_H_ */
