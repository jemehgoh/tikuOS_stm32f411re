/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_cpu_freq_boot_arch.h - STM32F411RE CPU clock / boot interface
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32F411_CPU_FREQ_BOOT_ARCH_H_
#define TIKU_STM32F411_CPU_FREQ_BOOT_ARCH_H_

#include <stdint.h>

/*---------------------------------------------------------------------------*/
/* Required HAL entry points                                                 */
/*---------------------------------------------------------------------------*/

/**
 * @brief Prepare the Cortex-M4 and STM32F411 reset-default hardware state.
 *
 * Called once from tiku_cpu_boot_init(). This function leaves the core on
 * HSI and enables only the baseline clocks needed by the next boot stages.
 * PLL setup belongs to tiku_cpu_freq_stm32f411_init().
 */
void tiku_cpu_boot_stm32f411_init(void);

/**
 * @brief Configure the STM32F411 clock tree.
 *
 * @param target_mhz Requested SYSCLK/HCLK frequency in MHz. Supported first
 *                   targets are 16, 48, 84, and 100 MHz. Unsupported targets
 *                   select the safe default, 100 MHz, and set the fault flag.
 */
void tiku_cpu_freq_stm32f411_init(unsigned int target_mhz);

/**
 * @brief Idle entry point for the scheduler.
 *
 * Plain WFI idle primitive. The scheduler's light idle maps here directly.
 */
void tiku_cpu_boot_stm32f411_power_wfi_enter(void);

/**
 * @brief Adaptive STM32F411 idle entry point.
 *
 * Deep/deepest idle maps here. It enters STOP only when the timer backend has
 * requested an RTC-backed STOP sleep window; otherwise it falls back to WFI.
 */
void tiku_cpu_boot_stm32f411_idle_enter(void);

/**
 * @brief Mark the next adaptive idle window as STOP-eligible.
 */
void tiku_cpu_boot_stm32f411_stop_request(void);

/**
 * @brief Clear STOP eligibility for adaptive idle.
 */
void tiku_cpu_boot_stm32f411_stop_cancel(void);

/**
 * @brief Report whether adaptive idle attempted STOP in this window.
 */
int tiku_cpu_boot_stm32f411_stop_was_attempted(void);

/**
 * @brief Restore the configured SYSCLK tree after a STOP-mode wake.
 *
 * STM32F411 resumes from STOP on HSI with PLL disabled. The current STM32
 * idle hook is WFI-only, so this is normally a no-op; RTC-mediated timer wake
 * code calls it before timestamp-based resync so future STOP-mode idle paths
 * do not run the post-wake accounting on a degraded clock tree.
 *
 * @return Non-zero when the configured clock tree is available, zero if the
 *         port had to fall back to HSI and set the clock fault flag.
 */
int tiku_cpu_boot_stm32f411_post_stop_wake(void);

/**
 * @brief Request a Cortex-M system reset.
 */
void tiku_cpu_boot_stm32f411_reset(void);

/*---------------------------------------------------------------------------*/
/* Clock-rate queries                                                        */
/*---------------------------------------------------------------------------*/

unsigned long tiku_cpu_stm32f411_clock_get_hz(void);  /* HCLK / core clock */
unsigned long tiku_cpu_stm32f411_smclk_get_hz(void);  /* APB1 peripheral clock */
unsigned long tiku_cpu_stm32f411_aclk_get_hz(void);   /* LSI/LSE, if enabled */
unsigned long tiku_cpu_stm32f411_pclk1_get_hz(void);
unsigned long tiku_cpu_stm32f411_pclk2_get_hz(void);
int           tiku_cpu_stm32f411_clock_has_fault(void);

#endif /* TIKU_STM32F411_CPU_FREQ_BOOT_ARCH_H_ */
