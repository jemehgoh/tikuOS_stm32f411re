/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Author: Jeremy Goh
 *
 * tiku_dcmipp_arch.c - STM32N6 DCMIPP driver state scaffolding.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_dcmipp_arch.h"
#include "tiku_stm32n6_regs.h"
#include "tiku_cpu_common.h"

#ifndef TIKU_DCMIPP_REG_READ
#define TIKU_DCMIPP_REG_READ(address) TIKU_REG32(address)
#endif
#ifndef TIKU_DCMIPP_REG_WRITE
#define TIKU_DCMIPP_REG_WRITE(address, value) (TIKU_REG32(address) = (value))
#endif

#define TIKU_DCMIPP_POSTPROC_ENABLE (1UL << 31)
#define TIKU_DCMIPP_STAGE_ENABLE    (1UL << 0)
#define TIKU_DCMIPP_FS_DT_MSK       0x3FUL
#define TIKU_DCMIPP_EVENT_TYPE_POS  0U
#define TIKU_DCMIPP_EVENT_TYPE_MSK  (0xFUL << TIKU_DCMIPP_EVENT_TYPE_POS)
#define TIKU_DCMIPP_EVENT_PIPE_POS  4U
#define TIKU_DCMIPP_EVENT_PIPE_MSK  (0x3UL << TIKU_DCMIPP_EVENT_PIPE_POS)
#define TIKU_DCMIPP_EVENT_VC_POS    6U
#define TIKU_DCMIPP_EVENT_VC_MSK    (0x3UL << TIKU_DCMIPP_EVENT_VC_POS)
#define TIKU_DCMIPP_EVENT_ERR_POS   8U
#define TIKU_DCMIPP_EVENT_ERR_MSK   (0xFUL << TIKU_DCMIPP_EVENT_ERR_POS)
#define TIKU_DCMIPP_EVENT_FLAG_POS  12U
#define TIKU_DCMIPP_EVENT_FLAG_MSK  (0x3FUL << TIKU_DCMIPP_EVENT_FLAG_POS)
#define TIKU_DCMIPP_AXI_WAIT_SPINS  100000UL

typedef struct {
    uint8_t hsfreqrange;
    uint16_t osc_freq_target;
} tiku_dcmipp_snps_freq_t;

/* ST's D-PHY bitrate selector table (DCMIPP_CSI_PHY_BT_80..2500). */
static const tiku_dcmipp_snps_freq_t tiku_dcmipp_snps_freqs[63] = {
    {0x00U, 460U}, {0x10U, 460U}, {0x20U, 460U}, {0x30U, 460U},
    {0x01U, 460U}, {0x11U, 460U}, {0x21U, 460U}, {0x31U, 460U},
    {0x02U, 460U}, {0x12U, 460U}, {0x22U, 460U}, {0x32U, 460U},
    {0x03U, 460U}, {0x13U, 460U}, {0x23U, 460U}, {0x33U, 460U},
    {0x04U, 460U}, {0x14U, 460U}, {0x25U, 460U}, {0x35U, 460U},
    {0x05U, 460U}, {0x16U, 460U}, {0x26U, 460U}, {0x37U, 460U},
    {0x07U, 460U}, {0x18U, 460U}, {0x28U, 460U}, {0x39U, 460U},
    {0x09U, 460U}, {0x19U, 460U}, {0x29U, 460U}, {0x3AU, 460U},
    {0x0AU, 460U}, {0x1AU, 460U}, {0x2AU, 460U}, {0x3BU, 460U},
    {0x0BU, 460U}, {0x1BU, 460U}, {0x2BU, 460U}, {0x3CU, 460U},
    {0x0CU, 460U}, {0x1CU, 460U}, {0x2CU, 460U}, {0x3DU, 285U},
    {0x0DU, 295U}, {0x1DU, 304U}, {0x2EU, 313U}, {0x3EU, 322U},
    {0x0EU, 331U}, {0x1EU, 341U}, {0x2FU, 350U}, {0x3FU, 359U},
    {0x0FU, 368U}, {0x40U, 377U}, {0x41U, 387U}, {0x42U, 396U},
    {0x43U, 405U}, {0x44U, 414U}, {0x45U, 423U}, {0x46U, 432U},
    {0x47U, 442U}, {0x48U, 451U}, {0x49U, 460U}
};

typedef struct {
    uint8_t enabled;
    uint8_t running;
    uint8_t continuous;
    uint8_t double_buffer;
    tiku_dcmipp_capture_mode_t capture_mode;
    tiku_dcmipp_pipe_config_t config;
    tiku_dcmipp_plane_address_t address;
} tiku_dcmipp_pipe_state_t;

typedef struct {
    uintptr_t dcmipp_base;
    uintptr_t csi_base;
    uint8_t peripheral_enabled;
    uint8_t input_enabled;
    tiku_dcmipp_input_t input;
    tiku_dcmipp_pipe_diff_source_t pipe_diff;
    uint8_t isp_enabled;
    tiku_dcmipp_isp_config_t isp_config;
    tiku_dcmipp_postproc_config_t postproc[DCMIPP_PIPE_COUNT];
    uint8_t postproc_enabled[DCMIPP_PIPE_COUNT];
    tiku_dcmipp_pipe_state_t pipe[DCMIPP_PIPE_COUNT];
    volatile uint32_t event_drops;
    volatile int last_error;
} tiku_dcmipp_context_t;

/* Global peripheral state struct. The initial implementation of this API is
 * not thread-safe; thread safety will be supported in future iterations. */
static tiku_dcmipp_context_t dcmipp = {
    .dcmipp_base = STM32N6_DCMIPP_BASE,
    .csi_base = STM32N6_CSI_BASE,
    .input = DCMIPP_INPUT_PARALLEL,
    .pipe_diff = DCMIPP_PIPEDIFF_COUPLED,
};

static int tiku_dcmipp_fail(int err) {
    dcmipp.last_error = err;
    return err;
}

static uint8_t tiku_dcmipp_valid_pipe(tiku_dcmipp_pipe_t pipe) {
    return pipe == PIPE0 || pipe == PIPE1 || pipe == PIPE2;
}

static uint8_t tiku_dcmipp_valid_parallel_format(
    tiku_dcmipp_parallel_format_t format) {
    switch (format) {
    case TIKU_DCMIPP_PARALLEL_FORMAT_BYTE:
    case TIKU_DCMIPP_PARALLEL_FORMAT_YUV422:
    case TIKU_DCMIPP_PARALLEL_FORMAT_RGB565:
    case TIKU_DCMIPP_PARALLEL_FORMAT_RGB666:
    case TIKU_DCMIPP_PARALLEL_FORMAT_RGB888:
    case TIKU_DCMIPP_PARALLEL_FORMAT_RAW8:
    case TIKU_DCMIPP_PARALLEL_FORMAT_RAW10:
    case TIKU_DCMIPP_PARALLEL_FORMAT_RAW12:
    case TIKU_DCMIPP_PARALLEL_FORMAT_RAW14:
    case TIKU_DCMIPP_PARALLEL_FORMAT_MONO8:
    case TIKU_DCMIPP_PARALLEL_FORMAT_MONO10:
    case TIKU_DCMIPP_PARALLEL_FORMAT_MONO12:
    case TIKU_DCMIPP_PARALLEL_FORMAT_MONO14:
        return 1U;
    default:
        return 0U;
    }
}

static void tiku_dcmipp_csi_phy_write(uint8_t reg_msb,
                                     uint8_t reg_lsb,
                                     uint8_t value) {
    /* Synopsys D-PHY test-interface write sequence used by the ST HAL. */
    TIKU_REG32(STM32N6_CSI_PHY_PTCR1) = STM32N6_CSI_PHY_PTCR1_TWM;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = STM32N6_CSI_PHY_PTCR0_TCKEN;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR1) = STM32N6_CSI_PHY_PTCR1_TWM;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = 0UL;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR1) = 0UL;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR1) =
        (uint32_t)reg_msb & STM32N6_CSI_PHY_PTCR1_TDI_MSK;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = STM32N6_CSI_PHY_PTCR0_TCKEN;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = 0UL;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR1) = STM32N6_CSI_PHY_PTCR1_TWM;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = STM32N6_CSI_PHY_PTCR0_TCKEN;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR1) =
        STM32N6_CSI_PHY_PTCR1_TWM |
        ((uint32_t)reg_lsb & STM32N6_CSI_PHY_PTCR1_TDI_MSK);
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = 0UL;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR1) =
        (uint32_t)value & STM32N6_CSI_PHY_PTCR1_TDI_MSK;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = STM32N6_CSI_PHY_PTCR0_TCKEN;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = 0UL;
}

static void tiku_dcmipp_nvic_enable(uint32_t irq) {
    /* NVIC external IRQ registers are arrays of 32-bit bitmaps: irq / 32
     * selects the word, irq % 32 selects the bit, and 1UL is the single-bit
     * mask shifted into that slot. */
    TIKU_REG32(STM32N6_NVIC_ITNS(irq / 32U)) &= ~(1UL << (irq % 32U));
    TIKU_REG32(STM32N6_NVIC_ICPR(irq / 32U)) = (1UL << (irq % 32U));
    TIKU_REG32(STM32N6_NVIC_ISER(irq / 32U)) = (1UL << (irq % 32U));
}

static void tiku_dcmipp_nvic_disable(uint32_t irq) {
    /* Same NVIC 32-IRQ-per-word layout as enable: write a single 1 bit to
     * clear-enable and clear-pending for this IRQ without touching neighbors. */
    TIKU_REG32(STM32N6_NVIC_ICER(irq / 32U)) = (1UL << (irq % 32U));
    TIKU_REG32(STM32N6_NVIC_ICPR(irq / 32U)) = (1UL << (irq % 32U));
}

static uint8_t tiku_dcmipp_valid_plane_address(tiku_dcmipp_pipe_t pipe,
                                               const tiku_dcmipp_plane_address_t *addr) {
    if (addr == NULL || addr->count == 0U || addr->count > 3U) {
        return 0U;
    }
    if ((pipe == PIPE0 || pipe == PIPE2) && addr->count != 1U) {
        return 0U;
    }

    for (uint8_t i = 0; i < addr->count; i++) {
        if (addr->plane[i] == (uintptr_t)0) {
            return 0U;
        }
    }
    return 1U;
}

static uint8_t tiku_dcmipp_enabled(void) {
    return dcmipp.peripheral_enabled != 0U;
}

static int tiku_dcmipp_require_enabled(void) {
    return tiku_dcmipp_enabled() ? TIKU_DCMIPP_OK : tiku_dcmipp_fail(TIKU_DCMIPP_ERR_DISABLED);
}

