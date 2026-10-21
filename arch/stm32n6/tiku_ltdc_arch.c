/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_ltdc_arch.c - STM32N657 LTDC initialization and configuration.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tiku_ltdc_arch.h"
#include "tiku_stm32n6_regs.h"

#include <string.h>

/* The indirection keeps register programming testable without changing the
 * production MMIO path. Tests can provide both macros before including this
 * file; SRCR writes are then observable as ordinary memory writes. */
#ifndef TIKU_LTDC_REG_READ
#define TIKU_LTDC_REG_READ(address) TIKU_REG32(address)
#endif
#ifndef TIKU_LTDC_REG_WRITE
#define TIKU_LTDC_REG_WRITE(address, value) (TIKU_REG32(address) = (value))
#endif

#define LTDC_ALL_FLAGS (STM32N6_LTDC_IER_ALL)
#define LTDC_ERROR_FLAGS (STM32N6_LTDC_ISR_FUIF | STM32N6_LTDC_ISR_TERRIF)
#define LTDC_CLOCK_POLL_SPINS 2000000UL
#define LTDC_PLL4_DIVM 4UL
#define LTDC_PLL4_VCO_TARGET_HZ 1200000000ULL

/* The N657 exposes one non-secure LTDC instance, so its vectors need one
 * caller-owned handle to reach the registered error callback. */
static tiku_ltdc_handle_t *ltdc_active_handle;
static uint8_t ltdc_clock_configured;

