/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tiku_region_hal.h - HAL interface for the memory region registry.
 *
 * Declares the arch-level region table accessor every port implements,
 * returning descriptors for memory available to generic checked APIs (SRAM,
 * NVM, peripherals, flash).  A port may intentionally omit a privately owned
 * or reserved window from the table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_REGION_HAL_H_
#define TIKU_REGION_HAL_H_

#include <stdint.h>
#include "hal/tiku_mem_hal.h"

/*---------------------------------------------------------------------------*/
/* FORWARD DECLARATION                                                       */
/*---------------------------------------------------------------------------*/

/* Full definition in tiku_mem.h */
struct tiku_mem_region;

/*---------------------------------------------------------------------------*/
/* REQUIRED PLATFORM FUNCTION                                                */
/*---------------------------------------------------------------------------*/

/**
 * @brief Return the platform's memory region table.
 *
 * Descriptors for the checked-access memory map, valid for the lifetime of
 * the system: a const table, or one the port fills in at run time.  Reserved
 * windows may be absent so tiku_region_contains() and tiku_region_claim()
 * reject generic callers for those addresses.  Each port implements it in
 * its arch directory (e.g. arch/msp430/tiku_region_arch.c).
 *
 * @param count  Output: number of entries in the returned table
 * @return Pointer to the platform's region descriptor array (const)
 */
const struct tiku_mem_region *tiku_region_arch_get_table(
    tiku_mem_arch_size_t *count);

#endif /* TIKU_REGION_HAL_H_ */