static uint8_t tiku_dcmipp_isp_enable_allowed(void) {
    if (dcmipp.pipe[PIPE1].enabled) {
        return 1U;
    }

    return dcmipp.pipe[PIPE2].enabled &&
           dcmipp.pipe_diff == DCMIPP_PIPEDIFF_COUPLED;
}

static uintptr_t tiku_dcmipp_fscr(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0FSCR;
    case PIPE1:
        return STM32N6_DCMIPP_P1FSCR;
    default:
        return STM32N6_DCMIPP_P2FSCR;
    }
}

static uintptr_t tiku_dcmipp_fctcr(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0FCTCR;
    case PIPE1:
        return STM32N6_DCMIPP_P1FCTCR;
    default:
        return STM32N6_DCMIPP_P2FCTCR;
    }
}

static uintptr_t tiku_dcmipp_crop_start_reg(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0SCSTR;
    case PIPE1:
        return STM32N6_DCMIPP_P1CRSTR;
    default:
        return STM32N6_DCMIPP_P2CRSTR;
    }
}

static uintptr_t tiku_dcmipp_crop_size_reg(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0SCSZR;
    case PIPE1:
        return STM32N6_DCMIPP_P1CRSZR;
    default:
        return STM32N6_DCMIPP_P2CRSZR;
    }
}

static uintptr_t tiku_dcmipp_ppcr(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0PPCR;
    case PIPE1:
        return STM32N6_DCMIPP_P1PPCR;
    default:
        return STM32N6_DCMIPP_P2PPCR;
    }
}

static uintptr_t tiku_dcmipp_ier(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0IER;
    case PIPE1:
        return STM32N6_DCMIPP_P1IER;
    default:
        return STM32N6_DCMIPP_P2IER;
    }
}

static uintptr_t tiku_dcmipp_sr(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0SR;
    case PIPE1:
        return STM32N6_DCMIPP_P1SR;
    default:
        return STM32N6_DCMIPP_P2SR;
    }
}

static uintptr_t tiku_dcmipp_fcr(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0FCR;
    case PIPE1:
        return STM32N6_DCMIPP_P1FCR;
    default:
        return STM32N6_DCMIPP_P2FCR;
    }
}

static uint32_t tiku_dcmipp_pipen_bit(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_P0FSCR_PIPEN;
    case PIPE1:
        return STM32N6_DCMIPP_P1FSCR_PIPEN;
    default:
        return STM32N6_DCMIPP_P2FSCR_PIPEN;
    }
}

static uint32_t tiku_dcmipp_pipe_fscr_value(tiku_dcmipp_pipe_t pipe,
                                            const tiku_dcmipp_pipe_config_t *config,
                                            uint8_t virtual_channel) {
    uint32_t value = config->flow_selection_flags;

    value |= (config->DataTypeIDA & TIKU_DCMIPP_FS_DT_MSK);
    if (pipe != PIPE2) {
        value |= (config->DataTypeIDB & TIKU_DCMIPP_FS_DT_MSK) << 8;
        value |= (config->DataTypeMode & 0x3UL) << 16;
    }
    if (pipe == PIPE1 && dcmipp.pipe_diff == DCMIPP_PIPEDIFF_INDEPENDENT) {
        value |= STM32N6_DCMIPP_P1FSCR_PIPEDIFF;
    }
    if (!(pipe == PIPE2 && dcmipp.pipe_diff == DCMIPP_PIPEDIFF_COUPLED)) {
        value |= ((uint32_t)virtual_channel & 0x3UL) << 19;
    }
    return value;
}

static void tiku_dcmipp_write_pipe_addresses(tiku_dcmipp_pipe_t pipe,
                                             const tiku_dcmipp_plane_address_t *addr,
                                             uint8_t buffer_index) {
    if (pipe == PIPE0) {
        TIKU_REG32(buffer_index == 0U ? STM32N6_DCMIPP_P0PPM0AR1 :
                   STM32N6_DCMIPP_P0PPM0AR2) = (uint32_t)addr->plane[0];
    } else if (pipe == PIPE1) {
        TIKU_REG32(buffer_index == 0U ? STM32N6_DCMIPP_P1PPM0AR1 :
                   STM32N6_DCMIPP_P1PPM0AR2) = (uint32_t)addr->plane[0];
        if (addr->count > 1U) {
            TIKU_REG32(buffer_index == 0U ? STM32N6_DCMIPP_P1PPM1AR1 :
                       STM32N6_DCMIPP_P1PPM1AR2) = (uint32_t)addr->plane[1];
        }
        if (addr->count > 2U) {
            TIKU_REG32(buffer_index == 0U ? STM32N6_DCMIPP_P1PPM2AR1 :
                       STM32N6_DCMIPP_P1PPM2AR2) = (uint32_t)addr->plane[2];
        }
    } else {
        TIKU_REG32(buffer_index == 0U ? STM32N6_DCMIPP_P2PPM0AR1 :
                   STM32N6_DCMIPP_P2PPM0AR2) = (uint32_t)addr->plane[0];
    }
}

static void tiku_dcmipp_write_pipe_pitch(tiku_dcmipp_pipe_t pipe,
                                         const uint32_t pitch[3]) {
    if (pipe == PIPE1) {
        TIKU_REG32(STM32N6_DCMIPP_P1PPM0PR) = pitch[0];
        TIKU_REG32(STM32N6_DCMIPP_P1PPM1PR) = pitch[1];
    } else if (pipe == PIPE2) {
        TIKU_REG32(STM32N6_DCMIPP_P2PPM0PR) = pitch[0];
    }
}

static uint32_t tiku_dcmipp_pipe_interrupt_mask(tiku_dcmipp_pipe_t pipe) {
    switch (pipe) {
    case PIPE0:
        return STM32N6_DCMIPP_IT_PIPE0_LINE |
               STM32N6_DCMIPP_IT_PIPE0_FRAME |
               STM32N6_DCMIPP_IT_PIPE0_VSYNC |
               STM32N6_DCMIPP_IT_PIPE0_LIMIT |
               STM32N6_DCMIPP_IT_PIPE0_OVR;
    case PIPE1:
        return STM32N6_DCMIPP_IT_PIPE1_LINE |
               STM32N6_DCMIPP_IT_PIPE1_FRAME |
               STM32N6_DCMIPP_IT_PIPE1_VSYNC |
               STM32N6_DCMIPP_IT_PIPE1_OVR;
    default:
        /* Pipe2 - callers validate the enum before reaching this helper. */
        return STM32N6_DCMIPP_IT_PIPE2_LINE |
               STM32N6_DCMIPP_IT_PIPE2_FRAME |
               STM32N6_DCMIPP_IT_PIPE2_VSYNC |
               STM32N6_DCMIPP_IT_PIPE2_OVR;
    }
}

static uint32_t tiku_dcmipp_common_interrupt_mask(void) {
    return STM32N6_DCMIPP_IT_AXI_TRANSFER_ERR |
           STM32N6_DCMIPP_IT_PARALLEL_SYNC_ERR;
}

static uint32_t tiku_dcmipp_local_interrupt_bits(tiku_dcmipp_pipe_t pipe,
                                                 uint32_t interrupt_mask) {
    uint32_t local = 0UL;

    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_LINE) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_LINE) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_LINE) != 0UL)) {
        local |= STM32N6_DCMIPP_PxIER_LINEIE;
    }
    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_FRAME) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_FRAME) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_FRAME) != 0UL)) {
        local |= STM32N6_DCMIPP_PxIER_FRAMEIE;
    }
    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_VSYNC) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_VSYNC) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_VSYNC) != 0UL)) {
        local |= STM32N6_DCMIPP_PxIER_VSYNCIE;
    }
    if (pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_LIMIT) != 0UL) {
        local |= STM32N6_DCMIPP_PxIER_LIMITIE;
    }
    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_OVR) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_OVR) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_OVR) != 0UL)) {
        local |= STM32N6_DCMIPP_PxIER_OVRIE;
    }
    return local;
}

static uint32_t tiku_dcmipp_local_clear_bits(tiku_dcmipp_pipe_t pipe,
                                             uint32_t interrupt_mask) {
    uint32_t clear = 0UL;

    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_LINE) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_LINE) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_LINE) != 0UL)) {
        clear |= STM32N6_DCMIPP_PxFCR_CLINEF;
    }
    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_FRAME) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_FRAME) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_FRAME) != 0UL)) {
        clear |= STM32N6_DCMIPP_PxFCR_CFRAMEF;
    }
    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_VSYNC) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_VSYNC) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_VSYNC) != 0UL)) {
        clear |= STM32N6_DCMIPP_PxFCR_CVSYNCF;
    }
    if (pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_LIMIT) != 0UL) {
        clear |= STM32N6_DCMIPP_PxFCR_CLIMITF;
    }
    if ((pipe == PIPE0 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE0_OVR) != 0UL) ||
        (pipe == PIPE1 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE1_OVR) != 0UL) ||
        (pipe == PIPE2 && (interrupt_mask & STM32N6_DCMIPP_IT_PIPE2_OVR) != 0UL)) {
        clear |= STM32N6_DCMIPP_PxFCR_COVRF;
    }
    return clear;
}

static uint8_t tiku_dcmipp_flag_index(uint32_t raw_status) {
    return raw_status == 0UL ? 0U : (uint8_t)(__builtin_ctzl(raw_status) + 1UL);
}

static tiku_event_data_t tiku_dcmipp_pack_event(const tiku_dcmipp_event_t *event) {
    uint32_t pipe = 3UL;

    if (event->pipe == PIPE0 || event->pipe == PIPE1 || event->pipe == PIPE2) {
        pipe = (uint32_t)event->pipe;
    }

    uintptr_t data = (((uint32_t)event->type << TIKU_DCMIPP_EVENT_TYPE_POS) &
                      TIKU_DCMIPP_EVENT_TYPE_MSK) |
                     ((pipe << TIKU_DCMIPP_EVENT_PIPE_POS) &
                      TIKU_DCMIPP_EVENT_PIPE_MSK) |
                     (((uint32_t)event->virtual_channel << TIKU_DCMIPP_EVENT_VC_POS) &
                      TIKU_DCMIPP_EVENT_VC_MSK) |
                     (((uint32_t)event->error << TIKU_DCMIPP_EVENT_ERR_POS) &
                      TIKU_DCMIPP_EVENT_ERR_MSK) |
                     (((uint32_t)tiku_dcmipp_flag_index(event->raw_status) <<
                       TIKU_DCMIPP_EVENT_FLAG_POS) & TIKU_DCMIPP_EVENT_FLAG_MSK);

    return (tiku_event_data_t)data;
}

