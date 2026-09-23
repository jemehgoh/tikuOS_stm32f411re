/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_ospi_arch.c - STM32N6 MX25UM51245G over XSPI2.
 *
 * Initialization, indirect DTR transfers, and DTR memory-mapped access live
 * here. Indirect commands abort the mapped mode before starting a transfer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include "tiku_ospi_arch.h"
#include "tiku_cache_arch.h"
#include "tiku_gpio_arch.h"
#include "tiku_stm32n6_regs.h"

/* IC3 carries the XSPI kernel clock. PLL1 runs at 1200 MHz, so a divider of 24
 * gives the conservative 50 MHz kernel clock used during bring-up. */
#define OSPI_IC_INDEX             3U
#define OSPI_IC_DIVIDER           24U
#define OSPI_KERNEL_CLOCK_HZ      50000000UL

/* The hardware prescaler encodes divide-by-(value + 1). */
#define OSPI_XSPI_PRESCALER       0U

/* Bounded so a wedged controller or flash costs an error rather than hanging
 * the caller. */
#define OSPI_SPINS                2000000UL
#define OSPI_RESET_SPINS          100000UL
#define OSPI_ERASE_SPINS          200000000UL

/* SPI command set. */
#define OSPI_CMD_SPI_RESET_ENABLE 0x66U
#define OSPI_CMD_SPI_RESET        0x99U
#define OSPI_CMD_SPI_READ_ID      0x9FU
#define OSPI_CMD_SPI_READ_STATUS  0x05U
#define OSPI_CMD_SPI_WRITE_ENABLE 0x06U
#define OSPI_CMD_SPI_WRITE_CR2    0x72U

/* OPI command set from ST's MX25UM51245G component. */
#define OSPI_CMD_OPI_RESET_ENABLE 0x6699U
#define OSPI_CMD_OPI_RESET        0x9966U
#define OSPI_CMD_OPI_READ_ID      0x9F60U
#define OSPI_CMD_OPI_READ_STATUS  0x05FAU
#define OSPI_CMD_OPI_WRITE_ENABLE 0x06F9U
#define OSPI_CMD_OPI_READ_STR     0xEC13U
#define OSPI_CMD_OPI_READ_DTR     0xEE11U
#define OSPI_CMD_OPI_PAGE_PROGRAM 0x12EDU
#define OSPI_CMD_OPI_SECTOR_ERASE 0x21DEU
#define OSPI_CMD_OPI_READ_CR2     0x718EU
#define OSPI_CMD_OPI_WRITE_CR2    0x728DU

#define OSPI_CR2_ADDRESS          0x00000000UL
#define OSPI_CR2_DOPI             0x02U
#define OSPI_STATUS_WIP           0x01U
#define OSPI_STATUS_WEL           0x02U

/* ST's MX25UM51245G command timings. */
#define OSPI_STR_READ_DUMMY_CYCLES 6U
#define OSPI_STR_REG_DUMMY_CYCLES  4U
#define OSPI_DTR_READ_DUMMY_CYCLES 6U
#define OSPI_DTR_REG_DUMMY_CYCLES 4U
#define OSPI_WRITE_DUMMY_CYCLES    0U

_Static_assert(OSPI_CMD_SPI_READ_ID == 0x9FU, "SPI JEDEC ID command");
_Static_assert(OSPI_CMD_SPI_WRITE_CR2 == 0x72U, "SPI CR2 write command");
_Static_assert(OSPI_CMD_OPI_READ_ID == 0x9F60U, "OPI DTR JEDEC ID command");
_Static_assert(OSPI_CMD_OPI_READ_STR == 0xEC13U, "OPI STR read command");
_Static_assert(OSPI_CMD_OPI_READ_STATUS == 0x05FAU,
               "OPI status command");
_Static_assert(OSPI_CMD_OPI_WRITE_ENABLE == 0x06F9U,
               "OPI write-enable command");
_Static_assert(OSPI_CMD_OPI_READ_DTR == 0xEE11U,
               "OPI DTR read command");
_Static_assert(OSPI_CMD_OPI_PAGE_PROGRAM == 0x12EDU,
               "OPI page-program command");
_Static_assert(OSPI_CMD_OPI_SECTOR_ERASE == 0x21DEU,
               "OPI sector-erase command");
_Static_assert(OSPI_CMD_OPI_READ_CR2 == 0x718EU, "OPI DTR CR2 read command");
_Static_assert(OSPI_CMD_OPI_WRITE_CR2 == 0x728DU,
               "OPI DTR CR2 write command");
_Static_assert(OSPI_CR2_DOPI == 0x02U, "MX25UM51245G DOPI bit");
_Static_assert(OSPI_KERNEL_CLOCK_HZ == 50000000UL, "OSPI kernel clock");
_Static_assert(OSPI_DTR_READ_DUMMY_CYCLES == 6U,
               "DTR array-read dummy cycles");
_Static_assert(OSPI_DTR_REG_DUMMY_CYCLES == 4U,
               "DTR register-read dummy cycles");
