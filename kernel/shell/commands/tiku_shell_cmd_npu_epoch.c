/*
 * tiku_shell_cmd_npu_epoch.c - STM32N6 epoch-controller IRQ self-test.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <arch/stm32n6/tiku_cpu_freq_boot_arch.h>
#include <arch/stm32n6/tiku_npu_arch.h>

#include <kernel/shell/tiku_shell.h>
#include "tiku_shell_cmd_npu_epoch.h"

void tiku_shell_cmd_npu_epoch(uint8_t argc, const char *argv[])
{
    tiku_npu_epoch_test_result_t result;
    unsigned long iterations = 100UL;
    unsigned long hz;
    unsigned long long average_cycles;
    int rc;

    if (argc > 1U) {
        iterations = strtoul(argv[1], NULL, 0);
    }
    if (iterations == 0UL || iterations > 10000UL) {
        SHELL_PRINTF("Usage: npu-irq-test [1..10000]\n");
        return;
    }

    rc = tiku_npu_epoch_irq_selftest((uint32_t)iterations, &result);
    hz = tiku_cpu_stm32n6_clock_get_hz();
    average_cycles = result.completed != 0U
        ? (unsigned long long)(result.latency_total_cycles / result.completed)
        : 0ULL;

    SHELL_PRINTF("npu-irq-test: rc=%d requested=%lu completed=%lu missed=%lu extra=%lu\n",
                 rc, iterations, (unsigned long)result.completed,
                 (unsigned long)result.missed, (unsigned long)result.extra);
    SHELL_PRINTF("  latency cycles min=%lu avg=%llu max=%lu (~%lu/%lu/%lu us @ %lu Hz)\n",
                 (unsigned long)result.latency_min_cycles,
                 average_cycles,
                 (unsigned long)result.latency_max_cycles,
                 (unsigned long)(((unsigned long long)result.latency_min_cycles * 1000000ULL) / hz),
                 (unsigned long)((average_cycles * 1000000ULL) / hz),
                 (unsigned long)(((unsigned long long)result.latency_max_cycles * 1000000ULL) / hz),
                 hz);
    SHELL_PRINTF("  negative IRQ-disabled: epoch_ran=%lu counter=%lu->%lu %s\n",
                 (unsigned long)result.negative_epoch_ran,
                 (unsigned long)result.negative_before,
                 (unsigned long)result.negative_after,
                 (result.negative_before == result.negative_after &&
                 result.negative_epoch_ran != 0U) ? "OK" : "FAIL");
    SHELL_PRINTF("  NVIC NPU0 priority=%u (LPTIM/GPDMA default 0; PendSV worker absent here)\n",
                 (unsigned)result.nvic_priority);
}