static void tiku_dcmipp_post_event(tiku_dcmipp_event_type_t type,
                                   tiku_dcmipp_pipe_t pipe,
                                   uint8_t virtual_channel,
                                   tiku_dcmipp_error_code_t error,
                                   uint32_t raw_status) {
    tiku_dcmipp_event_t event = {
        .type = type,
        .pipe = pipe,
        .virtual_channel = virtual_channel,
        .error = error,
        .raw_status = raw_status,
    };

    if (!tiku_process_post(TIKU_PROCESS_BROADCAST, TIKU_DCMIPP_EVENT_ID,
                           tiku_dcmipp_pack_event(&event))) {
        dcmipp.event_drops++;
    }
}

static void tiku_dcmipp_post_pipe_events(tiku_dcmipp_pipe_t pipe,
                                         uint32_t status,
                                         uint32_t line,
                                         uint32_t frame,
                                         uint32_t vsync,
                                         uint32_t limit,
                                         uint32_t overrun) {
    if ((status & line) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_LINE, pipe, 0U,
                               DCMIPP_ERROR_NONE, line);
    }
    if ((status & frame) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_FRAME_COMPLETE, pipe, 0U,
                               DCMIPP_ERROR_NONE, frame);
    }
    if ((status & vsync) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_VSYNC, pipe, 0U,
                               DCMIPP_ERROR_NONE, vsync);
    }
    if (limit != 0UL && (status & limit) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_LIMIT, pipe, 0U,
                               DCMIPP_ERROR_NONE, limit);
    }
    if ((status & overrun) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, pipe, 0U,
                               DCMIPP_ERROR_PIPE_OVERRUN, overrun);
    }
}

