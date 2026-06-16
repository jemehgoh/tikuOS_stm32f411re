/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_device_stm32f411re.h - STM32F411RE silicon-level constants
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_DEVICE_STM32F411RE_H_
#define TIKU_DEVICE_STM32F411RE_H_

#include <stdint.h>
#include <stm32f411xe.h>

/*---------------------------------------------------------------------------*/
/* DEVICE IDENTIFICATION                                                     */
/*---------------------------------------------------------------------------*/

#define TIKU_DEVICE_NAME            "STM32F411RE"

/*---------------------------------------------------------------------------*/
/* GPIO PORT AVAILABILITY                                                    */
/*---------------------------------------------------------------------------*/

/*
 * The STM32F411RE has five GPIO banks (Ports A..E) with up to 16 pins each.
 * The shell/VFS GPIO model is fixed at 8 pins per port, so the STM32 port
 * exposes A..D as eight virtual ports:
 *   P1 = PA0..PA7    P2 = PA8..PA15
 *   P3 = PB0..PB7    P4 = PB8..PB15
 *   P5 = PC0..PC7    P6 = PC8..PC15
 *   P7 = PD0..PD7    P8 = PD8..PD15
 *
 * Port E is not currently surfaced through the shell/VFS view.
 */
#define TIKU_DEVICE_HAS_PORT1       1
#define TIKU_DEVICE_HAS_PORT2       1
#define TIKU_DEVICE_HAS_PORT3       1
#define TIKU_DEVICE_HAS_PORT4       1
#define TIKU_DEVICE_HAS_PORT5       1
#define TIKU_DEVICE_HAS_PORT6       1
#define TIKU_DEVICE_HAS_PORT7       1
#define TIKU_DEVICE_HAS_PORT8       1
#define TIKU_DEVICE_HAS_PORT9       0
#define TIKU_DEVICE_HAS_PORTJ       0

#define TIKU_DEVICE_HAS_LFXT        0
#define TIKU_DEVICE_HAS_HFXT        1
#define TIKU_DEVICE_CS_HAS_KEY      0
#define TIKU_DEVICE_MAX_STABLE_MHZ  100

/*---------------------------------------------------------------------------*/
/* MEMORY SIZES                                                              */
/*---------------------------------------------------------------------------*/

/*
* 128KB of SRAM at 0x20000000.
*/
#define TIKU_DEVICE_RAM_SIZE        (128UL * 1024UL)
#define TIKU_DEVICE_RAM_START       SRAM_BASE

/*
* "FRAM" on the STM32F411RE refers to the on-chip flash (512 KB at 0x80000000).
*/
#define TIKU_DEVICE_FRAM_SIZE       (512UL * 1024UL)
#define TIKU_DEVICE_FRAM_START      FLASH_BASE
#define TIKU_DEVICE_FRAM_END        (FLASH_BASE + TIKU_DEVICE_FRAM_SIZE - 1UL)

/*
 * Internal-flash sector geometry (STM32F411RE, 512 KB total):
 *   S0..S3 = 16 KB each
 *   S4     = 64 KB
 *   S5..S7 = 128 KB each
 *
 * Sector 7 is reserved for the STM32 persistence backend so wear-
 * levelled write-back can erase/program it without touching code.
 */

#define TIKU_DEVICE_FLASH_SECTOR0_SIZE      (16UL * 1024UL)
#define TIKU_DEVICE_FLASH_SECTOR1_SIZE      (16UL * 1024UL)
#define TIKU_DEVICE_FLASH_SECTOR2_SIZE      (16UL * 1024UL)
#define TIKU_DEVICE_FLASH_SECTOR3_SIZE      (16UL * 1024UL)
#define TIKU_DEVICE_FLASH_SECTOR4_SIZE      (64UL * 1024UL)
#define TIKU_DEVICE_FLASH_SECTOR5_SIZE      (128UL * 1024UL)
#define TIKU_DEVICE_FLASH_SECTOR6_SIZE      (128UL * 1024UL)
#define TIKU_DEVICE_FLASH_SECTOR7_SIZE      (128UL * 1024UL)

#define TIKU_DEVICE_FLASH_APP_START         FLASH_BASE
#define TIKU_DEVICE_FLASH_APP_SIZE          \
    (TIKU_DEVICE_FRAM_SIZE - TIKU_DEVICE_FLASH_SECTOR7_SIZE)
#define TIKU_DEVICE_FLASH_APP_END           \
    (TIKU_DEVICE_FLASH_APP_START + TIKU_DEVICE_FLASH_APP_SIZE - 1UL)

#define TIKU_DEVICE_FLASH_PERSIST_SECTOR     7U
#define TIKU_DEVICE_FLASH_PERSIST_START      0x08060000UL
#define TIKU_DEVICE_FLASH_PERSIST_SIZE       TIKU_DEVICE_FLASH_SECTOR7_SIZE
#define TIKU_DEVICE_FLASH_PERSIST_END        \
    (TIKU_DEVICE_FLASH_PERSIST_START + TIKU_DEVICE_FLASH_PERSIST_SIZE - 1UL)

#define TIKU_DEVICE_FRAM_CONFIG_SIZE      576U
#define TIKU_DEVICE_FRAM_APP_SLOT_SIZE    4096U
#define TIKU_DEVICE_FRAM_APP_SLOT_COUNT   4

/*---------------------------------------------------------------------------*/
/* MPU                                                                       */
/*---------------------------------------------------------------------------*/

#define TIKU_DEVICE_HAS_MPU         1

#endif /* TIKU_DEVICE_STM32F411RE_H_ */