_Static_assert(OSPI_STR_READ_DUMMY_CYCLES == 6U,
               "STR array-read dummy cycles");
_Static_assert(OSPI_STR_REG_DUMMY_CYCLES == 4U,
               "STR register-read dummy cycles");
_Static_assert(OSPI_WRITE_DUMMY_CYCLES == 0U,
               "write-command dummy cycles");
_Static_assert(STM32N6_XSPI_CCR_IMODE_8L == (4UL << 0),
               "XSPI 8-line instruction mode");
_Static_assert(STM32N6_XSPI_CCR_ADMODE_8L == (4UL << 8),
               "XSPI 8-line address mode");
_Static_assert(STM32N6_XSPI_CCR_DMODE_8L == (4UL << 24),
               "XSPI 8-line data mode");
_Static_assert(STM32N6_XSPI_CCR_IDTR == (1UL << 3),
               "XSPI instruction DTR");
_Static_assert(STM32N6_XSPI_CCR_ADDTR == (1UL << 11),
               "XSPI address DTR");
_Static_assert(STM32N6_XSPI_CCR_DDTR == (1UL << 27),
               "XSPI data DTR");
_Static_assert(STM32N6_XSPI_CCR_DQSE == (1UL << 29),
               "XSPI DQS enable");
_Static_assert((TIKU_OSPI_PAGE_SIZE & 1U) == 0U,
               "DTR page size must be even");
_Static_assert((TIKU_OSPI_SIZE_BYTES & 1UL) == 0UL,
               "DTR flash size must be even");

typedef enum {
    OSPI_PROTOCOL_SPI,
    OSPI_PROTOCOL_OPI_STR,
    OSPI_PROTOCOL_OPI_DTR,
} ospi_protocol_t;

typedef struct {
    ospi_protocol_t protocol;
    uint8_t instruction_16;
    uint8_t instruction_dtr;
    uint8_t address_dtr;
    uint8_t data_dtr;
    uint8_t dqs;
    uint8_t use_address;
    uint8_t use_data;
    uint8_t write;
    uint32_t instruction;
    uint32_t address;
    void *buf;
    uint32_t len;
    uint8_t dummy_cycles;
} ospi_command_t;

static uint8_t ospi_ready;
static uint8_t ospi_mmap;
static unsigned long ospi_clock_hz_value;

static tiku_ospi_err_t ospi_mmap_disable_internal(void);

static int ospi_wait(uint32_t reg, uint32_t mask, int want_set,
                     unsigned long spins) {
    while (spins-- > 0UL) {
        uint32_t value = TIKU_REG32(reg) & mask;
        if (want_set ? (value != 0UL) : (value == 0UL)) {
            return 1;
        }
    }
    return 0;
}

static void ospi_delay(unsigned long spins) {
    while (spins-- > 0UL) {
        __asm__ volatile ("nop");
    }
}

static void ospi_finish(void) {
    TIKU_REG32(STM32N6_XSPI_FCR) = STM32N6_XSPI_FCR_ALL;
}

static int ospi_wait_fifo(unsigned long spins) {
    while (spins-- > 0UL) {
        uint32_t status = TIKU_REG32(STM32N6_XSPI_SR);
        if ((status & STM32N6_XSPI_SR_TEF) != 0UL) {
            return -1;
        }
        if ((status & (STM32N6_XSPI_SR_FTF | STM32N6_XSPI_SR_TCF)) != 0UL) {
            return 1;
        }
    }
    return 0;
}

static uint32_t ospi_command_ccr(const ospi_command_t *command) {
    uint32_t ccr = 0UL;
    uint32_t line_mode = (command->protocol == OSPI_PROTOCOL_SPI) ?
                         STM32N6_XSPI_CCR_IMODE_1L :
                         STM32N6_XSPI_CCR_IMODE_8L;

    ccr |= line_mode;
    if (command->instruction_16) {
        ccr |= STM32N6_XSPI_CCR_ISIZE_16;
    }
    if (command->instruction_dtr) {
        ccr |= STM32N6_XSPI_CCR_IDTR;
    }

    if (command->use_address) {
        ccr |= (command->protocol == OSPI_PROTOCOL_SPI) ?
               STM32N6_XSPI_CCR_ADMODE_1L :
               STM32N6_XSPI_CCR_ADMODE_8L;
        ccr |= STM32N6_XSPI_CCR_ADSIZE_32;
        if (command->address_dtr) {
            ccr |= STM32N6_XSPI_CCR_ADDTR;
        }
    }
    if (command->use_data) {
        ccr |= (command->protocol == OSPI_PROTOCOL_SPI) ?
               STM32N6_XSPI_CCR_DMODE_1L :
               STM32N6_XSPI_CCR_DMODE_8L;
        if (command->data_dtr) {
            ccr |= STM32N6_XSPI_CCR_DDTR;
        }
        if (command->dqs) {
            ccr |= STM32N6_XSPI_CCR_DQSE;
        }
    }
    return ccr;
}

