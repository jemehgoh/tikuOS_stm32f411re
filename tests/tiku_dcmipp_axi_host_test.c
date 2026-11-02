/* Host-side tests for the STM32N6 DCMIPP AXI IP-Plug setup. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../arch/stm32n6/tiku_stm32n6_regs.h"

typedef struct {
    uintptr_t address;
    uint32_t value;
} test_register_t;

typedef struct {
    uintptr_t address;
    uint32_t value;
} test_write_t;

static test_register_t registers[32];
static size_t register_count;
static test_write_t writes[32];
static size_t write_count;

static uint32_t test_mmio_read(uintptr_t address) {
    size_t i;
    for (i = 0U; i < register_count; ++i) {
        if (registers[i].address == address) {
            return registers[i].value;
        }
    }
    return 0UL;
}

static int ignore_rimc_writes;

static void test_mmio_write(uintptr_t address, uint32_t value) {
    size_t i;
    if (ignore_rimc_writes != 0 && address == STM32N6_RIFSC_RIMC_ATTR(
            STM32N6_RIFSC_RIMC_DCMIPP)) {
        return; /* models a locked RIMC (RIMC_CR.GLOCK) */
    }
    assert(write_count < sizeof(writes) / sizeof(writes[0]));
    writes[write_count++] = (test_write_t){address, value};
    for (i = 0U; i < register_count; ++i) {
        if (registers[i].address == address) {
            registers[i].value = value;
            return;
        }
    }
    assert(register_count < sizeof(registers) / sizeof(registers[0]));
    registers[register_count++] = (test_register_t){address, value};
}

#define TIKU_DCMIPP_REG_READ(address) test_mmio_read((uintptr_t)(address))
#define TIKU_DCMIPP_REG_WRITE(address, value) \
    test_mmio_write((uintptr_t)(address), (uint32_t)(value))
#include "../arch/stm32n6/tiku_dcmipp_arch.c"

uint8_t tiku_process_post(struct tiku_process *p, tiku_event_t event,
                          tiku_event_data_t data) {
    (void)p;
    (void)event;
    (void)data;
    return 1U;
}

void tiku_cpu_stm32n6_delay_ms(unsigned int ms) {
    (void)ms;
}

static void test_axi_plug_pipe2_registers(void) {
    size_t i;
    size_t lock_write = (size_t)-1;
    size_t unlock_write = (size_t)-1;

    register_count = 0U;
    write_count = 0U;
    dcmipp.peripheral_enabled = 1U;
    test_mmio_write(STM32N6_DCMIPP_IPGR3, STM32N6_DCMIPP_IPGR3_IDLE);
    test_mmio_write(STM32N6_DCMIPP_IPC5R2, 0x10000UL);
    write_count = 0U;

    assert(tiku_dcmipp_arch_axi_plug_configure_pipe2() == TIKU_DCMIPP_OK);
    assert(test_mmio_read(STM32N6_DCMIPP_IPGR1) == 0UL);
    assert(test_mmio_read(STM32N6_DCMIPP_IPC5R1) == 4UL);
    assert(test_mmio_read(STM32N6_DCMIPP_IPC5R2) == 0x10000UL);
    assert(test_mmio_read(STM32N6_DCMIPP_IPC5R3) == 0x027F01C0UL);
    assert(test_mmio_read(STM32N6_DCMIPP_IPGR2) == 0UL);

    for (i = 0U; i < write_count; ++i) {
        if (writes[i].address == STM32N6_DCMIPP_IPGR2 &&
            (writes[i].value & STM32N6_DCMIPP_IPGR2_PSTART) != 0UL) {
            lock_write = i;
        }
        if (writes[i].address == STM32N6_DCMIPP_IPGR2 &&
            (writes[i].value & STM32N6_DCMIPP_IPGR2_PSTART) == 0UL) {
            unlock_write = i;
        }
    }
    assert(lock_write != (size_t)-1);
    assert(unlock_write != (size_t)-1 && unlock_write > lock_write);
}

static void test_axi_plug_timeout_unlocks(void) {
    register_count = 0U;
    write_count = 0U;
    dcmipp.peripheral_enabled = 1U;
    assert(tiku_dcmipp_arch_axi_plug_configure_pipe2() == TIKU_DCMIPP_ERR_BUSY);
    assert(test_mmio_read(STM32N6_DCMIPP_IPGR2) == 0UL);
    assert(write_count == 2U);
}