static uint32_t tiku_dcmipp_common_from_local(tiku_dcmipp_pipe_t pipe,
                                              uint32_t status) {
    if (pipe == PIPE0) {
        uint32_t common = 0UL;
        if ((status & STM32N6_DCMIPP_PxSR_LINEF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE0_LINE;
        }
        if ((status & STM32N6_DCMIPP_PxSR_FRAMEF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE0_FRAME;
        }
        if ((status & STM32N6_DCMIPP_PxSR_VSYNCF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE0_VSYNC;
        }
        if ((status & STM32N6_DCMIPP_PxSR_LIMITF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE0_LIMIT;
        }
        if ((status & STM32N6_DCMIPP_PxSR_OVRF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE0_OVR;
        }
        return common;
    }
    if (pipe == PIPE1) {
        uint32_t common = 0UL;
        if ((status & STM32N6_DCMIPP_PxSR_LINEF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE1_LINE;
        }
        if ((status & STM32N6_DCMIPP_PxSR_FRAMEF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE1_FRAME;
        }
        if ((status & STM32N6_DCMIPP_PxSR_VSYNCF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE1_VSYNC;
        }
        if ((status & STM32N6_DCMIPP_PxSR_OVRF) != 0UL) {
            common |= STM32N6_DCMIPP_FLAG_PIPE1_OVR;
        }
        return common;
    }

    uint32_t common = 0UL;
    if ((status & STM32N6_DCMIPP_PxSR_LINEF) != 0UL) {
        common |= STM32N6_DCMIPP_FLAG_PIPE2_LINE;
    }
    if ((status & STM32N6_DCMIPP_PxSR_FRAMEF) != 0UL) {
        common |= STM32N6_DCMIPP_FLAG_PIPE2_FRAME;
    }
    if ((status & STM32N6_DCMIPP_PxSR_VSYNCF) != 0UL) {
        common |= STM32N6_DCMIPP_FLAG_PIPE2_VSYNC;
    }
    if ((status & STM32N6_DCMIPP_PxSR_OVRF) != 0UL) {
        common |= STM32N6_DCMIPP_FLAG_PIPE2_OVR;
    }
    return common;
}

static void tiku_dcmipp_post_csi_vc_events(uint32_t status,
                                           uint32_t base_flag,
                                           tiku_dcmipp_event_type_t type) {
    if ((status & (base_flag << 0U)) != 0UL) {
        tiku_dcmipp_post_event(type, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_NONE, base_flag << 0U);
    }
    if ((status & (base_flag << 1U)) != 0UL) {
        tiku_dcmipp_post_event(type, DCMIPP_PIPE_NONE, 1U,
                               DCMIPP_ERROR_NONE, base_flag << 1U);
    }
    if ((status & (base_flag << 2U)) != 0UL) {
        tiku_dcmipp_post_event(type, DCMIPP_PIPE_NONE, 2U,
                               DCMIPP_ERROR_NONE, base_flag << 2U);
    }
    if ((status & (base_flag << 3U)) != 0UL) {
        tiku_dcmipp_post_event(type, DCMIPP_PIPE_NONE, 3U,
                               DCMIPP_ERROR_NONE, base_flag << 3U);
    }
}

static void tiku_dcmipp_post_csi_lane_error(uint32_t status,
                                            uint32_t flag,
                                            uint8_t lane) {
    if ((status & flag) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, lane,
                               DCMIPP_ERROR_CSI_DPHY_LANE, flag);
    }
}

int tiku_dcmipp_arch_event_from_data(tiku_event_data_t data,
                                     tiku_dcmipp_event_t *out) {
    if (out == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }

    uintptr_t word = (uintptr_t)data;
    uint32_t pipe = (uint32_t)((word & TIKU_DCMIPP_EVENT_PIPE_MSK) >>
                               TIKU_DCMIPP_EVENT_PIPE_POS);
    uint32_t flag = (uint32_t)((word & TIKU_DCMIPP_EVENT_FLAG_MSK) >>
                               TIKU_DCMIPP_EVENT_FLAG_POS);

    out->type = (tiku_dcmipp_event_type_t)
        ((word & TIKU_DCMIPP_EVENT_TYPE_MSK) >> TIKU_DCMIPP_EVENT_TYPE_POS);
    out->pipe = (pipe == 3UL) ? DCMIPP_PIPE_NONE : (tiku_dcmipp_pipe_t)pipe;
    out->virtual_channel = (uint8_t)
        ((word & TIKU_DCMIPP_EVENT_VC_MSK) >> TIKU_DCMIPP_EVENT_VC_POS);
    out->error = (tiku_dcmipp_error_code_t)
        ((word & TIKU_DCMIPP_EVENT_ERR_MSK) >> TIKU_DCMIPP_EVENT_ERR_POS);
    out->raw_status = (flag == 0UL) ? 0UL : (1UL << (flag - 1UL));
    return TIKU_DCMIPP_OK;
}

/* The DCMIPP writes frames into the secure AXISRAM alias as its own bus
 * master, not with the CPU's attributes. RISAF drops writes that are not
 * secure, privileged and CID 1, with no error on the DCMIPP side, so set the
 * master up the way the LTDC and HPDMA drivers do. RIFSC is written by the
 * secure CPU; read back to catch a locked RIMC (GLOCK). */
static int tiku_dcmipp_master_attr_configure(void) {
    const uint32_t attr_mask = STM32N6_RIFSC_RIMC_ATTR_MCID |
                               STM32N6_RIFSC_RIMC_ATTR_SEC |
                               STM32N6_RIFSC_RIMC_ATTR_PRIV;
    const uint32_t attr = STM32N6_RIFSC_RIMC_ATTR_SEC |
                          STM32N6_RIFSC_RIMC_ATTR_PRIV |
                          STM32N6_RIFSC_RIMC_ATTR_CID1;
    const uintptr_t reg =
        STM32N6_RIFSC_RIMC_ATTR(STM32N6_RIFSC_RIMC_DCMIPP);

    /* The peripheral must be secure and privileged as well (ST's example calls
     * HAL_RIF_RISC_SetSlaveSecureAttributes for the DCMIPP next to the RIMC
     * setup): without it the master's frame writes never reach AXISRAM, while
     * the pipes still count lines and raise frame-complete. */
    TIKU_DCMIPP_REG_WRITE(STM32N6_RIFSC_RISC_SECCFGR2,
                          TIKU_DCMIPP_REG_READ(STM32N6_RIFSC_RISC_SECCFGR2) |
                              STM32N6_RIFSC_DCMIPP_BIT);
    TIKU_DCMIPP_REG_WRITE(STM32N6_RIFSC_RISC_PRIVCFGR2,
                          TIKU_DCMIPP_REG_READ(STM32N6_RIFSC_RISC_PRIVCFGR2) |
                              STM32N6_RIFSC_DCMIPP_BIT);
    if ((TIKU_DCMIPP_REG_READ(STM32N6_RIFSC_RISC_SECCFGR2) &
         STM32N6_RIFSC_DCMIPP_BIT) == 0UL ||
        (TIKU_DCMIPP_REG_READ(STM32N6_RIFSC_RISC_PRIVCFGR2) &
         STM32N6_RIFSC_DCMIPP_BIT) == 0UL) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_NOT_ALLOWED);
    }

    TIKU_DCMIPP_REG_WRITE(reg, attr);
    if ((TIKU_DCMIPP_REG_READ(reg) & attr_mask) != attr) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_NOT_ALLOWED);
    }
    return TIKU_DCMIPP_OK;
}

/* PLL1 runs at 1200 MHz. IC17 /4 gives the 300 MHz DCMIPP kernel clock and
 * IC18 /60 the 20 MHz CSI-2 kernel clock, the values ST's BSP programs. The
 * D-PHY configuration-clock range written in config_csi (CCFR 0x28) assumes
 * that 20 MHz. */
#define TIKU_DCMIPP_IC17_DIVIDER    4UL
#define TIKU_DCMIPP_IC18_DIVIDER    60UL

/** @brief Start one IC divider from PLL1 unless something already runs it. */
static void tiku_dcmipp_ic_start(uint32_t index, uint32_t divider) {
    uint32_t enable_bit = 1UL << (index - 1U);
    uint32_t cfg;

    if ((TIKU_REG32(STM32N6_RCC_DIVENR) & enable_bit) != 0UL) {
        return;
    }
    cfg = TIKU_REG32(STM32N6_RCC_ICCFGR(index));
    cfg &= ~(STM32N6_IC_INT_MSK | STM32N6_IC_SEL_MSK);
    cfg |= (STM32N6_IC_SEL_PLL1 << STM32N6_IC_SEL_POS) |
           ((divider - 1UL) << STM32N6_IC_INT_POS);
    TIKU_REG32(STM32N6_RCC_ICCFGR(index)) = cfg;
    TIKU_REG32(STM32N6_RCC_DIVENR) |= enable_bit;
}

int tiku_dcmipp_arch_enable(void) {
    TIKU_REG32(STM32N6_RCC_APB5ENR) |=
        STM32N6_RCC_APB5ENR_DCMIPP | STM32N6_RCC_APB5ENR_CSI;
    (void)TIKU_REG32(STM32N6_RCC_APB5ENR);

    /* Both kernel clocks come up disabled, so without this the CSI host and
     * D-PHY see no clock and never report a frame or an error. */
    tiku_dcmipp_ic_start(STM32N6_RCC_IC17, TIKU_DCMIPP_IC17_DIVIDER);
    tiku_dcmipp_ic_start(STM32N6_RCC_IC18, TIKU_DCMIPP_IC18_DIVIDER);
    TIKU_REG32(STM32N6_RCC_CCIPR1) =
        (TIKU_REG32(STM32N6_RCC_CCIPR1) & ~STM32N6_RCC_CCIPR1_DCMIPPSEL_MSK) |
        STM32N6_RCC_CCIPR1_DCMIPPSEL_IC17;

    if (tiku_dcmipp_master_attr_configure() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(STM32N6_DCMIPP_CMFCR) = STM32N6_DCMIPP_CMFCR_ALL;
    TIKU_REG32(STM32N6_DCMIPP_PRFCR) = STM32N6_DCMIPP_PRFCR_CERRF;
    TIKU_REG32(STM32N6_CSI_FCR0) = STM32N6_CSI_FCR0_ALL;
    TIKU_REG32(STM32N6_CSI_FCR1) = STM32N6_CSI_FCR1_ALL;
    dcmipp.peripheral_enabled = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_disable(void) {
    tiku_dcmipp_nvic_disable(STM32N6_IRQ_DCMIPP);
    tiku_dcmipp_nvic_disable(STM32N6_IRQ_CSI);

    TIKU_REG32(STM32N6_DCMIPP_CMIER) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_PRIER) = 0UL;
    TIKU_REG32(STM32N6_CSI_IER0) = 0UL;
    TIKU_REG32(STM32N6_CSI_IER1) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_PRCR) &= ~STM32N6_DCMIPP_PRCR_ENABLE;

    for (uint8_t i = 0; i < DCMIPP_PIPE_COUNT; i++) {
        TIKU_REG32(tiku_dcmipp_fctcr((tiku_dcmipp_pipe_t)i)) &=
            ~STM32N6_DCMIPP_PxFCTCR_CPTREQ;
        TIKU_REG32(tiku_dcmipp_fscr((tiku_dcmipp_pipe_t)i)) &=
            ~tiku_dcmipp_pipen_bit((tiku_dcmipp_pipe_t)i);
        dcmipp.pipe[i].enabled = 0U;
        dcmipp.pipe[i].running = 0U;
        dcmipp.pipe[i].continuous = 0U;
        dcmipp.pipe[i].double_buffer = 0U;
        dcmipp.postproc_enabled[i] = 0U;
    }

    dcmipp.input_enabled = 0U;
    dcmipp.isp_enabled = 0U;
    dcmipp.peripheral_enabled = 0U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_set_input(tiku_dcmipp_input_t input_type) {
    if (input_type != DCMIPP_INPUT_PARALLEL && input_type != DCMIPP_INPUT_CSI) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    if (input_type == DCMIPP_INPUT_CSI) {
        TIKU_REG32(STM32N6_DCMIPP_CMCR) |= STM32N6_DCMIPP_CMCR_INSEL;
    } else {
        TIKU_REG32(STM32N6_DCMIPP_CMCR) &= ~STM32N6_DCMIPP_CMCR_INSEL;
    }
    dcmipp.input = input_type;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_config_csi(const tiku_dcmipp_csi_config_t *config) {
    if (config == NULL ||
        (config->lane_count != TIKU_DCMIPP_CSI_ONE_DATA_LANE &&
         config->lane_count != TIKU_DCMIPP_CSI_TWO_DATA_LANES) ||
        (config->lane_mapping != TIKU_DCMIPP_CSI_LANES_PHYSICAL &&
         config->lane_mapping != TIKU_DCMIPP_CSI_LANES_INVERTED) ||
        (config->data_type != TIKU_DCMIPP_CSI_DT_RAW10 &&
         config->data_type != TIKU_DCMIPP_CSI_DT_RAW12) ||
        config->virtual_channel > 3U ||
        (uint32_t)config->phy_bitrate >=
            (uint32_t)(sizeof(tiku_dcmipp_snps_freqs) /
                       sizeof(tiku_dcmipp_snps_freqs[0]))) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_INVALID);
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    const tiku_dcmipp_snps_freq_t *freq =
        &tiku_dcmipp_snps_freqs[(uint32_t)config->phy_bitrate];
    uint32_t lane0_map =
        (config->lane_mapping == TIKU_DCMIPP_CSI_LANES_PHYSICAL) ? 1UL : 2UL;
    uint32_t lane1_map =
        (config->lane_mapping == TIKU_DCMIPP_CSI_LANES_PHYSICAL) ? 2UL : 1UL;
    uint32_t lane_error_mask = STM32N6_CSI_IER1_ALL;
    uint32_t active_lane_error_mask;

    /* Switch away from parallel capture while the CSI host is initialized. */
    TIKU_REG32(STM32N6_DCMIPP_PRCR) &= ~STM32N6_DCMIPP_PRCR_ENABLE;
    TIKU_REG32(STM32N6_DCMIPP_CMCR) |= STM32N6_DCMIPP_CMCR_INSEL;

    TIKU_REG32(STM32N6_CSI_CR) &= ~STM32N6_CSI_CR_CSIEN;
    TIKU_REG32(STM32N6_CSI_LMCFGR) =
        ((uint32_t)config->lane_count << STM32N6_CSI_LMCFGR_LANENB_POS) |
        (lane0_map << STM32N6_CSI_LMCFGR_DL0MAP_POS) |
        (lane1_map << STM32N6_CSI_LMCFGR_DL1MAP_POS);
    TIKU_REG32(STM32N6_CSI_CR) |= STM32N6_CSI_CR_CSIEN;

    /* Program the selected VC's CSI-2 data type and packed word width. */
    uintptr_t vc_cfgr2 = STM32N6_CSI_VCxCFGR2(config->virtual_channel);
    uint32_t vc_value = TIKU_REG32(vc_cfgr2);
    vc_value &= ~(STM32N6_CSI_VCxCFGR2_DT1_MSK |
                  STM32N6_CSI_VCxCFGR2_DT1FT_MSK |
                  STM32N6_CSI_VCxCFGR2_DT2_MSK |
                  STM32N6_CSI_VCxCFGR2_DT2FT_MSK);
    vc_value |= ((uint32_t)config->data_type &
                 STM32N6_CSI_VCxCFGR2_DT1_MSK) |
                ((config->data_type == TIKU_DCMIPP_CSI_DT_RAW10 ? 3UL : 4UL)
                 << STM32N6_CSI_VCxCFGR2_DT1FT_POS);
    TIKU_REG32(vc_cfgr2) = vc_value;

    /* Enable the data type: VCxCFGR2 only defines DT1, VCxCFGR1 turns it on.
     * Without this every pixel line is dropped and only frame sync packets get
     * through. Same as ST's HAL_DCMIPP_CSI_SetVCConfig: all data types, with
     * the common word-width format (BPP10 = 3, BPP12 = 4). */
    TIKU_REG32(STM32N6_CSI_VCxCFGR1(config->virtual_channel)) =
        ((config->data_type == TIKU_DCMIPP_CSI_DT_RAW10 ? 3UL : 4UL)
         << STM32N6_CSI_VCxCFGR1_CDTFT_POS) |
        STM32N6_CSI_VCxCFGR1_ALLDT;

    /* Enable shared CSI error and short-packet interrupt sources. */
    TIKU_REG32(STM32N6_CSI_IER0) |=
        STM32N6_CSI_IER0_CCFIFOFIE | STM32N6_CSI_IER0_SYNCERRIE |
        STM32N6_CSI_IER0_SPKTERRIE | STM32N6_CSI_IER0_IDERRIE |
        STM32N6_CSI_IER0_SPKTIE;

    /* Enable D-PHY errors for only the physical receive lanes in use. */
    if (config->lane_count == TIKU_DCMIPP_CSI_ONE_DATA_LANE) {
        if (config->lane_mapping == TIKU_DCMIPP_CSI_LANES_PHYSICAL) {
            active_lane_error_mask = STM32N6_CSI_IER1_ESOTDL0IE |
                STM32N6_CSI_IER1_ESOTSYNCDL0IE | STM32N6_CSI_IER1_EESCDL0IE |
                STM32N6_CSI_IER1_ESYNCESCDL0IE | STM32N6_CSI_IER1_ECTRLDL0IE;
        } else {
            active_lane_error_mask = STM32N6_CSI_IER1_ESOTDL1IE |
                STM32N6_CSI_IER1_ESOTSYNCDL1IE | STM32N6_CSI_IER1_EESCDL1IE |
                STM32N6_CSI_IER1_ESYNCESCDL1IE | STM32N6_CSI_IER1_ECTRLDL1IE;
        }
    } else {
        active_lane_error_mask = lane_error_mask;
    }
    TIKU_REG32(STM32N6_CSI_IER1) &= ~lane_error_mask;
    TIKU_REG32(STM32N6_CSI_IER1) |= active_lane_error_mask;

    /* Reset and configure the Synopsys D-PHY using ST's test-interface flow. */
    TIKU_REG32(STM32N6_CSI_PHY_PRCR) &= ~STM32N6_CSI_PHY_PEN;
    TIKU_REG32(STM32N6_CSI_PCR) = 0UL;
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = STM32N6_CSI_PHY_PTCR0_TCKEN;
    tiku_cpu_stm32n6_delay_ms(1U);
    TIKU_REG32(STM32N6_CSI_PHY_PTCR0) = 0UL;

    TIKU_REG32(STM32N6_CSI_PHY_PFCR) =
        (0x28UL << STM32N6_CSI_PHY_PFCR_CCFR_POS) |
        ((uint32_t)freq->hsfreqrange << STM32N6_CSI_PHY_PFCR_HSFR_POS);
    tiku_dcmipp_csi_phy_write(0x00U, 0x08U, 0x38U);
    tiku_dcmipp_csi_phy_write(0x00U, 0xE4U, 0x11U);
    tiku_dcmipp_csi_phy_write(0x00U, 0xE3U,
                              (uint8_t)(freq->osc_freq_target >> 8));
    tiku_dcmipp_csi_phy_write(0x00U, 0xE3U,
                              (uint8_t)(freq->osc_freq_target & 0xFFU));
    TIKU_REG32(STM32N6_CSI_PHY_PFCR) =
        (0x28UL << STM32N6_CSI_PHY_PFCR_CCFR_POS) |
        ((uint32_t)freq->hsfreqrange << STM32N6_CSI_PHY_PFCR_HSFR_POS) |
        STM32N6_CSI_PHY_PFCR_DLD;

    uint32_t phy_control = STM32N6_CSI_PCR_DL0EN | STM32N6_CSI_PCR_CLEN |
                           STM32N6_CSI_PCR_NPWRDOWN;
    if (config->lane_count == TIKU_DCMIPP_CSI_TWO_DATA_LANES) {
        phy_control |= STM32N6_CSI_PCR_DL1EN;
    }
    TIKU_REG32(STM32N6_CSI_PCR) = phy_control;
    TIKU_REG32(STM32N6_CSI_PHY_PRCR) |= STM32N6_CSI_PHY_PEN;
    TIKU_REG32(STM32N6_CSI_PHY_PMCR) = 0UL;

    tiku_dcmipp_nvic_enable(STM32N6_IRQ_CSI);
    dcmipp.input = DCMIPP_INPUT_CSI;
    dcmipp.input_enabled = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_axi_plug_configure_pipe2(void) {
    uint32_t spins;
    uint32_t value;

    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    /* IPGR2.PSTART requests a short freeze; IPGR3.IDLE acknowledges it. */
    TIKU_DCMIPP_REG_WRITE(STM32N6_DCMIPP_IPGR2,
                          TIKU_DCMIPP_REG_READ(STM32N6_DCMIPP_IPGR2) |
                              STM32N6_DCMIPP_IPGR2_PSTART);
    for (spins = 0UL; spins < TIKU_DCMIPP_AXI_WAIT_SPINS; ++spins) {
        if ((TIKU_DCMIPP_REG_READ(STM32N6_DCMIPP_IPGR3) &
             STM32N6_DCMIPP_IPGR3_IDLE) != 0UL) {
            break;
        }
    }
    if (spins == TIKU_DCMIPP_AXI_WAIT_SPINS) {
        TIKU_DCMIPP_REG_WRITE(STM32N6_DCMIPP_IPGR2,
                              TIKU_DCMIPP_REG_READ(STM32N6_DCMIPP_IPGR2) &
                                  ~STM32N6_DCMIPP_IPGR2_PSTART);
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_BUSY);
    }

    value = TIKU_DCMIPP_REG_READ(STM32N6_DCMIPP_IPGR1);
    value &= ~STM32N6_DCMIPP_IPGR1_MEMORYPAGE_MSK;
    TIKU_DCMIPP_REG_WRITE(STM32N6_DCMIPP_IPGR1, value);

    /* Client 5 is Pipe 2. Keep SVCMAPPING at reset (hardware-owned), give
     * this single active client the default arbitration weight, and retain
     * its full reset FIFO span. A 128-byte burst is the documented maximum. */
    TIKU_DCMIPP_REG_WRITE(STM32N6_DCMIPP_IPC5R1,
                          (4UL << STM32N6_DCMIPP_IPC5R1_TRAFFIC_POS) |
                              (0UL << STM32N6_DCMIPP_IPC5R1_OTR_POS));
    value = TIKU_DCMIPP_REG_READ(STM32N6_DCMIPP_IPC5R2);
    value &= ~STM32N6_DCMIPP_IPC5R2_WLRU_MSK;
    value |= 1UL << STM32N6_DCMIPP_IPC5R2_WLRU_POS;
    TIKU_DCMIPP_REG_WRITE(STM32N6_DCMIPP_IPC5R2, value);
    TIKU_DCMIPP_REG_WRITE(STM32N6_DCMIPP_IPC5R3,
                          (448UL << STM32N6_DCMIPP_IPC5R3_DPREGSTART_POS) |
                              (639UL << STM32N6_DCMIPP_IPC5R3_DPREGEND_POS));

    TIKU_DCMIPP_REG_WRITE(STM32N6_DCMIPP_IPGR2,
                          TIKU_DCMIPP_REG_READ(STM32N6_DCMIPP_IPGR2) &
                              ~STM32N6_DCMIPP_IPGR2_PSTART);
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_config_parallel(
    const tiku_dcmipp_parallel_config_t *config) {
    if (config == NULL || !tiku_dcmipp_valid_parallel_format(config->format) ||
        config->vsync_polarity > TIKU_DCMIPP_PARALLEL_POLARITY_HIGH ||
        config->hsync_polarity > TIKU_DCMIPP_PARALLEL_POLARITY_HIGH ||
        config->pixel_clock_polarity > TIKU_DCMIPP_PARALLEL_PCLK_RISING ||
        config->extended_data_mode > TIKU_DCMIPP_PARALLEL_DATA_16_BITS ||
        config->sync_mode > TIKU_DCMIPP_PARALLEL_SYNC_EMBEDDED ||
        config->swap_bits > 1U || config->swap_cycles > 1U) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_INVALID);
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    uint32_t prcr = (uint32_t)config->format;
    if (config->vsync_polarity == TIKU_DCMIPP_PARALLEL_POLARITY_HIGH) {
        prcr |= STM32N6_DCMIPP_PRCR_VSPOL;
    }
    if (config->hsync_polarity == TIKU_DCMIPP_PARALLEL_POLARITY_HIGH) {
        prcr |= STM32N6_DCMIPP_PRCR_HSPOL;
    }
    if (config->pixel_clock_polarity == TIKU_DCMIPP_PARALLEL_PCLK_RISING) {
        prcr |= STM32N6_DCMIPP_PRCR_PCKPOL;
    }
    prcr |= (uint32_t)config->extended_data_mode << STM32N6_DCMIPP_PRCR_EDM_POS;
    if (config->sync_mode == TIKU_DCMIPP_PARALLEL_SYNC_EMBEDDED) {
        prcr |= STM32N6_DCMIPP_PRCR_ESS;
    }
    if (config->swap_cycles != 0U) {
        prcr |= STM32N6_DCMIPP_PRCR_SWAPCYCLES;
    }
    if (config->swap_bits != 0U) {
        prcr |= STM32N6_DCMIPP_PRCR_SWAPBITS;
    }

    /* Parallel selection disables and powers down the CSI host and PHY. */
    TIKU_REG32(STM32N6_CSI_CR) &= ~STM32N6_CSI_CR_CSIEN;
    TIKU_REG32(STM32N6_CSI_PHY_PRCR) &= ~STM32N6_CSI_PHY_PEN;
    TIKU_REG32(STM32N6_CSI_PCR) = 0UL;
    TIKU_REG32(STM32N6_CSI_IER0) = 0UL;
    TIKU_REG32(STM32N6_CSI_IER1) = 0UL;
    tiku_dcmipp_nvic_disable(STM32N6_IRQ_CSI);

    TIKU_REG32(STM32N6_DCMIPP_PRCR) = prcr;
    if (config->sync_mode == TIKU_DCMIPP_PARALLEL_SYNC_EMBEDDED) {
        TIKU_REG32(STM32N6_DCMIPP_PRESCR) =
            ((uint32_t)config->sync_codes.frame_start) |
            ((uint32_t)config->sync_codes.line_start << STM32N6_DCMIPP_PRESCR_LSC_POS) |
            ((uint32_t)config->sync_codes.line_end << STM32N6_DCMIPP_PRESCR_LEC_POS) |
            ((uint32_t)config->sync_codes.frame_end << STM32N6_DCMIPP_PRESCR_FEC_POS);
        TIKU_REG32(STM32N6_DCMIPP_PRESUR) = 0xFFFFFFFFUL;
    }
    TIKU_REG32(STM32N6_DCMIPP_CMFCR) =
        STM32N6_DCMIPP_CLEAR_PARALLEL_SYNC_ERROR;
    TIKU_REG32(STM32N6_DCMIPP_PRIER) |= STM32N6_DCMIPP_PRIER_ERRIE;
    TIKU_REG32(STM32N6_DCMIPP_CMIER) |=
        STM32N6_DCMIPP_IT_PARALLEL_SYNC_ERR;
    TIKU_REG32(STM32N6_DCMIPP_PRCR) |= STM32N6_DCMIPP_PRCR_ENABLE;
    TIKU_REG32(STM32N6_DCMIPP_CMCR) &= ~STM32N6_DCMIPP_CMCR_INSEL;

    tiku_dcmipp_nvic_enable(STM32N6_IRQ_DCMIPP);
    dcmipp.input = DCMIPP_INPUT_PARALLEL;
    dcmipp.input_enabled = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_enable_input(void) {
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    if (dcmipp.input == DCMIPP_INPUT_PARALLEL) {
        TIKU_REG32(STM32N6_DCMIPP_PRCR) |= STM32N6_DCMIPP_PRCR_ENABLE;
    } else {
        TIKU_REG32(STM32N6_CSI_CR) |= STM32N6_CSI_CR_CSIEN;
        TIKU_REG32(STM32N6_CSI_PHY_PRCR) |= STM32N6_CSI_PHY_PEN;
    }
    dcmipp.input_enabled = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_disable_input(void) {
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    if (dcmipp.input == DCMIPP_INPUT_PARALLEL) {
        TIKU_REG32(STM32N6_DCMIPP_PRCR) &= ~STM32N6_DCMIPP_PRCR_ENABLE;
    } else {
        TIKU_REG32(STM32N6_CSI_CR) &= ~STM32N6_CSI_CR_CSIEN;
        TIKU_REG32(STM32N6_CSI_PHY_PRCR) &= ~STM32N6_CSI_PHY_PEN;
    }
    dcmipp.input_enabled = 0U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_set_pipe_diff(tiku_dcmipp_pipe_diff_source_t source) {
    if (source != DCMIPP_PIPEDIFF_COUPLED &&
        source != DCMIPP_PIPEDIFF_INDEPENDENT) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    if (source == DCMIPP_PIPEDIFF_INDEPENDENT) {
        TIKU_REG32(STM32N6_DCMIPP_P1FSCR) |= STM32N6_DCMIPP_P1FSCR_PIPEDIFF;
    } else {
        TIKU_REG32(STM32N6_DCMIPP_P1FSCR) &= ~STM32N6_DCMIPP_P1FSCR_PIPEDIFF;
    }
    dcmipp.pipe_diff = source;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_configure(tiku_dcmipp_pipe_t pipe,
                                    const tiku_dcmipp_pipe_config_t *config) {
    if (!tiku_dcmipp_valid_pipe(pipe) || config == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    uint32_t pipen = TIKU_REG32(tiku_dcmipp_fscr(pipe)) & tiku_dcmipp_pipen_bit(pipe);
    TIKU_REG32(tiku_dcmipp_fscr(pipe)) =
        tiku_dcmipp_pipe_fscr_value(pipe, config, 0U) | pipen;
    TIKU_REG32(tiku_dcmipp_fctcr(pipe)) =
        (TIKU_REG32(tiku_dcmipp_fctcr(pipe)) & ~STM32N6_DCMIPP_PxFCTCR_FRATE_MSK) |
        (config->frame_rate & STM32N6_DCMIPP_PxFCTCR_FRATE_MSK);
    TIKU_REG32(tiku_dcmipp_crop_start_reg(pipe)) = config->crop_start;
    TIKU_REG32(tiku_dcmipp_crop_size_reg(pipe)) = config->crop_size;
    if (pipe == PIPE0) {
        TIKU_REG32(STM32N6_DCMIPP_P0DCLMTR) =
            config->dump_limit & STM32N6_DCMIPP_P0DCLMTR_LIMIT_MSK;
    }

    uint32_t dbm = TIKU_REG32(tiku_dcmipp_ppcr(pipe)) & STM32N6_DCMIPP_PxPPCR_DBM;
    TIKU_REG32(tiku_dcmipp_ppcr(pipe)) = config->pixel_packer | dbm;
    dcmipp.pipe[pipe].config = *config;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_enable(tiku_dcmipp_pipe_t pipe) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_fscr(pipe)) |= tiku_dcmipp_pipen_bit(pipe);
    dcmipp.pipe[pipe].enabled = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_disable(tiku_dcmipp_pipe_t pipe) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_fctcr(pipe)) &= ~STM32N6_DCMIPP_PxFCTCR_CPTREQ;
    TIKU_REG32(tiku_dcmipp_fscr(pipe)) &= ~tiku_dcmipp_pipen_bit(pipe);
    dcmipp.pipe[pipe].enabled = 0U;
    dcmipp.pipe[pipe].running = 0U;
    dcmipp.pipe[pipe].continuous = 0U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_start_capture(tiku_dcmipp_pipe_t pipe,
                                        const tiku_dcmipp_plane_address_t *address,
                                        uint8_t virtual_channel,
                                        tiku_dcmipp_capture_mode_t capture_mode) {
    if (!tiku_dcmipp_valid_pipe(pipe) ||
        !tiku_dcmipp_valid_plane_address(pipe, address) ||
        virtual_channel > 3U ||
        (capture_mode != DCMIPP_CAPTURE_SNAPSHOT &&
         capture_mode != DCMIPP_CAPTURE_CONTINUOUS)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }
    if (!dcmipp.pipe[pipe].enabled) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_PIPE_DISABLED);
    }
    if (dcmipp.pipe[pipe].running && dcmipp.pipe[pipe].continuous) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_BUSY);
    }

    /* A CSI virtual channel forwards nothing to the pipes until it is started
     * (ST's HAL does this in HAL_DCMIPP_CSI_PIPE_Start). The channel begins at
     * the next frame boundary, so this is also safe before the sensor streams.
     * ST then polls SR0.VCnSTATEF, but that only goes active once frames are
     * arriving, so it is left to the caller (see STM32N6_CSI_SR0_VCSTATEF). */
    if (dcmipp.input == DCMIPP_INPUT_CSI) {
        TIKU_REG32(STM32N6_CSI_CR) |= STM32N6_CSI_CR_VCSTART(virtual_channel);
    }

    tiku_dcmipp_write_pipe_addresses(pipe, address, 0U);
    TIKU_REG32(tiku_dcmipp_fscr(pipe)) =
        tiku_dcmipp_pipe_fscr_value(pipe, &dcmipp.pipe[pipe].config,
                                    virtual_channel) |
        tiku_dcmipp_pipen_bit(pipe);

    uint32_t fctcr = TIKU_REG32(tiku_dcmipp_fctcr(pipe));
    fctcr &= ~(STM32N6_DCMIPP_PxFCTCR_CPTMODE | STM32N6_DCMIPP_PxFCTCR_CPTREQ);
    if (capture_mode == DCMIPP_CAPTURE_SNAPSHOT) {
        fctcr |= STM32N6_DCMIPP_PxFCTCR_CPTMODE;
    }
    TIKU_REG32(tiku_dcmipp_fctcr(pipe)) = fctcr | STM32N6_DCMIPP_PxFCTCR_CPTREQ;

    // Setup pipe state
    dcmipp.pipe[pipe].address = *address;
    dcmipp.pipe[pipe].capture_mode = capture_mode;
    dcmipp.pipe[pipe].continuous = (capture_mode == DCMIPP_CAPTURE_CONTINUOUS);
    dcmipp.pipe[pipe].running = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_flow_selection_configure(tiku_dcmipp_pipe_t pipe,
                                                   uint32_t flow_selection_flags) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    dcmipp.pipe[pipe].config.flow_selection_flags = flow_selection_flags;
    TIKU_REG32(tiku_dcmipp_fscr(pipe)) =
        tiku_dcmipp_pipe_fscr_value(pipe, &dcmipp.pipe[pipe].config, 0U) |
        (TIKU_REG32(tiku_dcmipp_fscr(pipe)) & tiku_dcmipp_pipen_bit(pipe));
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_flow_selection_disable(tiku_dcmipp_pipe_t pipe) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    dcmipp.pipe[pipe].config.flow_selection_flags = 0UL;
    TIKU_REG32(tiku_dcmipp_fscr(pipe)) &= tiku_dcmipp_pipen_bit(pipe);
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_crop_configure(tiku_dcmipp_pipe_t pipe,
                                         const tiku_dcmipp_crop_config_t *config) {
    if (!tiku_dcmipp_valid_pipe(pipe) || config == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_crop_start_reg(pipe)) = config->start;
    TIKU_REG32(tiku_dcmipp_crop_size_reg(pipe)) =
        config->size | TIKU_DCMIPP_POSTPROC_ENABLE;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_crop_rect(tiku_dcmipp_pipe_t pipe,
                                    uint16_t x, uint16_t y,
                                    uint16_t width, uint16_t height) {
    tiku_dcmipp_crop_config_t config;

    if ((pipe != PIPE1 && pipe != PIPE2) || width == 0U || height == 0U ||
        x > 4094U || y > 4094U || width > 4094U || height > 4094U) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_INVALID);
    }
    config.start = (uint32_t)x | ((uint32_t)y << 16);
    config.size = (uint32_t)width | ((uint32_t)height << 16) |
                  TIKU_DCMIPP_POSTPROC_ENABLE;
    return tiku_dcmipp_arch_pipe_crop_configure(pipe, &config);
}

int tiku_dcmipp_arch_pipe_crop_disable(tiku_dcmipp_pipe_t pipe) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_crop_start_reg(pipe)) = 0UL;
    TIKU_REG32(tiku_dcmipp_crop_size_reg(pipe)) = 0UL;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_dump_limit_configure(tiku_dcmipp_pipe_t pipe,
                                               const tiku_dcmipp_dump_limit_config_t *config) {
    if (pipe != PIPE0 || config == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    uint32_t value = config->limit_words & STM32N6_DCMIPP_P0DCLMTR_LIMIT_MSK;

    if (config->enable != 0U) {
        value |= STM32N6_DCMIPP_P0DCLMTR_ENABLE;
    }
    TIKU_REG32(STM32N6_DCMIPP_P0DCLMTR) = value;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_dump_limit_disable(tiku_dcmipp_pipe_t pipe) {
    if (pipe != PIPE0) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(STM32N6_DCMIPP_P0DCLMTR) = 0UL;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_decimation_configure(tiku_dcmipp_pipe_t pipe,
                                               const tiku_dcmipp_decimation_config_t *config) {
    if ((pipe != PIPE1 && pipe != PIPE2) || config == NULL ||
        config->horizontal > 3U || config->vertical > 3U) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    uint32_t value = ((uint32_t)config->horizontal << 1) |
                     ((uint32_t)config->vertical << 3);
    if (config->enable != 0U) {
        value |= TIKU_DCMIPP_STAGE_ENABLE;
    }
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DCCR :
               STM32N6_DCMIPP_P2DCCR) = value;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_decimation_disable(tiku_dcmipp_pipe_t pipe) {
    if (pipe != PIPE1 && pipe != PIPE2) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DCCR :
               STM32N6_DCMIPP_P2DCCR) = 0UL;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_pixel_packer_configure(tiku_dcmipp_pipe_t pipe,
                                                 const tiku_dcmipp_pixel_packer_config_t *config) {
    if (!tiku_dcmipp_valid_pipe(pipe) || config == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_ppcr(pipe)) =
        config->control |
        (TIKU_REG32(tiku_dcmipp_ppcr(pipe)) & STM32N6_DCMIPP_PxPPCR_DBM);
    tiku_dcmipp_write_pipe_pitch(pipe, config->pitch);
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_pixel_packer_disable(tiku_dcmipp_pipe_t pipe) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_ppcr(pipe)) =
        TIKU_REG32(tiku_dcmipp_ppcr(pipe)) & STM32N6_DCMIPP_PxPPCR_DBM;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_status_output_configure(tiku_dcmipp_pipe_t pipe,
                                                  uintptr_t address) {
    if (!tiku_dcmipp_valid_pipe(pipe) || address == (uintptr_t)0) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(pipe == PIPE0 ? STM32N6_DCMIPP_P0STM0AR :
               (pipe == PIPE1 ? STM32N6_DCMIPP_P1STM0AR :
                STM32N6_DCMIPP_P2STM0AR)) = (uint32_t)address;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_status_output_disable(tiku_dcmipp_pipe_t pipe) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(pipe == PIPE0 ? STM32N6_DCMIPP_P0STM0AR :
               (pipe == PIPE1 ? STM32N6_DCMIPP_P1STM0AR :
                STM32N6_DCMIPP_P2STM0AR)) = 0UL;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_double_buffer_enable(tiku_dcmipp_pipe_t pipe) {
    if (!tiku_dcmipp_valid_pipe(pipe)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_ppcr(pipe)) |= STM32N6_DCMIPP_PxPPCR_DBM;
    dcmipp.pipe[pipe].double_buffer = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_double_buffer_update_address(tiku_dcmipp_pipe_t pipe,
                                                  const tiku_dcmipp_plane_address_t *addr) {
    if (!tiku_dcmipp_valid_pipe(pipe) ||
        !tiku_dcmipp_valid_plane_address(pipe, addr)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }
    if (dcmipp.pipe[pipe].double_buffer == 0U) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_NOT_ALLOWED);
    }

    tiku_dcmipp_write_pipe_addresses(pipe, addr, 1U);
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_double_buffer_set_address(tiku_dcmipp_pipe_t pipe,
                                               uint8_t index,
                                               const tiku_dcmipp_plane_address_t *addr) {
    if (!tiku_dcmipp_valid_pipe(pipe) || index > 1U ||
        !tiku_dcmipp_valid_plane_address(pipe, addr)) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }
    if (dcmipp.pipe[pipe].double_buffer == 0U) {
        return tiku_dcmipp_fail(TIKU_DCMIPP_ERR_NOT_ALLOWED);
    }

    tiku_dcmipp_write_pipe_addresses(pipe, addr, index);
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_isp_setup(const tiku_dcmipp_isp_config_t *config) {
    if (config == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    dcmipp.isp_config = *config;
    TIKU_REG32(STM32N6_DCMIPP_P1SRCR) = config->stat_removal;
    TIKU_REG32(STM32N6_DCMIPP_P1BPRCR) = config->bad_pixel_removal;
    TIKU_REG32(STM32N6_DCMIPP_P1BLCCR) = config->black_level;
    TIKU_REG32(STM32N6_DCMIPP_P1EXCR1) = config->exposure1;
    TIKU_REG32(STM32N6_DCMIPP_P1EXCR2) = config->exposure2;
    TIKU_REG32(STM32N6_DCMIPP_P1ST1CR) = config->statistics[0];
    TIKU_REG32(STM32N6_DCMIPP_P1ST2CR) = config->statistics[1];
    TIKU_REG32(STM32N6_DCMIPP_P1ST3CR) = config->statistics[2];
    TIKU_REG32(STM32N6_DCMIPP_P1STSTR) = config->statistics_window_start;
    TIKU_REG32(STM32N6_DCMIPP_P1STSZR) = config->statistics_window_size;
    TIKU_REG32(STM32N6_DCMIPP_P1DMCR) = config->demosaic;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_isp_enable(void) {
    if (!dcmipp.peripheral_enabled) {
        return TIKU_DCMIPP_ERR_DISABLED;
    }
    if (!tiku_dcmipp_isp_enable_allowed()) {
        return TIKU_DCMIPP_ERR_NOT_ALLOWED;
    }

    TIKU_REG32(STM32N6_DCMIPP_P1SRCR) = dcmipp.isp_config.stat_removal;
    TIKU_REG32(STM32N6_DCMIPP_P1BPRCR) = dcmipp.isp_config.bad_pixel_removal;
    TIKU_REG32(STM32N6_DCMIPP_P1BLCCR) = dcmipp.isp_config.black_level;
    TIKU_REG32(STM32N6_DCMIPP_P1EXCR1) = dcmipp.isp_config.exposure1;
    TIKU_REG32(STM32N6_DCMIPP_P1EXCR2) = dcmipp.isp_config.exposure2;
    TIKU_REG32(STM32N6_DCMIPP_P1ST1CR) = dcmipp.isp_config.statistics[0];
    TIKU_REG32(STM32N6_DCMIPP_P1ST2CR) = dcmipp.isp_config.statistics[1];
    TIKU_REG32(STM32N6_DCMIPP_P1ST3CR) = dcmipp.isp_config.statistics[2];
    TIKU_REG32(STM32N6_DCMIPP_P1DMCR) = dcmipp.isp_config.demosaic;
    dcmipp.isp_enabled = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_isp_disable(void) {
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(STM32N6_DCMIPP_P1BPRCR) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_P1BLCCR) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_P1EXCR1) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_P1EXCR2) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_P1ST1CR) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_P1ST2CR) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_P1ST3CR) = 0UL;
    TIKU_REG32(STM32N6_DCMIPP_P1DMCR) = 0UL;
    dcmipp.isp_enabled = 0U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_postprocessing_configure(tiku_dcmipp_pipe_t pipe,
                                              const tiku_dcmipp_postproc_config_t *config) {
    if ((pipe != PIPE1 && pipe != PIPE2) || config == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    dcmipp.postproc[pipe] = *config;
    TIKU_REG32(tiku_dcmipp_crop_start_reg(pipe)) = config->crop_start;
    TIKU_REG32(tiku_dcmipp_crop_size_reg(pipe)) = config->crop_size;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DCCR :
               STM32N6_DCMIPP_P2DCCR) = config->decimation;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DSCR :
               STM32N6_DCMIPP_P2DSCR) = config->downsize_control;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DSRTIOR :
               STM32N6_DCMIPP_P2DSRTIOR) = config->downsize_ratio;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DSSZR :
               STM32N6_DCMIPP_P2DSSZR) = config->downsize_destination_size;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1CMRICR :
               STM32N6_DCMIPP_P2CMRICR) = config->roi_common;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1GMCR :
               STM32N6_DCMIPP_P2GMCR) = config->gamma;
    TIKU_REG32(tiku_dcmipp_ppcr(pipe)) =
        config->pixel_packer |
        (TIKU_REG32(tiku_dcmipp_ppcr(pipe)) & STM32N6_DCMIPP_PxPPCR_DBM);
    tiku_dcmipp_write_pipe_pitch(pipe, config->pitch);

    if (pipe == PIPE1) {
        TIKU_REG32(STM32N6_DCMIPP_P1CCCR) = config->color_conversion[0];
        TIKU_REG32(STM32N6_DCMIPP_P1CCRR1) = config->color_conversion[1];
        TIKU_REG32(STM32N6_DCMIPP_P1CCRR2) = config->color_conversion[2];
        TIKU_REG32(STM32N6_DCMIPP_P1CCGR1) = config->color_conversion[3];
        TIKU_REG32(STM32N6_DCMIPP_P1CCGR2) = config->color_conversion[4];
        TIKU_REG32(STM32N6_DCMIPP_P1CCBR1) = config->color_conversion[5];
        TIKU_REG32(STM32N6_DCMIPP_P1CCBR2) = config->color_conversion[6];
        TIKU_REG32(STM32N6_DCMIPP_P1YUVCR) = config->yuv_conversion[0];
    }

    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_postprocessing_enable(tiku_dcmipp_pipe_t pipe) {
    if (pipe != PIPE1 && pipe != PIPE2) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_crop_size_reg(pipe)) |= TIKU_DCMIPP_POSTPROC_ENABLE;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DSCR :
               STM32N6_DCMIPP_P2DSCR) |= TIKU_DCMIPP_POSTPROC_ENABLE;
    dcmipp.postproc_enabled[pipe] = 1U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_postprocessing_disable(tiku_dcmipp_pipe_t pipe) {
    if (pipe != PIPE1 && pipe != PIPE2) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(tiku_dcmipp_crop_size_reg(pipe)) &= ~TIKU_DCMIPP_POSTPROC_ENABLE;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DCCR :
               STM32N6_DCMIPP_P2DCCR) = 0UL;
    TIKU_REG32(pipe == PIPE1 ? STM32N6_DCMIPP_P1DSCR :
               STM32N6_DCMIPP_P2DSCR) &= ~TIKU_DCMIPP_POSTPROC_ENABLE;
    dcmipp.postproc_enabled[pipe] = 0U;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_interrupt_enable(tiku_dcmipp_pipe_t pipe,
                                           uint32_t interrupt_mask) {
    if (!tiku_dcmipp_valid_pipe(pipe) || interrupt_mask == 0U) {
        return TIKU_DCMIPP_ERR_INVALID;
    }

    uint32_t allowed = tiku_dcmipp_pipe_interrupt_mask(pipe) |
                       tiku_dcmipp_common_interrupt_mask();

    if ((interrupt_mask & ~allowed) != 0U) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(STM32N6_DCMIPP_CMFCR) =
        interrupt_mask & STM32N6_DCMIPP_CMFCR_ALL;
    if ((interrupt_mask & STM32N6_DCMIPP_IT_PARALLEL_SYNC_ERR) != 0UL) {
        TIKU_REG32(STM32N6_DCMIPP_PRFCR) = STM32N6_DCMIPP_PRFCR_CERRF;
        TIKU_REG32(STM32N6_DCMIPP_PRIER) |= STM32N6_DCMIPP_PRIER_ERRIE;
    }
    TIKU_REG32(tiku_dcmipp_fcr(pipe)) =
        tiku_dcmipp_local_clear_bits(pipe, interrupt_mask);

    TIKU_REG32(STM32N6_DCMIPP_CMIER) |= interrupt_mask;
    TIKU_REG32(tiku_dcmipp_ier(pipe)) |=
        tiku_dcmipp_local_interrupt_bits(pipe, interrupt_mask);
    tiku_dcmipp_nvic_enable(STM32N6_IRQ_DCMIPP);
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_pipe_interrupt_disable(tiku_dcmipp_pipe_t pipe,
                                            uint32_t interrupt_mask) {
    if (!tiku_dcmipp_valid_pipe(pipe) || interrupt_mask == 0U) {
        return TIKU_DCMIPP_ERR_INVALID;
    }

    uint32_t allowed = tiku_dcmipp_pipe_interrupt_mask(pipe) |
                       tiku_dcmipp_common_interrupt_mask();

    if ((interrupt_mask & ~allowed) != 0U) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }
    TIKU_REG32(STM32N6_DCMIPP_CMIER) &= ~interrupt_mask;
    if ((interrupt_mask & STM32N6_DCMIPP_IT_PARALLEL_SYNC_ERR) != 0UL) {
        TIKU_REG32(STM32N6_DCMIPP_PRIER) &= ~STM32N6_DCMIPP_PRIER_ERRIE;
    }
    TIKU_REG32(tiku_dcmipp_ier(pipe)) &=
        ~tiku_dcmipp_local_interrupt_bits(pipe, interrupt_mask);
    if (TIKU_REG32(STM32N6_DCMIPP_CMIER) == 0UL &&
        TIKU_REG32(STM32N6_DCMIPP_P0IER) == 0UL &&
        TIKU_REG32(STM32N6_DCMIPP_P1IER) == 0UL &&
        TIKU_REG32(STM32N6_DCMIPP_P2IER) == 0UL) {
        tiku_dcmipp_nvic_disable(STM32N6_IRQ_DCMIPP);
    }
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_csi_interrupt_enable(uint32_t ier0_mask,
                                          uint32_t ier1_mask) {
    if ((ier0_mask == 0U && ier1_mask == 0U) ||
        (ier0_mask & ~STM32N6_CSI_IER0_ALL) != 0UL ||
        (ier1_mask & ~STM32N6_CSI_IER1_ALL) != 0UL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(STM32N6_CSI_FCR0) = ier0_mask;
    TIKU_REG32(STM32N6_CSI_FCR1) = ier1_mask;
    TIKU_REG32(STM32N6_CSI_IER0) |= ier0_mask;
    TIKU_REG32(STM32N6_CSI_IER1) |= ier1_mask;
    tiku_dcmipp_nvic_enable(STM32N6_IRQ_CSI);
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_csi_interrupt_disable(uint32_t ier0_mask,
                                           uint32_t ier1_mask) {
    if ((ier0_mask == 0U && ier1_mask == 0U) ||
        (ier0_mask & ~STM32N6_CSI_IER0_ALL) != 0UL ||
        (ier1_mask & ~STM32N6_CSI_IER1_ALL) != 0UL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    TIKU_REG32(STM32N6_CSI_IER0) &= ~ier0_mask;
    TIKU_REG32(STM32N6_CSI_IER1) &= ~ier1_mask;
    if (TIKU_REG32(STM32N6_CSI_IER0) == 0UL &&
        TIKU_REG32(STM32N6_CSI_IER1) == 0UL) {
        tiku_dcmipp_nvic_disable(STM32N6_IRQ_CSI);
    }
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_get_dump_count(tiku_dcmipp_pipe_t pipe, uint32_t *out) {
    if (pipe != PIPE0 || out == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    *out = TIKU_REG32(STM32N6_DCMIPP_P0DCCNTR);
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

int tiku_dcmipp_arch_get_frame_count(tiku_dcmipp_pipe_t pipe, uint32_t *out) {
    if ((pipe != PIPE1 && pipe != PIPE2) || out == NULL) {
        return TIKU_DCMIPP_ERR_INVALID;
    }
    if (tiku_dcmipp_require_enabled() != TIKU_DCMIPP_OK) {
        return dcmipp.last_error;
    }

    uint32_t cmcr = TIKU_REG32(STM32N6_DCMIPP_CMCR);

    /* CMFRCR is shared; CMCR.PSFC selects which pipe feeds the frame counter. */
    cmcr &= ~STM32N6_DCMIPP_CMCR_PSFC_MSK;
    cmcr |= ((uint32_t)pipe << STM32N6_DCMIPP_CMCR_PSFC_POS) &
            STM32N6_DCMIPP_CMCR_PSFC_MSK;
    TIKU_REG32(STM32N6_DCMIPP_CMCR) = cmcr;

    *out = TIKU_REG32(STM32N6_DCMIPP_CMFRCR) &
           STM32N6_DCMIPP_CMFRCR_FRMCNT_MSK;
    dcmipp.last_error = TIKU_DCMIPP_OK;
    return TIKU_DCMIPP_OK;
}

void tiku_stm32n6_dcmipp_isr(void) {
    uint32_t common = TIKU_REG32(STM32N6_DCMIPP_CMSR2) &
                      TIKU_REG32(STM32N6_DCMIPP_CMIER) &
                      STM32N6_DCMIPP_CMSR2_ALL;
    uint32_t pr = TIKU_REG32(STM32N6_DCMIPP_PRSR) &
                  TIKU_REG32(STM32N6_DCMIPP_PRIER) &
                  STM32N6_DCMIPP_PRSR_ERRF;
    uint32_t p0 = TIKU_REG32(tiku_dcmipp_sr(PIPE0)) &
                  TIKU_REG32(tiku_dcmipp_ier(PIPE0));
    uint32_t p1 = TIKU_REG32(tiku_dcmipp_sr(PIPE1)) &
                  TIKU_REG32(tiku_dcmipp_ier(PIPE1));
    uint32_t p2 = TIKU_REG32(tiku_dcmipp_sr(PIPE2)) &
                  TIKU_REG32(tiku_dcmipp_ier(PIPE2));

    TIKU_REG32(STM32N6_DCMIPP_CMFCR) = common;
    if (pr != 0UL) {
        TIKU_REG32(STM32N6_DCMIPP_PRFCR) = STM32N6_DCMIPP_PRFCR_CERRF;
        common |= STM32N6_DCMIPP_FLAG_PARALLEL_SYNC_ERROR;
    }
    TIKU_REG32(tiku_dcmipp_fcr(PIPE0)) = p0 & (STM32N6_DCMIPP_PxFCR_CLINEF |
                                               STM32N6_DCMIPP_PxFCR_CFRAMEF |
                                               STM32N6_DCMIPP_PxFCR_CVSYNCF |
                                               STM32N6_DCMIPP_PxFCR_CLIMITF |
                                               STM32N6_DCMIPP_PxFCR_COVRF);
    TIKU_REG32(tiku_dcmipp_fcr(PIPE1)) = p1 & (STM32N6_DCMIPP_PxFCR_CLINEF |
                                               STM32N6_DCMIPP_PxFCR_CFRAMEF |
                                               STM32N6_DCMIPP_PxFCR_CVSYNCF |
                                               STM32N6_DCMIPP_PxFCR_COVRF);
    TIKU_REG32(tiku_dcmipp_fcr(PIPE2)) = p2 & (STM32N6_DCMIPP_PxFCR_CLINEF |
                                               STM32N6_DCMIPP_PxFCR_CFRAMEF |
                                               STM32N6_DCMIPP_PxFCR_CVSYNCF |
                                               STM32N6_DCMIPP_PxFCR_COVRF);

    common |= tiku_dcmipp_common_from_local(PIPE0, p0);
    common |= tiku_dcmipp_common_from_local(PIPE1, p1);
    common |= tiku_dcmipp_common_from_local(PIPE2, p2);

    if ((common & STM32N6_DCMIPP_FLAG_AXI_TRANSFER_ERROR) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_AXI_TRANSFER,
                               STM32N6_DCMIPP_FLAG_AXI_TRANSFER_ERROR);
    }
    if ((common & STM32N6_DCMIPP_FLAG_PARALLEL_SYNC_ERROR) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_PARALLEL_SYNC,
                               STM32N6_DCMIPP_FLAG_PARALLEL_SYNC_ERROR);
    }

    tiku_dcmipp_post_pipe_events(PIPE0, common,
                                 STM32N6_DCMIPP_FLAG_PIPE0_LINE,
                                 STM32N6_DCMIPP_FLAG_PIPE0_FRAME,
                                 STM32N6_DCMIPP_FLAG_PIPE0_VSYNC,
                                 STM32N6_DCMIPP_FLAG_PIPE0_LIMIT,
                                 STM32N6_DCMIPP_FLAG_PIPE0_OVR);
    tiku_dcmipp_post_pipe_events(PIPE1, common,
                                 STM32N6_DCMIPP_FLAG_PIPE1_LINE,
                                 STM32N6_DCMIPP_FLAG_PIPE1_FRAME,
                                 STM32N6_DCMIPP_FLAG_PIPE1_VSYNC,
                                 0UL,
                                 STM32N6_DCMIPP_FLAG_PIPE1_OVR);
    tiku_dcmipp_post_pipe_events(PIPE2, common,
                                 STM32N6_DCMIPP_FLAG_PIPE2_LINE,
                                 STM32N6_DCMIPP_FLAG_PIPE2_FRAME,
                                 STM32N6_DCMIPP_FLAG_PIPE2_VSYNC,
                                 0UL,
                                 STM32N6_DCMIPP_FLAG_PIPE2_OVR);
}

void tiku_stm32n6_csi_isr(void) {
    uint32_t sr0 = TIKU_REG32(STM32N6_CSI_SR0) &
                   TIKU_REG32(STM32N6_CSI_IER0) &
                   STM32N6_CSI_SR0_ALL;
    uint32_t sr1 = TIKU_REG32(STM32N6_CSI_SR1) &
                   TIKU_REG32(STM32N6_CSI_IER1) &
                   STM32N6_CSI_SR1_ALL;

    TIKU_REG32(STM32N6_CSI_FCR0) = sr0;
    TIKU_REG32(STM32N6_CSI_FCR1) = sr1;

    tiku_dcmipp_post_csi_vc_events(sr0, STM32N6_CSI_SR0_LB0F,
                                   DCMIPP_EVENT_CSI_LINE_BYTE);
    tiku_dcmipp_post_csi_vc_events(sr0, STM32N6_CSI_SR0_TIM0F,
                                   DCMIPP_EVENT_CSI_TIMER);
    tiku_dcmipp_post_csi_vc_events(sr0, STM32N6_CSI_SR0_SOF0F,
                                   DCMIPP_EVENT_CSI_SOF);
    tiku_dcmipp_post_csi_vc_events(sr0, STM32N6_CSI_SR0_EOF0F,
                                   DCMIPP_EVENT_CSI_EOF);

    if ((sr0 & STM32N6_CSI_SR0_SPKTF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_CSI_SHORT_PACKET,
                               DCMIPP_PIPE_NONE, 0U, DCMIPP_ERROR_NONE,
                               STM32N6_CSI_SR0_SPKTF);
    }
    if ((sr0 & STM32N6_CSI_SR0_CCFIFOFF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_CLOCK_CHANGER_FIFO,
                               STM32N6_CSI_SR0_CCFIFOFF);
    }
    if ((sr0 & STM32N6_CSI_SR0_CRCERRF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_CRC,
                               STM32N6_CSI_SR0_CRCERRF);
    }
    if ((sr0 & STM32N6_CSI_SR0_ECCERRF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_ECC,
                               STM32N6_CSI_SR0_ECCERRF);
    }
    if ((sr0 & STM32N6_CSI_SR0_CECCERRF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_CORRECTED_ECC,
                               STM32N6_CSI_SR0_CECCERRF);
    }
    if ((sr0 & STM32N6_CSI_SR0_IDERRF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_ID,
                               STM32N6_CSI_SR0_IDERRF);
    }
    if ((sr0 & STM32N6_CSI_SR0_SPKTERRF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_SHORT_PACKET,
                               STM32N6_CSI_SR0_SPKTERRF);
    }
    if ((sr0 & STM32N6_CSI_SR0_WDERRF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_WATCHDOG,
                               STM32N6_CSI_SR0_WDERRF);
    }
    if ((sr0 & STM32N6_CSI_SR0_SYNCERRF) != 0UL) {
        tiku_dcmipp_post_event(DCMIPP_EVENT_ERROR, DCMIPP_PIPE_NONE, 0U,
                               DCMIPP_ERROR_CSI_SYNC,
                               STM32N6_CSI_SR0_SYNCERRF);
    }

    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ESOTDL0F, 0U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ESOTSYNCDL0F, 0U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_EESCDL0F, 0U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ESYNCESCDL0F, 0U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ECTRLDL0F, 0U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ESOTDL1F, 1U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ESOTSYNCDL1F, 1U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_EESCDL1F, 1U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ESYNCESCDL1F, 1U);
    tiku_dcmipp_post_csi_lane_error(sr1, STM32N6_CSI_SR1_ECTRLDL1F, 1U);
}