/* Run one indirect command for SPI, OPI STR, or OPI DTR. */
static tiku_ospi_err_t ospi_command(const ospi_command_t *command) {
    if (command == NULL || (command->len != 0U && command->buf == NULL)) {
        return TIKU_OSPI_ERR_ARG;
    }
    if (ospi_mmap) {
        tiku_ospi_err_t rc = ospi_mmap_disable_internal();
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
    }
    if (!ospi_wait(STM32N6_XSPI_SR, STM32N6_XSPI_SR_BUSY, 0, OSPI_SPINS)) {
        return TIKU_OSPI_ERR_TIMEOUT;
    }
    ospi_finish();

    uint32_t cr = TIKU_REG32(STM32N6_XSPI_CR);
    cr &= ~STM32N6_XSPI_CR_FMODE_MSK;
    cr |= (command->write ? STM32N6_XSPI_FMODE_WRITE
                          : STM32N6_XSPI_FMODE_READ)
           << STM32N6_XSPI_CR_FMODE_POS;
    TIKU_REG32(STM32N6_XSPI_CR) = cr;

    if (command->len != 0U) {
        TIKU_REG32(STM32N6_XSPI_DLR) = command->len - 1UL;
    }
    TIKU_REG32(STM32N6_XSPI_CCR) = ospi_command_ccr(command);
    TIKU_REG32(STM32N6_XSPI_TCR) =
        ((uint32_t)command->dummy_cycles << STM32N6_XSPI_TCR_DCYC_POS) &
        STM32N6_XSPI_TCR_DCYC_MSK;
    TIKU_REG32(STM32N6_XSPI_IR) = command->instruction;

    if (command->use_address) {
        TIKU_REG32(STM32N6_XSPI_AR) = command->address;
    }

    uint8_t *read_buf = (uint8_t *)command->buf;
    const uint8_t *write_buf = (const uint8_t *)command->buf;

    for (uint32_t i = 0U; i < command->len; i++) {
        if (ospi_wait_fifo(OSPI_SPINS) <= 0) {
            ospi_finish();
            return TIKU_OSPI_ERR_TIMEOUT;
        }

        if (command->write) {
            TIKU_REG8(STM32N6_XSPI_DR) = write_buf[i];
        } else {
            read_buf[i] = TIKU_REG8(STM32N6_XSPI_DR);
        }
    }

    // Flush FIFO - to allow for reads with an odd number of values
    uint8_t placeholder;
    while ((TIKU_REG32(STM32N6_XSPI_SR) & (127UL << 8)) > 0) {
        placeholder = TIKU_REG8(STM32N6_XSPI_DR);
    }

    if (!ospi_wait(STM32N6_XSPI_SR, STM32N6_XSPI_SR_TCF, 1,
                   OSPI_SPINS)) {
        ospi_finish();
        return TIKU_OSPI_ERR_TIMEOUT;
    }
    if (!ospi_wait(STM32N6_XSPI_SR, STM32N6_XSPI_SR_BUSY, 0,
               OSPI_SPINS)) {
        ospi_finish();
        return TIKU_OSPI_ERR_TIMEOUT;
    }
    if ((TIKU_REG32(STM32N6_XSPI_SR) & STM32N6_XSPI_SR_TEF) != 0UL) {
        ospi_finish();
        return TIKU_OSPI_ERR_TIMEOUT;
    }
    ospi_finish();
    return TIKU_OSPI_OK;
}

static tiku_ospi_err_t ospi_spi_command(uint32_t instruction,
                                        uint32_t address,
                                        uint8_t use_address,
                                        void *buf, uint32_t len,
                                        uint8_t write) {
    const ospi_command_t command = {
        .protocol = OSPI_PROTOCOL_SPI,
        .instruction_16 = 0U,
        .instruction_dtr = 0U,
        .address_dtr = 0U,
        .data_dtr = 0U,
        .dqs = 0U,
        .use_address = use_address,
        .use_data = (len != 0U),
        .write = write,
        .instruction = instruction,
        .address = address,
        .buf = buf,
        .len = len,
        .dummy_cycles = 0U,
    };
    return ospi_command(&command);
}

static inline tiku_ospi_err_t ospi_opi_str_command(uint32_t instruction,
                                                   uint32_t address,
                                                   uint8_t use_address,
                                                   void *buf, uint32_t len,
                                                   uint8_t write,
                                                   uint8_t dummy_cycles) {
    const ospi_command_t command = {
        .protocol = OSPI_PROTOCOL_OPI_STR,
        .instruction_16 = 1U,
        .instruction_dtr = 0U,
        .address_dtr = 0U,
        .data_dtr = 0U,
        .dqs = 0U,
        .use_address = use_address,
        .use_data = (len != 0U),
        .write = write,
        .instruction = instruction,
        .address = address,
        .buf = buf,
        .len = len,
        .dummy_cycles = dummy_cycles,
    };
    return ospi_command(&command);
}

