/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 *
 * Author: Jeremy Goh
 *
 * tiku_npu_arch.h - STM32N6 board bring-up and LL-ATON IRQ bridge.
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

/**
 * @brief Enable the STM32N6 NPU domain and register its reserved tier.
 *
 * Applications call this after boot and before binding or loading a model.
 * LL-ATON owns the ATON runtime and model execution. A second call returns
 * TIKU_NPU_INIT_ALREADY.
 */
int tiku_npu_init(void);

/**
 * @brief Shut down the application-owned NPU runtime.
 *
 * The call is rejected while a model is bound, loaded, or running.  A
 * successful shutdown releases the NPU tier so a later application can call
 * tiku_npu_init() again in the same boot.
 */
int tiku_npu_shutdown(void);

/** @brief Live RCC readback captured/used by the bring-up check. */
uint32_t tiku_npu_clock_readback(void);

/** @brief Non-zero when all SRAM banks reserved for NPU memory are powered. */
int tiku_npu_power_enabled(void);

#endif /* TIKU_STM32N6_NPU_ARCH_H_ */