static void test_pipe_setup_crop_and_rgb888_output(void) {
    const tiku_dcmipp_pipe_config_t pipe1 = {
        .DataTypeMode = 0U,
        .DataTypeIDA = 0x2BU,
        .DataTypeIDB = 0x2BU,
    };
    const tiku_dcmipp_pipe_config_t pipe2 = {
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB888,
    };
    const tiku_dcmipp_postproc_config_t postproc = {
        .pitch = {288U, 0U, 0U},
    };
    const tiku_dcmipp_plane_address_t output = {
        .plane = {0x34000000UL, 0UL, 0UL},
        .count = 1U,
    };

    register_count = 0U;
    write_count = 0U;
    dcmipp.peripheral_enabled = 1U;

    assert(tiku_dcmipp_arch_pipe_configure(PIPE1, &pipe1) == TIKU_DCMIPP_OK);
    assert((test_mmio_read(STM32N6_DCMIPP_P1FSCR) &
            (STM32N6_DCMIPP_P1FSCR_DTIDA_MSK |
             STM32N6_DCMIPP_P1FSCR_DTIDB_MSK |
             STM32N6_DCMIPP_P1FSCR_DTMODE_MSK)) == 0x00002B2BUL);

    assert(tiku_dcmipp_arch_pipe_configure(PIPE2, &pipe2) == TIKU_DCMIPP_OK);
    assert(test_mmio_read(STM32N6_DCMIPP_P2PPCR) ==
           TIKU_DCMIPP_PIXEL_PACKER_RGB888);
    assert(tiku_dcmipp_arch_postprocessing_configure(PIPE2, &postproc) ==
           TIKU_DCMIPP_OK);
    assert(test_mmio_read(STM32N6_DCMIPP_P2PPM0PR) == 288U);
    assert(tiku_dcmipp_arch_pipe_crop_rect(PIPE2, 1248U, 924U, 96U, 96U) ==
           TIKU_DCMIPP_OK);
    assert(test_mmio_read(STM32N6_DCMIPP_P2CRSTR) ==
           (1248UL | (924UL << 16)));
    assert(test_mmio_read(STM32N6_DCMIPP_P2CRSZR) ==
           (96UL | (96UL << 16) | TIKU_DCMIPP_POSTPROC_ENABLE));

    dcmipp.pipe[PIPE2].enabled = 1U;
    assert(tiku_dcmipp_arch_pipe_start_capture(
               PIPE2, &output, 0U, DCMIPP_CAPTURE_CONTINUOUS) ==
           TIKU_DCMIPP_OK);
    assert(test_mmio_read(STM32N6_DCMIPP_P2PPM0AR1) == 0x34000000UL);
    assert((test_mmio_read(STM32N6_DCMIPP_P2FCTCR) &
            STM32N6_DCMIPP_PxFCTCR_CPTREQ) != 0UL);
}

static void test_master_attr_is_secure_priv_cid1(void) {
    const uintptr_t reg = STM32N6_RIFSC_RIMC_ATTR(STM32N6_RIFSC_RIMC_DCMIPP);

    /* RIF_MASTER_INDEX_DCMIPP is 9: RIFSC + 0xC10 + 4 * 9. */
    assert(reg == STM32N6_RIFSC_BASE + 0xC34UL);

    register_count = 0U;
    write_count = 0U;
    ignore_rimc_writes = 0;
    assert(tiku_dcmipp_master_attr_configure() == TIKU_DCMIPP_OK);
    assert(test_mmio_read(reg) == (STM32N6_RIFSC_RIMC_ATTR_SEC |
                                   STM32N6_RIFSC_RIMC_ATTR_PRIV |
                                   (1UL << 4)));
    assert(write_count == 1U);
}

static void test_master_attr_locked_rimc_fails(void) {
    register_count = 0U;
    write_count = 0U;
    ignore_rimc_writes = 1;
    assert(tiku_dcmipp_master_attr_configure() ==
           TIKU_DCMIPP_ERR_NOT_ALLOWED);
    ignore_rimc_writes = 0;
}

int main(void) {
    test_master_attr_is_secure_priv_cid1();
    test_master_attr_locked_rimc_fails();
    test_axi_plug_pipe2_registers();
    test_axi_plug_timeout_unlocks();
    test_pipe_setup_crop_and_rgb888_output();
    puts("STM32N6 DCMIPP AXI host tests passed");
    return 0;
}