static tiku_ospi_err_t ospi_opi_dtr_command(uint32_t instruction,
                                            uint32_t address,
                                            uint8_t use_address,
                                            void *buf, uint32_t len,
                                            uint8_t write,
                                            uint8_t dqs,
                                            uint8_t dummy_cycles) {
    const ospi_command_t command = {
        .protocol = OSPI_PROTOCOL_OPI_DTR,
        .instruction_16 = 1U,
        .instruction_dtr = 1U,
        .address_dtr = use_address,
        .data_dtr = (len != 0U),
        .dqs = dqs,
        .use_address = use_address,
        .use_data = (len != 0U),
        .write = write,
        .instruction = instruction,
        .address = address,
        .buf = buf,
        .len = len,
        .dummy_cycles = dummy_cycles,
    };
    return ospi_command(&command);
}

static tiku_ospi_err_t ospi_read_spi_id(uint8_t id[3]) {
    return ospi_spi_command(OSPI_CMD_SPI_READ_ID, 0UL, 0U, id, 3U, 0U);
}

static tiku_ospi_err_t ospi_read_opi_dtr_id(uint8_t id[3]) {
    uint8_t opi_id[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
    tiku_ospi_err_t rc = ospi_opi_dtr_command(OSPI_CMD_OPI_READ_ID, 0UL, 1U, opi_id, 6U,
                                0U, 1U, OSPI_DTR_REG_DUMMY_CYCLES);
    id[0] = opi_id[1];
    id[1] = opi_id[3];
    id[2] = opi_id[5];

    return rc;
}

static int ospi_id_matches(const uint8_t id[3]) {
    return id[0] == TIKU_OSPI_MFR_MACRONIX &&
           id[1] == TIKU_OSPI_TYPE_MX25UM &&
           id[2] == TIKU_OSPI_CAPACITY_512M;
}

static tiku_ospi_err_t ospi_read_spi_status(uint8_t *status) {
    return ospi_spi_command(OSPI_CMD_SPI_READ_STATUS, 0UL, 0U, status, 1U,
                            0U);
}

static tiku_ospi_err_t ospi_wait_spi_ready(unsigned long spins) {
    while (spins-- > 0UL) {
        uint8_t status = 0U;
        tiku_ospi_err_t rc = ospi_read_spi_status(&status);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        if ((status & OSPI_STATUS_WIP) == 0U) {
            return TIKU_OSPI_OK;
        }
    }
    return TIKU_OSPI_ERR_TIMEOUT;
}

static tiku_ospi_err_t ospi_read_opi_dtr_status(uint8_t *status) {
    uint8_t value[2] = { 0U, 0U };
    tiku_ospi_err_t rc = ospi_opi_dtr_command(OSPI_CMD_OPI_READ_STATUS,
                                              0UL, 1U, value, sizeof(value),
                                              0U, 1U,
                                              OSPI_DTR_REG_DUMMY_CYCLES);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    if (value[0] != value[1]) {
        return TIKU_OSPI_ERR_STATE;
    }
    *status = value[0];
    return TIKU_OSPI_OK;
}

static tiku_ospi_err_t ospi_wait_opi_ready(unsigned long spins) {
    while (spins-- > 0UL) {
        uint8_t status = 0U;
        tiku_ospi_err_t rc = ospi_read_opi_dtr_status(&status);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        if ((status & OSPI_STATUS_WIP) == 0U) {
            return TIKU_OSPI_OK;
        }
    }
    return TIKU_OSPI_ERR_TIMEOUT;
}

static tiku_ospi_err_t ospi_write_enable_opi(void) {
    tiku_ospi_err_t rc = ospi_opi_dtr_command(OSPI_CMD_OPI_WRITE_ENABLE,
                                              0UL, 0U, NULL, 0U, 1U, 0U,
                                              OSPI_WRITE_DUMMY_CYCLES);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }

    uint8_t status = 0U;
    rc = ospi_read_opi_dtr_status(&status);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    return (status & OSPI_STATUS_WEL) != 0U
               ? TIKU_OSPI_OK
               : TIKU_OSPI_ERR_PROGRAM;
}

static tiku_ospi_err_t ospi_spi_reset(void) {
    tiku_ospi_err_t rc = ospi_spi_command(OSPI_CMD_SPI_RESET_ENABLE, 0UL,
                                          0U, NULL, 0U, 1U);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    rc = ospi_spi_command(OSPI_CMD_SPI_RESET, 0UL, 0U, NULL, 0U, 1U);
    if (rc == TIKU_OSPI_OK) {
        ospi_delay(OSPI_RESET_SPINS);
    }
    return rc;
}

static tiku_ospi_err_t ospi_opi_dtr_reset(void) {
    tiku_ospi_err_t rc = ospi_opi_dtr_command(OSPI_CMD_OPI_RESET_ENABLE,
                                              0UL, 0U, NULL, 0U, 1U, 0U,
                                              OSPI_WRITE_DUMMY_CYCLES);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    rc = ospi_opi_dtr_command(OSPI_CMD_OPI_RESET, 0UL, 0U, NULL, 0U, 1U,
                              0U, OSPI_WRITE_DUMMY_CYCLES);
    if (rc == TIKU_OSPI_OK) {
        ospi_delay(OSPI_RESET_SPINS);
    }
    return rc;
}