static int ltdc_clock_wait(uintptr_t address, uint32_t mask, int set) {
    uint32_t spin;
    for (spin = 0UL; spin < LTDC_CLOCK_POLL_SPINS; ++spin) {
        if (((TIKU_LTDC_REG_READ(address) & mask) != 0UL) == (set != 0)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t ltdc_abs_diff_u32(uint32_t a, uint32_t b) {
    return a >= b ? a - b : b - a;
}

tiku_ltdc_status_t tiku_ltdc_clock_configure(
    const tiku_ltdc_clock_config_t *config,
    uint32_t *actual_pixel_clock_hz) {
    uint32_t cr;
    uint32_t hsi_div;
    uint32_t hsi_hz;
    uint32_t pll_n;
    uint32_t pll4_cfg1;
    uint32_t pll4_cfg3;
    uint32_t ic_cfg;
    uint32_t divenr;
    uint32_t ccipr4;
    uint32_t divider;
    uint32_t best_divider = 0UL;
    uint32_t best_rate = 0UL;
    uint32_t best_error = UINT32_MAX;

    if (config == 0 || actual_pixel_clock_hz == 0 ||
        config->pixel_clock_hz == 0UL) {
        return TIKU_LTDC_ERR_ARG;
    }
    if (config->pixel_clock_hz > STM32N6_LTDC_MAX_PIXEL_CLOCK_HZ) {
        return TIKU_LTDC_ERR_RANGE;
    }
    if (ltdc_active_handle != 0 ||
        (TIKU_LTDC_REG_READ(STM32N6_RCC_APB5ENR) &
         STM32N6_RCC_APB5ENR_LTDC) != 0UL) {
        return TIKU_LTDC_ERR_STATE;
    }

    ltdc_clock_configured = 0U;
    cr = TIKU_LTDC_REG_READ(STM32N6_RCC_CR);
    if ((cr & STM32N6_RCC_CR_HSION) == 0UL) {
        TIKU_LTDC_REG_WRITE(STM32N6_RCC_CR, cr | STM32N6_RCC_CR_HSION);
    }
    if (!ltdc_clock_wait(STM32N6_RCC_SR, STM32N6_RCC_SR_HSIRDY, 1)) {
        return TIKU_LTDC_ERR_CLOCK;
    }

    hsi_div = (TIKU_LTDC_REG_READ(STM32N6_RCC_HSICFGR) &
               STM32N6_RCC_HSICFGR_DIV_MSK) >>
              STM32N6_RCC_HSICFGR_DIV_POS;
    hsi_hz = STM32N6_HSI_HZ >> hsi_div;
    /* Keep the PLL input divider fixed at four and scale N to compensate for
     * HSIDIV. This produces a 1.2 GHz VCO from any legal HSI divider while
     * leaving the shared HSI divider and PLL1 untouched. */
    pll_n = (uint32_t)((LTDC_PLL4_VCO_TARGET_HZ * LTDC_PLL4_DIVM +
                        (uint64_t)hsi_hz / 2ULL) /
                       (uint64_t)hsi_hz);
    if (pll_n == 0UL || pll_n > 0xFFFUL) {
        return TIKU_LTDC_ERR_CLOCK;
    }

    cr = TIKU_LTDC_REG_READ(STM32N6_RCC_CR);
    if ((cr & STM32N6_RCC_CR_PLL4ON) != 0UL) {
        TIKU_LTDC_REG_WRITE(STM32N6_RCC_CR,
                            cr & ~STM32N6_RCC_CR_PLL4ON);
        if (!ltdc_clock_wait(STM32N6_RCC_SR, STM32N6_RCC_SR_PLL4RDY, 0)) {
            return TIKU_LTDC_ERR_CLOCK;
        }
    }

    pll4_cfg1 = TIKU_LTDC_REG_READ(STM32N6_RCC_PLL4CFGR1);
    pll4_cfg1 &= ~(STM32N6_PLL4_DIVN_MSK | STM32N6_PLL4_DIVM_MSK |
                   STM32N6_PLL4_BYP | STM32N6_PLL4_SEL_MSK);
    pll4_cfg1 |= (pll_n << STM32N6_PLL4_DIVN_POS) |
                 (LTDC_PLL4_DIVM << STM32N6_PLL4_DIVM_POS) |
                 (STM32N6_PLL4_SEL_HSI << STM32N6_PLL4_SEL_POS);
    TIKU_LTDC_REG_WRITE(STM32N6_RCC_PLL4CFGR1, pll4_cfg1);
    TIKU_LTDC_REG_WRITE(STM32N6_RCC_PLL4CFGR2, 0UL);

    pll4_cfg3 = TIKU_LTDC_REG_READ(STM32N6_RCC_PLL4CFGR3);
    pll4_cfg3 &= ~(STM32N6_PLL4_PDIV1_MSK | STM32N6_PLL4_PDIV2_MSK);
    pll4_cfg3 |= (1UL << STM32N6_PLL4_PDIV1_POS) |
                 (1UL << STM32N6_PLL4_PDIV2_POS) | STM32N6_PLL4_PDIVEN;
    TIKU_LTDC_REG_WRITE(STM32N6_RCC_PLL4CFGR3, pll4_cfg3);

    TIKU_LTDC_REG_WRITE(STM32N6_RCC_CR,
                        TIKU_LTDC_REG_READ(STM32N6_RCC_CR) |
                            STM32N6_RCC_CR_PLL4ON);
    if (!ltdc_clock_wait(STM32N6_RCC_SR, STM32N6_RCC_SR_PLL4RDY, 1)) {
        TIKU_LTDC_REG_WRITE(STM32N6_RCC_CR,
                            TIKU_LTDC_REG_READ(STM32N6_RCC_CR) &
                                ~STM32N6_RCC_CR_PLL4ON);
        return TIKU_LTDC_ERR_CLOCK;
    }

    /* IC dividers encode divide-by-(ICINT + 1), from 1 through 256. */
    for (divider = 1UL; divider <= 256UL; ++divider) {
        uint32_t rate = (uint32_t)(LTDC_PLL4_VCO_TARGET_HZ / divider);
        uint32_t error = ltdc_abs_diff_u32(rate, config->pixel_clock_hz);
        if (error < best_error) {
            best_error = error;
            best_divider = divider;
            best_rate = rate;
        }
    }

    ic_cfg = TIKU_LTDC_REG_READ(STM32N6_RCC_ICCFGR(STM32N6_RCC_IC16));
    ic_cfg &= ~(STM32N6_IC_INT_MSK | STM32N6_IC_SEL_MSK);
    ic_cfg |= ((best_divider - 1UL) << STM32N6_IC_INT_POS) |
              (STM32N6_IC_SEL_PLL4 << STM32N6_IC_SEL_POS);
    TIKU_LTDC_REG_WRITE(STM32N6_RCC_ICCFGR(STM32N6_RCC_IC16), ic_cfg);

    divenr = TIKU_LTDC_REG_READ(STM32N6_RCC_DIVENR);
    TIKU_LTDC_REG_WRITE(STM32N6_RCC_DIVENR,
                        divenr | STM32N6_RCC_DIVENR_IC16);
    ccipr4 = TIKU_LTDC_REG_READ(STM32N6_RCC_CCIPR4);
    ccipr4 &= ~STM32N6_RCC_CCIPR4_LTDCSEL_MSK;
    ccipr4 |= STM32N6_RCC_CCIPR4_LTDCSEL_IC16;
    TIKU_LTDC_REG_WRITE(STM32N6_RCC_CCIPR4, ccipr4);

    *actual_pixel_clock_hz = best_rate;
    ltdc_clock_configured = 1U;
    return TIKU_LTDC_OK;
}

typedef struct {
    uint32_t pfcr;
    uint32_t fpf0r;
    uint32_t fpf1r;
    uint32_t bytes_per_pixel;
} ltdc_format_t;

static int ltdc_state_valid(tiku_ltdc_state_t state) {
    return state == TIKU_LTDC_STATE_INIT ||
           state == TIKU_LTDC_STATE_CONFIGURED ||
           state == TIKU_LTDC_STATE_ACTIVE;
}

static uint32_t ltdc_pack_timing(uint64_t horizontal, uint64_t vertical) {
    return (uint32_t)(((horizontal - 1ULL) << STM32N6_LTDC_TIMING_H_POS) |
                      ((vertical - 1ULL) << STM32N6_LTDC_TIMING_V_POS));
}

static int ltdc_timing_count_valid(uint64_t count) {
    return count != 0ULL && count <= 65536ULL;
}

static int ltdc_global_config_valid(const tiku_ltdc_global_config_t *config) {
    uint64_t h_sync;
    uint64_t h_back;
    uint64_t h_active;
    uint64_t h_total;
    uint64_t v_sync;
    uint64_t v_back;
    uint64_t v_active;
    uint64_t v_total;

    if (config == 0 || config->active_width == 0UL ||
        config->active_height == 0UL || config->hsync_width == 0UL ||
        config->vsync_height == 0UL || config->background_rgb > 0xFFFFFFUL ||
        (config->hsync_polarity != TIKU_LTDC_POLARITY_ACTIVE_LOW &&
         config->hsync_polarity != TIKU_LTDC_POLARITY_ACTIVE_HIGH) ||
        (config->vsync_polarity != TIKU_LTDC_POLARITY_ACTIVE_LOW &&
         config->vsync_polarity != TIKU_LTDC_POLARITY_ACTIVE_HIGH) ||
        (config->data_enable_polarity != TIKU_LTDC_POLARITY_ACTIVE_LOW &&
         config->data_enable_polarity != TIKU_LTDC_POLARITY_ACTIVE_HIGH) ||
        (config->pixel_clock_polarity != TIKU_LTDC_PIXEL_CLOCK_NORMAL &&
         config->pixel_clock_polarity != TIKU_LTDC_PIXEL_CLOCK_INVERTED) ||
        config->dithering_enabled > 1U) {
        return 0;
    }

    h_sync = config->hsync_width;
    h_back = h_sync + (uint64_t)config->h_back_porch;
    h_active = h_back + (uint64_t)config->active_width;
    h_total = h_active + (uint64_t)config->h_front_porch;
    v_sync = config->vsync_height;
    v_back = v_sync + (uint64_t)config->v_back_porch;
    v_active = v_back + (uint64_t)config->active_height;
    v_total = v_active + (uint64_t)config->v_front_porch;

    return ltdc_timing_count_valid(h_sync) &&
           ltdc_timing_count_valid(h_back) &&
           ltdc_timing_count_valid(h_active) &&
           ltdc_timing_count_valid(h_total) &&
           ltdc_timing_count_valid(v_sync) &&
           ltdc_timing_count_valid(v_back) &&
           ltdc_timing_count_valid(v_active) &&
           ltdc_timing_count_valid(v_total);
}

static int ltdc_format_get(tiku_ltdc_pixel_format_t format,
                           ltdc_format_t *result) {
    if (result == 0) {
        return 0;
    }

    result->fpf0r = 0UL;
    result->fpf1r = 0UL;
    switch (format) {
    case TIKU_LTDC_PIXEL_FORMAT_ARGB8888:
        result->pfcr = 0UL;
        result->bytes_per_pixel = 4UL;
        return 1;
    case TIKU_LTDC_PIXEL_FORMAT_RGB888:
        result->pfcr = 6UL;
        result->bytes_per_pixel = 3UL;
        return 1;
    case TIKU_LTDC_PIXEL_FORMAT_RGB565:
        result->pfcr = 4UL;
        result->bytes_per_pixel = 2UL;
        return 1;
    case TIKU_LTDC_PIXEL_FORMAT_ARGB1555:
        result->pfcr = STM32N6_LTDC_PFCR_FLEXIBLE;
        result->bytes_per_pixel = 2UL;
        result->fpf0r = (1UL << STM32N6_LTDC_FPF0R_ALEN_POS) |
                        (15UL << STM32N6_LTDC_FPF0R_APOS_POS) |
                        (5UL << STM32N6_LTDC_FPF0R_RLEN_POS) |
                        (10UL << STM32N6_LTDC_FPF0R_RPOS_POS);
        result->fpf1r = (2UL << STM32N6_LTDC_FPF1R_PSIZE_POS) |
                        (5UL << STM32N6_LTDC_FPF1R_BLEN_POS) |
                        (5UL << STM32N6_LTDC_FPF1R_GLEN_POS) |
                        (5UL << STM32N6_LTDC_FPF1R_GPOS_POS);
        return 1;
    case TIKU_LTDC_PIXEL_FORMAT_ARGB4444:
        result->pfcr = STM32N6_LTDC_PFCR_FLEXIBLE;
        result->bytes_per_pixel = 2UL;
        result->fpf0r = (4UL << STM32N6_LTDC_FPF0R_ALEN_POS) |
                        (12UL << STM32N6_LTDC_FPF0R_APOS_POS) |
                        (4UL << STM32N6_LTDC_FPF0R_RLEN_POS) |
                        (8UL << STM32N6_LTDC_FPF0R_RPOS_POS);
        result->fpf1r = (2UL << STM32N6_LTDC_FPF1R_PSIZE_POS) |
                        (4UL << STM32N6_LTDC_FPF1R_BLEN_POS) |
                        (4UL << STM32N6_LTDC_FPF1R_GLEN_POS) |
                        (4UL << STM32N6_LTDC_FPF1R_GPOS_POS);
        return 1;
    case TIKU_LTDC_PIXEL_FORMAT_L8:
        result->pfcr = STM32N6_LTDC_PFCR_FLEXIBLE;
        result->bytes_per_pixel = 1UL;
        result->fpf0r = (8UL << STM32N6_LTDC_FPF0R_RLEN_POS);
        result->fpf1r = (1UL << STM32N6_LTDC_FPF1R_PSIZE_POS) |
                        (8UL << STM32N6_LTDC_FPF1R_BLEN_POS) |
                        (8UL << STM32N6_LTDC_FPF1R_GLEN_POS);
        return 1;
    case TIKU_LTDC_PIXEL_FORMAT_AL44:
        result->pfcr = STM32N6_LTDC_PFCR_FLEXIBLE;
        result->bytes_per_pixel = 1UL;
        result->fpf0r = (4UL << STM32N6_LTDC_FPF0R_ALEN_POS) |
                        (4UL << STM32N6_LTDC_FPF0R_APOS_POS) |
                        (4UL << STM32N6_LTDC_FPF0R_RLEN_POS);
        result->fpf1r = (1UL << STM32N6_LTDC_FPF1R_PSIZE_POS) |
                        (4UL << STM32N6_LTDC_FPF1R_BLEN_POS) |
                        (4UL << STM32N6_LTDC_FPF1R_GLEN_POS);
        return 1;
    case TIKU_LTDC_PIXEL_FORMAT_AL88:
        result->pfcr = STM32N6_LTDC_PFCR_FLEXIBLE;
        result->bytes_per_pixel = 2UL;
        result->fpf0r = (8UL << STM32N6_LTDC_FPF0R_ALEN_POS) |
                        (8UL << STM32N6_LTDC_FPF0R_APOS_POS) |
                        (8UL << STM32N6_LTDC_FPF0R_RLEN_POS);
        result->fpf1r = (2UL << STM32N6_LTDC_FPF1R_PSIZE_POS) |
                        (8UL << STM32N6_LTDC_FPF1R_BLEN_POS) |
                        (8UL << STM32N6_LTDC_FPF1R_GLEN_POS);
        return 1;
    default:
        return 0;
    }
}

static int ltdc_layer_config_valid(const tiku_ltdc_handle_t *handle,
                                   const tiku_ltdc_layer_config_t *config,
                                   ltdc_format_t *format,
                                   uint32_t *line_length) {
    uint64_t width;
    uint64_t height;
    uint64_t visible_bytes;
    uint64_t h_start;
    uint64_t h_stop;
    uint64_t v_start;
    uint64_t v_stop;
    uint64_t h_back;
    uint64_t v_back;

    if (handle == 0 || config == 0 || format == 0 || line_length == 0 ||
        !handle->global_configured ||
        !ltdc_format_get(config->pixel_format, format) ||
        config->x_start >= config->x_end ||
        config->y_start >= config->y_end ||
        config->x_end > handle->global_config.active_width ||
        config->y_end > handle->global_config.active_height ||
        (config->blending_factor1 != TIKU_LTDC_BLEND_CONSTANT_ALPHA &&
         config->blending_factor1 !=
             TIKU_LTDC_BLEND_PIXEL_ALPHA_X_CONSTANT_ALPHA) ||
        (config->blending_factor2 != TIKU_LTDC_BLEND_CONSTANT_ALPHA &&
         config->blending_factor2 !=
             TIKU_LTDC_BLEND_PIXEL_ALPHA_X_CONSTANT_ALPHA)) {
        return 0;
    }

    width = (uint64_t)config->x_end - (uint64_t)config->x_start;
    height = (uint64_t)config->y_end - (uint64_t)config->y_start;
    visible_bytes = width * (uint64_t)format->bytes_per_pixel;
    if (visible_bytes == 0ULL || visible_bytes + 7ULL > 65535ULL ||
        config->pitch_bytes < visible_bytes || config->pitch_bytes > 65535UL ||
        config->framebuffer_lines < height ||
        config->framebuffer_lines == 0UL || config->framebuffer_lines > 65535UL) {
        return 0;
    }

    h_back = (uint64_t)handle->global_config.hsync_width +
             (uint64_t)handle->global_config.h_back_porch;
    v_back = (uint64_t)handle->global_config.vsync_height +
             (uint64_t)handle->global_config.v_back_porch;
    h_start = h_back + (uint64_t)config->x_start + 1ULL;
    h_stop = h_back + (uint64_t)config->x_end;
    v_start = v_back + (uint64_t)config->y_start + 1ULL;
    v_stop = v_back + (uint64_t)config->y_end;
    if (h_start > 65535ULL || h_stop > 65535ULL ||
        v_start > 65535ULL || v_stop > 65535ULL) {
        return 0;
    }

    *line_length = (uint32_t)(visible_bytes + 7ULL);
    return 1;
}

static uint32_t ltdc_blend_factor1(tiku_ltdc_blend_factor_t factor) {
    return factor == TIKU_LTDC_BLEND_CONSTANT_ALPHA
               ? STM32N6_LTDC_BFCR_BF1_CA
               : STM32N6_LTDC_BFCR_BF1_PAXCA;
}

static uint32_t ltdc_blend_factor2(tiku_ltdc_blend_factor_t factor) {
    return factor == TIKU_LTDC_BLEND_CONSTANT_ALPHA
               ? STM32N6_LTDC_BFCR_BF2_CA
               : STM32N6_LTDC_BFCR_BF2_PAXCA;
}

tiku_ltdc_status_t tiku_ltdc_init(tiku_ltdc_handle_t *handle) {
    if (handle == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    if (ltdc_active_handle != 0) {
        return TIKU_LTDC_ERR_STATE;
    }
    if (ltdc_clock_configured == 0U) {
        return TIKU_LTDC_ERR_CLOCK;
    }

    memset(handle, 0, sizeof(*handle));
    TIKU_LTDC_REG_WRITE(STM32N6_RCC_APB5ENR,
                        TIKU_LTDC_REG_READ(STM32N6_RCC_APB5ENR) |
                            STM32N6_RCC_APB5ENR_LTDC);
    handle->state = TIKU_LTDC_STATE_INIT;
    ltdc_active_handle = handle;
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_deinit(tiku_ltdc_handle_t *handle) {
    uint32_t layer;

    if (handle == 0) {
        return TIKU_LTDC_ERR_ARG;
    }

    /* A RESET handle has already been deinitialized. Avoid touching registers
     * when its clock gate may already be off. */
    if (handle->state != TIKU_LTDC_STATE_RESET) {
        TIKU_LTDC_REG_WRITE(STM32N6_LTDC_GCR,
                            TIKU_LTDC_REG_READ(STM32N6_LTDC_GCR) &
                                ~STM32N6_LTDC_GCR_LTDCEN);
        for (layer = 0UL; layer < TIKU_LTDC_LAYER_COUNT; ++layer) {
            TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LCR(layer),
                                TIKU_LTDC_REG_READ(STM32N6_LTDC_LCR(layer)) &
                                    ~STM32N6_LTDC_LAYER_LEN);
        }
        TIKU_LTDC_REG_WRITE(STM32N6_LTDC_IER, 0UL);
        TIKU_LTDC_REG_WRITE(STM32N6_LTDC_IER2, 0UL);
        TIKU_LTDC_REG_WRITE(STM32N6_LTDC_ICR, LTDC_ALL_FLAGS);
        TIKU_LTDC_REG_WRITE(STM32N6_LTDC_ICR2, LTDC_ALL_FLAGS);
        TIKU_LTDC_REG_WRITE(STM32N6_RCC_APB5ENR,
                            TIKU_LTDC_REG_READ(STM32N6_RCC_APB5ENR) &
                                ~STM32N6_RCC_APB5ENR_LTDC);
    }

    memset(handle, 0, sizeof(*handle));
    handle->state = TIKU_LTDC_STATE_RESET;
    if (ltdc_active_handle == handle) {
        ltdc_active_handle = 0;
    }
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_configure(
    tiku_ltdc_handle_t *handle, const tiku_ltdc_global_config_t *config) {
    uint64_t h_sync;
    uint64_t h_back;
    uint64_t h_active;
    uint64_t h_total;
    uint64_t v_sync;
    uint64_t v_back;
    uint64_t v_active;
    uint64_t v_total;
    uint32_t gcr;
    uint32_t was_active;
    uint32_t layer;

    if (handle == 0 || config == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    if (!ltdc_state_valid(handle->state) ||
        ((handle->state == TIKU_LTDC_STATE_CONFIGURED ||
          handle->state == TIKU_LTDC_STATE_ACTIVE) &&
         !handle->global_configured)) {
        return TIKU_LTDC_ERR_STATE;
    }
    if (!ltdc_global_config_valid(config)) {
        return TIKU_LTDC_ERR_RANGE;
    }

    h_sync = config->hsync_width;
    h_back = h_sync + (uint64_t)config->h_back_porch;
    h_active = h_back + (uint64_t)config->active_width;
    h_total = h_active + (uint64_t)config->h_front_porch;
    v_sync = config->vsync_height;
    v_back = v_sync + (uint64_t)config->v_back_porch;
    v_active = v_back + (uint64_t)config->active_height;
    v_total = v_active + (uint64_t)config->v_front_porch;

    was_active = handle->state == TIKU_LTDC_STATE_ACTIVE;
    if (was_active) {
        (void)tiku_ltdc_stop(handle);
    }
    gcr = TIKU_LTDC_REG_READ(STM32N6_LTDC_GCR);
    if (was_active || (gcr & STM32N6_LTDC_GCR_LTDCEN) != 0UL) {
        TIKU_LTDC_REG_WRITE(STM32N6_LTDC_GCR,
                            gcr & ~STM32N6_LTDC_GCR_LTDCEN);
    }
    gcr &= ~STM32N6_LTDC_GCR_LTDCEN;
    gcr &= ~(STM32N6_LTDC_GCR_HSPOL | STM32N6_LTDC_GCR_VSPOL |
             STM32N6_LTDC_GCR_DEPOL | STM32N6_LTDC_GCR_PCPOL |
             STM32N6_LTDC_GCR_DEN);
    if (config->hsync_polarity == TIKU_LTDC_POLARITY_ACTIVE_HIGH) {
        gcr |= STM32N6_LTDC_GCR_HSPOL;
    }
    if (config->vsync_polarity == TIKU_LTDC_POLARITY_ACTIVE_HIGH) {
        gcr |= STM32N6_LTDC_GCR_VSPOL;
    }
    if (config->data_enable_polarity == TIKU_LTDC_POLARITY_ACTIVE_HIGH) {
        gcr |= STM32N6_LTDC_GCR_DEPOL;
    }
    if (config->pixel_clock_polarity == TIKU_LTDC_PIXEL_CLOCK_INVERTED) {
        gcr |= STM32N6_LTDC_GCR_PCPOL;
    }
    if (config->dithering_enabled != 0U) {
        gcr |= STM32N6_LTDC_GCR_DEN;
    }

    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_GCR, gcr);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_SSCR,
                        ltdc_pack_timing(h_sync, v_sync));
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_BPCR,
                        ltdc_pack_timing(h_back, v_back));
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_AWCR,
                        ltdc_pack_timing(h_active, v_active));
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_TWCR,
                        ltdc_pack_timing(h_total, v_total));
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_BCCR, config->background_rgb);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_SRCR, STM32N6_LTDC_SRCR_IMR);

    handle->global_config = *config;
    handle->global_configured = 1U;
    for (layer = 0UL; layer < TIKU_LTDC_LAYER_COUNT; ++layer) {
        handle->layer_configured[layer] = 0U;
    }
    handle->state = TIKU_LTDC_STATE_CONFIGURED;
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_configure_layer(
    tiku_ltdc_handle_t *handle, tiku_ltdc_layer_t layer,
    const tiku_ltdc_layer_config_t *config) {
    ltdc_format_t format;
    uint32_t line_length;
    uint32_t index;
    uint32_t h_back;
    uint32_t v_back;
    uint32_t h_start;
    uint32_t h_stop;
    uint32_t v_start;
    uint32_t v_stop;
    uint32_t blend;
    uint32_t reload;

    if (handle == 0 || config == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    if ((uint32_t)layer >= (uint32_t)TIKU_LTDC_LAYER_COUNT) {
        return TIKU_LTDC_ERR_ARG;
    }
    if (!ltdc_state_valid(handle->state) ||
        (handle->state != TIKU_LTDC_STATE_CONFIGURED &&
         handle->state != TIKU_LTDC_STATE_ACTIVE) ||
        !handle->global_configured) {
        return TIKU_LTDC_ERR_STATE;
    }
    if (!ltdc_layer_config_valid(handle, config, &format, &line_length)) {
        return TIKU_LTDC_ERR_RANGE;
    }

    index = (uint32_t)layer;
    h_back = handle->global_config.hsync_width +
             handle->global_config.h_back_porch;
    v_back = handle->global_config.vsync_height +
             handle->global_config.v_back_porch;
    h_start = h_back + config->x_start + 1UL;
    h_stop = h_back + config->x_end;
    v_start = v_back + config->y_start + 1UL;
    v_stop = v_back + config->y_end;

    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LWHPCR(index),
                        (h_stop << STM32N6_LTDC_WINDOW_STOP_POS) | h_start);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LWVPCR(index),
                        (v_stop << STM32N6_LTDC_WINDOW_STOP_POS) | v_start);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LPFCR(index), format.pfcr);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LFPF0R(index), format.fpf0r);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LFPF1R(index), format.fpf1r);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LCFBLR(index),
                        (config->pitch_bytes << STM32N6_LTDC_CFBLR_CFBP_POS) |
                            line_length);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LCFBLNR(index),
                        config->framebuffer_lines);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LCACR(index), config->constant_alpha);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LDCCR(index), config->default_argb);
    blend = ltdc_blend_factor1(config->blending_factor1) |
            ltdc_blend_factor2(config->blending_factor2);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LBFCR(index), blend);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LCR(index),
                        TIKU_LTDC_REG_READ(STM32N6_LTDC_LCR(index)) |
                            STM32N6_LTDC_LAYER_LEN);
    reload = handle->state == TIKU_LTDC_STATE_ACTIVE
                 ? STM32N6_LTDC_SRCR_VBR
                 : STM32N6_LTDC_SRCR_IMR;
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_SRCR, reload);

    handle->layer_config[index] = *config;
    handle->layer_configured[index] = 1U;
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_set_layer_framebuffer(
    tiku_ltdc_handle_t *handle, tiku_ltdc_layer_t layer, uintptr_t address) {
    uint32_t index;
    uint32_t reload;

    if (handle == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    if ((uint32_t)layer >= (uint32_t)TIKU_LTDC_LAYER_COUNT) {
        return TIKU_LTDC_ERR_ARG;
    }
    if ((handle->state != TIKU_LTDC_STATE_CONFIGURED &&
         handle->state != TIKU_LTDC_STATE_ACTIVE) ||
        !handle->global_configured) {
        return TIKU_LTDC_ERR_STATE;
    }
    index = (uint32_t)layer;
    if (handle->layer_configured[index] == 0U) {
        return TIKU_LTDC_ERR_STATE;
    }
    if (address > (uintptr_t)UINT32_MAX) {
        return TIKU_LTDC_ERR_RANGE;
    }

    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_LCFBAR(index), (uint32_t)address);
    reload = handle->state == TIKU_LTDC_STATE_ACTIVE
                 ? STM32N6_LTDC_SRCR_VBR
                 : STM32N6_LTDC_SRCR_IMR;
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_SRCR, reload);
    handle->framebuffer[index] = address;
    handle->framebuffer_set[index] = 1U;
    return TIKU_LTDC_OK;
}

