/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_thread_arch.c - STM32F411RE worker-thread switcher shim
 *
 * The STM32F411RE's Cortex-M4F uses the shared Cortex-M PendSV/PSP
 * switcher.  The CMSIS startup table names the vector PendSV_Handler;
 * tiku_irq_bridge_stm32f411.c owns that strong vector symbol and forwards
 * it to this port-local hook.  Defining the hook here overrides the bridge's
 * weak declaration without changing startup assembly or IRQ routing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define TIKU_THREAD_ARCH_PENDSV  tiku_stm32f411_pendsv_handler
#include "kernel/threads/tiku_thread_cortexm.inl"