static tiku_ospi_err_t ospi_write_enable_spi(void) {
    tiku_ospi_err_t rc = ospi_spi_command(OSPI_CMD_SPI_WRITE_ENABLE, 0UL,
                                          0U, NULL, 0U, 1U);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }

    uint8_t status = 0U;
    rc = ospi_read_spi_status(&status);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    return (status & OSPI_STATUS_WEL) != 0U
               ? TIKU_OSPI_OK
               : TIKU_OSPI_ERR_PROGRAM;
}

static tiku_ospi_err_t ospi_write_dopi_config(void) {
    uint8_t value = OSPI_CR2_DOPI;
    tiku_ospi_err_t rc = ospi_write_enable_spi();
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    rc = ospi_spi_command(OSPI_CMD_SPI_WRITE_CR2, OSPI_CR2_ADDRESS, 1U,
                          &value, 1U, 1U);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    return ospi_wait_opi_ready(OSPI_SPINS);
}

static tiku_ospi_err_t ospi_read_opi_dtr_cr2(void) {
    uint8_t value[2] = { 0U, 0U };
    tiku_ospi_err_t rc = ospi_opi_dtr_command(OSPI_CMD_OPI_READ_CR2,
                                              OSPI_CR2_ADDRESS, 1U, value,
                                              sizeof(value), 0U,
                                              1U,
                                              OSPI_DTR_REG_DUMMY_CYCLES);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    if ((value[0] != value[1]) || ((value[0] & OSPI_CR2_DOPI) == 0U)) {
        return TIKU_OSPI_ERR_STATE;
    }
    return TIKU_OSPI_OK;
}

static int ospi_flash_range_valid(uint32_t addr, uint32_t len) {
    return addr < TIKU_OSPI_SIZE_BYTES &&
           len <= (TIKU_OSPI_SIZE_BYTES - addr);
}

static void ospi_invalidate_flash_range(uint32_t addr, uint32_t len) {
    tiku_stm32n6_dcache_invalidate(
        (const void *)(uintptr_t)(TIKU_OSPI_MMAP_BASE + addr), len);
}

static void ospi_configure_mmap_commands(void) {
    const ospi_command_t read_command = {
        .protocol = OSPI_PROTOCOL_OPI_DTR,
        .instruction_16 = 1U,
        .instruction_dtr = 1U,
        .address_dtr = 1U,
        .data_dtr = 1U,
        .dqs = 1U,
        .use_address = 1U,
        .use_data = 1U,
        .write = 0U,
        .instruction = OSPI_CMD_OPI_READ_DTR,
        .address = 0UL,
        .buf = NULL,
        .len = 0U,
        .dummy_cycles = OSPI_DTR_READ_DUMMY_CYCLES,
    };
    const ospi_command_t write_command = {
        .protocol = OSPI_PROTOCOL_OPI_DTR,
        .instruction_16 = 1U,
        .instruction_dtr = 1U,
        .address_dtr = 1U,
        .data_dtr = 1U,
        .dqs = 0U,
        .use_address = 1U,
        .use_data = 1U,
        .write = 1U,
        .instruction = OSPI_CMD_OPI_PAGE_PROGRAM,
        .address = 0UL,
        .buf = NULL,
        .len = 0U,
        .dummy_cycles = OSPI_WRITE_DUMMY_CYCLES,
    };

    TIKU_REG32(STM32N6_XSPI_AR) = read_command.address;
    TIKU_REG32(STM32N6_XSPI_CCR) = ospi_command_ccr(&read_command);
    TIKU_REG32(STM32N6_XSPI_TCR) =
        ((uint32_t)read_command.dummy_cycles << STM32N6_XSPI_TCR_DCYC_POS) &
        STM32N6_XSPI_TCR_DCYC_MSK;
    TIKU_REG32(STM32N6_XSPI_IR) = read_command.instruction;

    TIKU_REG32(STM32N6_XSPI_AR) = write_command.address;
    TIKU_REG32(STM32N6_XSPI_WCCR) = ospi_command_ccr(&write_command);
    TIKU_REG32(STM32N6_XSPI_WTCR) =
        ((uint32_t)write_command.dummy_cycles << STM32N6_XSPI_TCR_DCYC_POS) &
        STM32N6_XSPI_TCR_DCYC_MSK;
    TIKU_REG32(STM32N6_XSPI_WIR) = write_command.instruction;
}

static int ospi_aligned_range_valid(uint32_t addr, uint32_t len) {
    return len != 0U && (addr & 1U) == 0U && (len & 1U) == 0U &&
           ospi_flash_range_valid(addr, len);
}

static tiku_ospi_err_t ospi_read_dtr_aligned(uint32_t addr, void *buf,
                                             uint32_t len) {
    if (buf == NULL || !ospi_aligned_range_valid(addr, len)) {
        return TIKU_OSPI_ERR_ARG;
    }
    return ospi_opi_dtr_command(OSPI_CMD_OPI_READ_DTR, addr, 1U, buf, len,
                                0U, 1U, OSPI_DTR_READ_DUMMY_CYCLES);
}