static void ltdc_clear_pending_flags(uintptr_t status_reg,
                                     uintptr_t clear_reg) {
    uint32_t pending = TIKU_LTDC_REG_READ(status_reg) & LTDC_ALL_FLAGS;
    if (pending != 0UL) {
        TIKU_LTDC_REG_WRITE(clear_reg, pending);
    }
}

tiku_ltdc_status_t tiku_ltdc_start(tiku_ltdc_handle_t *handle) {
    uint32_t layer;
    int framebuffer_ready = 0;

    if (handle == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    if (handle->state != TIKU_LTDC_STATE_CONFIGURED ||
        !handle->global_configured) {
        return TIKU_LTDC_ERR_STATE;
    }
    for (layer = 0UL; layer < TIKU_LTDC_LAYER_COUNT; ++layer) {
        if (handle->layer_configured[layer] != 0U &&
            handle->framebuffer_set[layer] != 0U) {
            framebuffer_ready = 1;
        }
    }
    if (!framebuffer_ready) {
        return TIKU_LTDC_ERR_STATE;
    }

    /* Clear stale status before enabling the two supported error sources. */
    ltdc_clear_pending_flags(STM32N6_LTDC_ISR, STM32N6_LTDC_ICR);
    ltdc_clear_pending_flags(STM32N6_LTDC_ISR2, STM32N6_LTDC_ICR2);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_IER, LTDC_ERROR_FLAGS);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_IER2, LTDC_ERROR_FLAGS);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_GCR,
                        TIKU_LTDC_REG_READ(STM32N6_LTDC_GCR) |
                            STM32N6_LTDC_GCR_LTDCEN);
    handle->state = TIKU_LTDC_STATE_ACTIVE;
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_stop(tiku_ltdc_handle_t *handle) {
    if (handle == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    if (handle->state != TIKU_LTDC_STATE_ACTIVE) {
        return TIKU_LTDC_ERR_STATE;
    }

    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_GCR,
                        TIKU_LTDC_REG_READ(STM32N6_LTDC_GCR) &
                            ~STM32N6_LTDC_GCR_LTDCEN);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_IER, 0UL);
    TIKU_LTDC_REG_WRITE(STM32N6_LTDC_IER2, 0UL);
    ltdc_clear_pending_flags(STM32N6_LTDC_ISR, STM32N6_LTDC_ICR);
    ltdc_clear_pending_flags(STM32N6_LTDC_ISR2, STM32N6_LTDC_ICR2);
    handle->state = TIKU_LTDC_STATE_CONFIGURED;
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_get_state(const tiku_ltdc_handle_t *handle,
                                       tiku_ltdc_state_t *state) {
    if (handle == 0 || state == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    *state = handle->state;
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_get_error_flags(
    const tiku_ltdc_handle_t *handle, tiku_ltdc_error_flags_t *errors) {
    if (handle == 0 || errors == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    *errors = handle->error_flags;
    return TIKU_LTDC_OK;
}

tiku_ltdc_status_t tiku_ltdc_register_error_callback(
    tiku_ltdc_handle_t *handle, tiku_ltdc_error_callback_t callback) {
    if (handle == 0) {
        return TIKU_LTDC_ERR_ARG;
    }
    if (!ltdc_state_valid(handle->state)) {
        return TIKU_LTDC_ERR_STATE;
    }
    handle->error_callback = callback;
    return TIKU_LTDC_OK;
}

static void ltdc_general_irq_bank(uintptr_t status_reg,
                                  uintptr_t clear_reg) {
    uint32_t pending = TIKU_LTDC_REG_READ(status_reg) &
                       STM32N6_LTDC_ISR_RRIF;
    if (pending != 0UL) {
        TIKU_LTDC_REG_WRITE(clear_reg, pending);
    }
}

static void ltdc_error_irq_bank(uintptr_t status_reg,
                                uintptr_t clear_reg) {
    uint32_t pending = TIKU_LTDC_REG_READ(status_reg) & LTDC_ERROR_FLAGS;
    tiku_ltdc_error_flags_t errors = 0UL;
    tiku_ltdc_handle_t *handle = ltdc_active_handle;

    if (pending == 0UL) {
        return;
    }
    TIKU_LTDC_REG_WRITE(clear_reg, pending);
    if ((pending & STM32N6_LTDC_ISR_FUIF) != 0UL) {
        errors |= TIKU_LTDC_ERROR_FIFO_UNDERRUN;
    }
    if ((pending & STM32N6_LTDC_ISR_TERRIF) != 0UL) {
        errors |= TIKU_LTDC_ERROR_TRANSFER;
    }
    if (handle != 0 && errors != 0UL) {
        handle->error_flags |= errors;
        if (handle->error_callback != 0) {
            handle->error_callback(handle, errors);
        }
    }
}

void tiku_ltdc_irq_handler(void) {
    ltdc_general_irq_bank(STM32N6_LTDC_ISR, STM32N6_LTDC_ICR);
    ltdc_general_irq_bank(STM32N6_LTDC_ISR2, STM32N6_LTDC_ICR2);
}

void tiku_ltdc_error_irq_handler(void) {
    ltdc_error_irq_bank(STM32N6_LTDC_ISR, STM32N6_LTDC_ICR);
    ltdc_error_irq_bank(STM32N6_LTDC_ISR2, STM32N6_LTDC_ICR2);
}

/* Startup-table bridges select the bank associated with the entering vector. */
void tiku_stm32n6_ltdc_lo_isr(void) {
    ltdc_general_irq_bank(STM32N6_LTDC_ISR, STM32N6_LTDC_ICR);
}

void tiku_stm32n6_ltdc_up_isr(void) {
    ltdc_general_irq_bank(STM32N6_LTDC_ISR2, STM32N6_LTDC_ICR2);
}

void tiku_stm32n6_ltdc_lo_err_isr(void) {
    ltdc_error_irq_bank(STM32N6_LTDC_ISR, STM32N6_LTDC_ICR);
}

void tiku_stm32n6_ltdc_up_err_isr(void) {
    ltdc_error_irq_bank(STM32N6_LTDC_ISR2, STM32N6_LTDC_ICR2);
}