static tiku_ospi_err_t ospi_program_aligned(uint32_t addr, const void *buf,
                                            uint32_t len) {
    if (buf == NULL || !ospi_aligned_range_valid(addr, len)) {
        return TIKU_OSPI_ERR_ARG;
    }

    const uint8_t *source = (const uint8_t *)buf;
    uint32_t remaining = len;
    while (remaining != 0U) {
        uint32_t page_offset = addr % TIKU_OSPI_PAGE_SIZE;
        uint32_t count = TIKU_OSPI_PAGE_SIZE - page_offset;
        if (count > remaining) {
            count = remaining;
        }
        if ((count & 1U) != 0U) {
            return TIKU_OSPI_ERR_ARG;
        }

        tiku_ospi_err_t rc = ospi_wait_opi_ready(OSPI_SPINS);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        rc = ospi_write_enable_opi();
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        rc = ospi_opi_dtr_command(OSPI_CMD_OPI_PAGE_PROGRAM, addr, 1U,
                                  (void *)(uintptr_t)source, count, 1U, 0U,
                                  OSPI_WRITE_DUMMY_CYCLES);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        rc = ospi_wait_opi_ready(OSPI_ERASE_SPINS);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }

        addr += count;
        source += count;
        remaining -= count;
    }
    return TIKU_OSPI_OK;
}

static tiku_ospi_err_t ospi_program_edge(uint32_t addr, uint8_t index,
                                         uint8_t value) {
    if ((addr & 1U) != 0U || index > 1U ||
        addr > (TIKU_OSPI_SIZE_BYTES - 2UL) ||
        (addr / TIKU_OSPI_PAGE_SIZE) !=
            ((addr + 1UL) / TIKU_OSPI_PAGE_SIZE)) {
        return TIKU_OSPI_ERR_ARG;
    }

    uint8_t pair[2] = { 0U, 0U };
    tiku_ospi_err_t rc = ospi_read_dtr_aligned(addr, pair, sizeof(pair));
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    pair[index] = value;
    return ospi_program_aligned(addr, pair, sizeof(pair));
}

static void ospi_invalidate_program_range(uint32_t addr, uint32_t len) {
    uint32_t aligned_addr = addr & ~1U;
    uint32_t aligned_end = addr + len;
    if ((aligned_end & 1U) != 0U) {
        aligned_end++;
    }
    ospi_invalidate_flash_range(aligned_addr, aligned_end - aligned_addr);
}

static void ospi_configure_clock(void) {
    uint32_t ic = TIKU_REG32(STM32N6_RCC_ICCFGR(OSPI_IC_INDEX));
    ic &= ~(STM32N6_IC_INT_MSK | STM32N6_IC_SEL_MSK);
    ic |= (STM32N6_IC_SEL_PLL1 << STM32N6_IC_SEL_POS);
    ic |= ((OSPI_IC_DIVIDER - 1UL) << STM32N6_IC_INT_POS);
    TIKU_REG32(STM32N6_RCC_ICCFGR(OSPI_IC_INDEX)) = ic;
    TIKU_REG32(STM32N6_RCC_DIVENR) |= (1UL << (OSPI_IC_INDEX - 1U));

    uint32_t ccipr = TIKU_REG32(STM32N6_RCC_CCIPR6);
    ccipr &= ~STM32N6_CCIPR6_XSPI2SEL_MSK;
    ccipr |= STM32N6_CCIPR6_XSPI2SEL_IC3;
    TIKU_REG32(STM32N6_RCC_CCIPR6) = ccipr;
}

static void ospi_reset_peripherals(void) {
    /* Reset both the XSPI2 controller and the XSPIM routing block. This is
     * required for a clean handoff after warm/debug resets; clearing XSPI2_CR
     * alone does not restore the peripheral's reset state. */
    TIKU_REG32(STM32N6_RCC_AHB5RSTSR) =
        STM32N6_RCC_AHB5RSTSR_XSPI2 | STM32N6_RCC_AHB5RSTSR_XSPIM;
    (void)TIKU_REG32(STM32N6_RCC_AHB5RSTSR);

    TIKU_REG32(STM32N6_RCC_AHB5RSTCR) =
        STM32N6_RCC_AHB5RSTCR_XSPI2 | STM32N6_RCC_AHB5RSTCR_XSPIM;
    (void)TIKU_REG32(STM32N6_RCC_AHB5RSTCR);
}

static tiku_ospi_err_t ospi_enable_pads(void) {
    /* Enable clock signals to XSPI2 (connected to flash on the Nucleo)
       and to GPION (pins for XSPI2 lines) */
    TIKU_REG32(STM32N6_RCC_AHB5ENR) |= STM32N6_RCC_AHB5ENR_XSPI2 |
                                       STM32N6_RCC_AHB5ENR_XSPIM;
    TIKU_REG32(STM32N6_RCC_AHB4ENR) |= STM32N6_RCC_AHB4ENR_GPION;
    (void)TIKU_REG32(STM32N6_RCC_AHB5ENR);

    ospi_reset_peripherals();

    /* Enable VDDIO - power regulator for XSPI2 */
    TIKU_REG32(STM32N6_PWR_SVMCR3) |= STM32N6_PWR_SVMCR3_VDDIO3VMEN;
    if (!ospi_wait(STM32N6_PWR_SVMCR3, STM32N6_PWR_SVMCR3_VDDIO3RDY, 1,
                   OSPI_SPINS)) {
        return TIKU_OSPI_ERR_TIMEOUT;
    }
    TIKU_REG32(STM32N6_PWR_SVMCR3) |= STM32N6_PWR_SVMCR3_VDDIO3SV |
                                      STM32N6_PWR_SVMCR3_VDDIO3VRSEL;

    static const uint8_t pins[] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 8U, 9U, 10U, 11U
    };
    for (size_t i = 0U; i < sizeof(pins) / sizeof(pins[0]); i++) {
        tiku_stm32n6_gpio_init_alt(STM32N6_GPIO_PORT_N, pins[i],
                                   STM32N6_XSPI2_AF);
    }

    TIKU_REG32(STM32N6_XSPIM_CR) = STM32N6_XSPIM_CR_CSSEL_OVR_EN;
    return TIKU_OSPI_OK;
}

static void ospi_configure_controller(void) {
    TIKU_REG32(STM32N6_XSPI_CR) = 0UL;
    TIKU_REG32(STM32N6_XSPI_DCR1) =
        STM32N6_XSPI_DCR1_MTYP_MACRONIX |
        STM32N6_XSPI_DCR1_DEVSIZE_64MB |
        (2UL << STM32N6_XSPI_DCR1_CSHT_POS);
    TIKU_REG32(STM32N6_XSPI_DCR2) =
        STM32N6_XSPI_DCR2_PRESCALER(OSPI_XSPI_PRESCALER);
    TIKU_REG32(STM32N6_XSPI_TCR) = 0UL;
    TIKU_REG32(STM32N6_XSPI_CR) = STM32N6_XSPI_CR_EN;

    uint32_t prescaler = (TIKU_REG32(STM32N6_XSPI_DCR2) &
                          STM32N6_XSPI_DCR2_PRESCALER_MSK) >>
                         STM32N6_XSPI_DCR2_PRESCALER_POS;
    ospi_clock_hz_value = OSPI_KERNEL_CLOCK_HZ / (prescaler + 1UL);
}

tiku_ospi_err_t tiku_ospi_init(void) {
    if (ospi_ready) {
        return TIKU_OSPI_OK;
    }
    ospi_clock_hz_value = 0UL;

    ospi_configure_clock();
    tiku_ospi_err_t rc = ospi_enable_pads();
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    ospi_configure_controller();

    rc = ospi_spi_reset();
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }

    uint8_t id[3] = { 0U, 0U, 0U };
    rc = ospi_read_spi_id(id);
    
    if (!ospi_id_matches(id)) {
        return TIKU_OSPI_ERR_ID;
    }

    rc = ospi_write_dopi_config();
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }

    rc = ospi_read_opi_dtr_id(id);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }

    if (!ospi_id_matches(id)) {
        return TIKU_OSPI_ERR_ID;
    }

    rc = ospi_read_opi_dtr_cr2();
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }

    ospi_ready = 1U;
    return TIKU_OSPI_OK;
}

tiku_ospi_err_t tiku_ospi_read_id(tiku_ospi_id_t *out) {
    if (out == NULL) {
        return TIKU_OSPI_ERR_ARG;
    }
    if (!ospi_ready) {
        return TIKU_OSPI_ERR_STATE;
    }

    uint8_t id[3] = { 0U, 0U, 0U };
    tiku_ospi_err_t rc = ospi_read_opi_dtr_id(id);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    out->mfr = id[0];
    out->type = id[1];
    out->capacity = id[2];
    return TIKU_OSPI_OK;
}

tiku_ospi_err_t tiku_ospi_read(uint32_t addr, void *buf, uint32_t len) {
    if (buf == NULL || len == 0U || !ospi_flash_range_valid(addr, len)) {
        return TIKU_OSPI_ERR_ARG;
    }
    if (!ospi_ready) {
        return TIKU_OSPI_ERR_STATE;
    }

    uint8_t *destination = (uint8_t *)buf;
    uint32_t current = addr;
    uint32_t remaining = len;
    if ((current & 1U) != 0U) {
        uint8_t pair[2] = { 0U, 0U };
        tiku_ospi_err_t rc = ospi_read_dtr_aligned(current - 1U, pair,
                                                   sizeof(pair));
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        destination[0] = pair[1];
        current++;
        destination++;
        remaining--;
    }

    if (remaining == 0U) {
        return TIKU_OSPI_OK;
    }
    if ((remaining & 1U) != 0U) {
        uint8_t pair[2] = { 0U, 0U };
        uint32_t middle_len = remaining - 1U;
        tiku_ospi_err_t rc;

        if (middle_len != 0U) {
            rc = ospi_read_dtr_aligned(current, destination, middle_len);
            if (rc != TIKU_OSPI_OK) {
                return rc;
            }
        }
        rc = ospi_read_dtr_aligned(current + middle_len, pair,
                                   sizeof(pair));
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        destination[middle_len] = pair[0];
        return TIKU_OSPI_OK;
    }
    return ospi_read_dtr_aligned(current, destination, remaining);
}

tiku_ospi_err_t tiku_ospi_program(uint32_t addr, const void *buf,
                                  uint32_t len) {
    if (buf == NULL || len == 0U || !ospi_flash_range_valid(addr, len)) {
        return TIKU_OSPI_ERR_ARG;
    }
    if (!ospi_ready) {
        return TIKU_OSPI_ERR_STATE;
    }

    const uint8_t *source = (const uint8_t *)buf;
    uint32_t current = addr;
    uint32_t remaining = len;
    if ((current & 1U) != 0U) {
        tiku_ospi_err_t rc = ospi_program_edge(current - 1U, 1U,
                                               source[0]);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        current++;
        source++;
        remaining--;
    }

    if ((remaining & 1U) != 0U) {
        uint32_t tail_addr = current + remaining - 1U;
        tiku_ospi_err_t rc = ospi_program_edge(tail_addr & ~1U, 0U,
                                               source[remaining - 1U]);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
        remaining--;
    }

    if (remaining != 0U) {
        tiku_ospi_err_t rc = ospi_program_aligned(current, source, remaining);
        if (rc != TIKU_OSPI_OK) {
            return rc;
        }
    }

    ospi_invalidate_program_range(addr, len);
    return TIKU_OSPI_OK;
}

tiku_ospi_err_t tiku_ospi_erase_sector(uint32_t addr) {
    if (addr >= TIKU_OSPI_SIZE_BYTES) {
        return TIKU_OSPI_ERR_ARG;
    }
    if (!ospi_ready) {
        return TIKU_OSPI_ERR_STATE;
    }

    uint32_t sector_addr = addr & ~(TIKU_OSPI_SECTOR_SIZE - 1UL);
    tiku_ospi_err_t rc = ospi_wait_opi_ready(OSPI_SPINS);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    rc = ospi_write_enable_opi();
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    rc = ospi_opi_dtr_command(OSPI_CMD_OPI_SECTOR_ERASE, sector_addr, 1U,
                              NULL, 0U, 1U, 0U, OSPI_WRITE_DUMMY_CYCLES);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }
    rc = ospi_wait_opi_ready(OSPI_ERASE_SPINS);
    if (rc != TIKU_OSPI_OK) {
        return rc;
    }

    ospi_invalidate_flash_range(sector_addr, TIKU_OSPI_SECTOR_SIZE);
    return TIKU_OSPI_OK;
}

static tiku_ospi_err_t ospi_mmap_disable_internal(void) {
    if (!ospi_mmap) {
        return TIKU_OSPI_OK;
    }

    TIKU_REG32(STM32N6_XSPI_CR) |= STM32N6_XSPI_CR_ABORT;
    if (!ospi_wait(STM32N6_XSPI_CR, STM32N6_XSPI_CR_ABORT, 0,
                   OSPI_SPINS) ||
        !ospi_wait(STM32N6_XSPI_SR, STM32N6_XSPI_SR_BUSY, 0, OSPI_SPINS)) {
        return TIKU_OSPI_ERR_TIMEOUT;
    }
    ospi_finish();

    uint32_t cr = TIKU_REG32(STM32N6_XSPI_CR);
    cr &= ~STM32N6_XSPI_CR_FMODE_MSK;
    TIKU_REG32(STM32N6_XSPI_CR) = cr;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    ospi_mmap = 0U;
    return TIKU_OSPI_OK;
}

tiku_ospi_err_t tiku_ospi_mmap_enable(void) {
    if (!ospi_ready) {
        return TIKU_OSPI_ERR_STATE;
    }
    if (ospi_mmap) {
        return TIKU_OSPI_OK;
    }
    if (!ospi_wait(STM32N6_XSPI_SR, STM32N6_XSPI_SR_BUSY, 0, OSPI_SPINS)) {
        return TIKU_OSPI_ERR_TIMEOUT;
    }
    ospi_finish();

    uint32_t cr = TIKU_REG32(STM32N6_XSPI_CR);
    cr &= ~STM32N6_XSPI_CR_FMODE_MSK;
    TIKU_REG32(STM32N6_XSPI_CR) = cr;
    ospi_configure_mmap_commands();

    cr = TIKU_REG32(STM32N6_XSPI_CR);
    cr &= ~STM32N6_XSPI_CR_FMODE_MSK;
    cr |= STM32N6_XSPI_FMODE_MMAP << STM32N6_XSPI_CR_FMODE_POS;
    TIKU_REG32(STM32N6_XSPI_CR) = cr;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    ospi_mmap = 1U;
    return TIKU_OSPI_OK;
}

tiku_ospi_err_t tiku_ospi_mmap_disable(void) {
    return ospi_mmap_disable_internal();
}

unsigned long tiku_ospi_clock_hz(void) {
    return ospi_ready ? ospi_clock_hz_value : 0UL;
}

int tiku_ospi_ready(void) {
    return ospi_ready ? 1 : 0;
}
