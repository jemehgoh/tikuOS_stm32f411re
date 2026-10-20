/* Host-side register tests for the STM32N6 LTDC phase 3 implementation. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../arch/stm32n6/tiku_ltdc_arch.h"
#include "../arch/stm32n6/tiku_stm32n6_regs.h"

typedef struct {
    uintptr_t address;
    uint32_t value;
} test_register_t;

typedef struct {
    uintptr_t address;
    uint32_t value;
} test_write_t;

static test_register_t registers[64];
static size_t register_count;
static test_write_t writes[256];
static size_t write_count;
static uint32_t callback_count;
static tiku_ltdc_error_flags_t callback_errors;
static tiku_ltdc_handle_t *callback_handle;

static uint32_t test_mmio_read(uintptr_t address) {
    size_t i;
    for (i = 0U; i < register_count; ++i) {
        if (registers[i].address == address) {
            return registers[i].value;
        }
    }
    return 0UL;
}

static void test_mmio_write(uintptr_t address, uint32_t value) {
    size_t i;
    assert(write_count < (sizeof(writes) / sizeof(writes[0])));
    writes[write_count].address = address;
    writes[write_count].value = value;
    ++write_count;
    for (i = 0U; i < register_count; ++i) {
        if (registers[i].address == address) {
            registers[i].value = value;
            break;
        }
    }
    if (i == register_count) {
        assert(register_count < (sizeof(registers) / sizeof(registers[0])));
        registers[register_count].address = address;
        registers[register_count].value = value;
        ++register_count;
    }

    /* ICR and ICR2 are write-one-to-clear views of ISR and ISR2. */
    if (address == STM32N6_LTDC_ICR || address == STM32N6_LTDC_ICR2) {
        uintptr_t status_address = address == STM32N6_LTDC_ICR
                                       ? STM32N6_LTDC_ISR
                                       : STM32N6_LTDC_ISR2;
        size_t status_index;
        for (status_index = 0U; status_index < register_count; ++status_index) {
            if (registers[status_index].address == status_address) {
                registers[status_index].value &= ~value;
                break;
            }
        }
    }
}

static void test_mmio_reset(void) {
    register_count = 0U;
    write_count = 0U;
}

#define TIKU_LTDC_REG_READ(address) test_mmio_read((uintptr_t)(address))
#define TIKU_LTDC_REG_WRITE(address, value) \
    test_mmio_write((uintptr_t)(address), (uint32_t)(value))
#include "../arch/stm32n6/tiku_ltdc_arch.c"

static void test_error_callback(tiku_ltdc_handle_t *handle,
                                tiku_ltdc_error_flags_t errors) {
    ++callback_count;
    callback_errors = errors;
    callback_handle = handle;
}

static void callback_reset(void) {
    callback_count = 0UL;
    callback_errors = 0UL;
    callback_handle = 0;
}

static tiku_ltdc_global_config_t valid_global_config(void) {
    tiku_ltdc_global_config_t config = {0};
    config.active_width = 800UL;
    config.active_height = 480UL;
    config.hsync_width = 40UL;
    config.h_back_porch = 88UL;
    config.h_front_porch = 40UL;
    config.vsync_height = 3UL;
    config.v_back_porch = 32UL;
    config.v_front_porch = 10UL;
    config.hsync_polarity = TIKU_LTDC_POLARITY_ACTIVE_HIGH;
    config.vsync_polarity = TIKU_LTDC_POLARITY_ACTIVE_LOW;
    config.data_enable_polarity = TIKU_LTDC_POLARITY_ACTIVE_HIGH;
    config.pixel_clock_polarity = TIKU_LTDC_PIXEL_CLOCK_INVERTED;
    config.background_rgb = 0x123456UL;
    config.dithering_enabled = 1U;
    return config;
}

static tiku_ltdc_layer_config_t valid_layer_config(
    tiku_ltdc_pixel_format_t format, uint32_t bytes_per_pixel) {
    tiku_ltdc_layer_config_t config = {0};
    config.x_start = 10UL;
    config.x_end = 18UL;
    config.y_start = 2UL;
    config.y_end = 6UL;
    config.pixel_format = format;
    config.pitch_bytes = 8UL * bytes_per_pixel;
    config.framebuffer_lines = 4UL;
    config.constant_alpha = 127U;
    config.blending_factor1 = TIKU_LTDC_BLEND_PIXEL_ALPHA_X_CONSTANT_ALPHA;
    config.blending_factor2 = TIKU_LTDC_BLEND_CONSTANT_ALPHA;
    config.default_argb = 0xA1B2C3D4UL;
    return config;
}

static void test_init_and_deinit(void) {
    tiku_ltdc_handle_t handle;
    test_mmio_reset();
    assert(tiku_ltdc_init(0) == TIKU_LTDC_ERR_ARG);
    assert(write_count == 0U);

    handle.error_flags = 0xFFFFFFFFUL;
    handle.error_callback = test_error_callback;
    assert(tiku_ltdc_init(&handle) == TIKU_LTDC_OK);
    assert(handle.state == TIKU_LTDC_STATE_INIT);
    assert(handle.global_configured == 0U);
    assert(handle.layer_configured[0] == 0U);
    assert(handle.layer_configured[1] == 0U);
    assert(handle.error_flags == 0UL);
    assert(handle.error_callback == 0);
    assert((test_mmio_read(STM32N6_RCC_APB5ENR) &
            STM32N6_RCC_APB5ENR_LTDC) != 0UL);

    test_mmio_write(STM32N6_LTDC_IER, STM32N6_LTDC_IER_ALL);
    test_mmio_write(STM32N6_LTDC_IER2, STM32N6_LTDC_IER_ALL);
    test_mmio_write(STM32N6_LTDC_GCR, STM32N6_LTDC_GCR_LTDCEN);
    assert(tiku_ltdc_deinit(&handle) == TIKU_LTDC_OK);
    assert(handle.state == TIKU_LTDC_STATE_RESET);
    assert(handle.global_configured == 0U);
    assert(handle.error_callback == 0);
    assert((test_mmio_read(STM32N6_RCC_APB5ENR) &
            STM32N6_RCC_APB5ENR_LTDC) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_GCR) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_IER) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_IER2) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_ICR) == STM32N6_LTDC_ICR_ALL);
    assert(test_mmio_read(STM32N6_LTDC_ICR2) == STM32N6_LTDC_ICR_ALL);
}

static void test_global_configuration(void) {
    tiku_ltdc_handle_t handle;
    tiku_ltdc_global_config_t config = valid_global_config();
    size_t writes_before;

    test_mmio_reset();
    assert(tiku_ltdc_init(&handle) == TIKU_LTDC_OK);
    write_count = 0U;
    config.active_width = 0UL;
    assert(tiku_ltdc_configure(&handle, &config) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    config = valid_global_config();
    config.h_front_porch = UINT32_MAX;
    assert(tiku_ltdc_configure(&handle, &config) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    config = valid_global_config();
    config.hsync_polarity = (tiku_ltdc_polarity_t)-1;
    assert(tiku_ltdc_configure(&handle, &config) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);

    config = valid_global_config();
    assert(tiku_ltdc_configure(&handle, &config) == TIKU_LTDC_OK);
    assert(handle.state == TIKU_LTDC_STATE_CONFIGURED);
    assert(handle.global_configured == 1U);
    assert(test_mmio_read(STM32N6_LTDC_SSCR) ==
           ((39UL << 16U) | 2UL));
    assert(test_mmio_read(STM32N6_LTDC_BPCR) ==
           ((127UL << 16U) | 34UL));
    assert(test_mmio_read(STM32N6_LTDC_AWCR) ==
           ((927UL << 16U) | 514UL));
    assert(test_mmio_read(STM32N6_LTDC_TWCR) ==
           ((967UL << 16U) | 524UL));
    assert(test_mmio_read(STM32N6_LTDC_BCCR) == 0x123456UL);
    assert(test_mmio_read(STM32N6_LTDC_SRCR) == STM32N6_LTDC_SRCR_IMR);
    assert((test_mmio_read(STM32N6_LTDC_GCR) &
            (STM32N6_LTDC_GCR_HSPOL | STM32N6_LTDC_GCR_DEPOL |
             STM32N6_LTDC_GCR_PCPOL | STM32N6_LTDC_GCR_DEN)) ==
           (STM32N6_LTDC_GCR_HSPOL | STM32N6_LTDC_GCR_DEPOL |
            STM32N6_LTDC_GCR_PCPOL | STM32N6_LTDC_GCR_DEN));

    handle.layer_configured[0] = 1U;
    handle.layer_configured[1] = 1U;
    handle.framebuffer_set[0] = 1U;
    handle.framebuffer[0] = 0x20000000UL;
    handle.state = TIKU_LTDC_STATE_ACTIVE;
    test_mmio_write(STM32N6_LTDC_GCR,
                    test_mmio_read(STM32N6_LTDC_GCR) |
                        STM32N6_LTDC_GCR_LTDCEN);
    writes_before = write_count;
    config.background_rgb = 0xABCDEFUL;
    assert(tiku_ltdc_configure(&handle, &config) == TIKU_LTDC_OK);
    assert(write_count > writes_before);
    assert(writes[writes_before].address == STM32N6_LTDC_GCR);
    assert((writes[writes_before].value & STM32N6_LTDC_GCR_LTDCEN) == 0UL);
    assert(handle.state == TIKU_LTDC_STATE_CONFIGURED);
    assert(handle.layer_configured[0] == 0U);
    assert(handle.layer_configured[1] == 0U);
    assert(handle.framebuffer_set[0] == 1U);
    assert(handle.framebuffer[0] == 0x20000000UL);
    assert(tiku_ltdc_deinit(&handle) == TIKU_LTDC_OK);
}

static void test_layer_formats_and_reload(void) {
    static const struct {
        tiku_ltdc_pixel_format_t format;
        uint32_t bytes_per_pixel;
        uint32_t pfcr;
        uint32_t fpf0r;
        uint32_t fpf1r;
    } formats[] = {
        {TIKU_LTDC_PIXEL_FORMAT_ARGB8888, 4UL, 0UL, 0UL, 0UL},
        {TIKU_LTDC_PIXEL_FORMAT_RGB888, 3UL, 6UL, 0UL, 0UL},
        {TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL, 4UL, 0UL, 0UL},
        {TIKU_LTDC_PIXEL_FORMAT_ARGB1555, 2UL, 7UL,
         (1UL << 5U) | 15UL | (5UL << 14U) | (10UL << 9U),
         (2UL << 18U) | (5UL << 14U) | (5UL << 5U) | (5UL)},
        {TIKU_LTDC_PIXEL_FORMAT_ARGB4444, 2UL, 7UL,
         (4UL << 5U) | 12UL | (4UL << 14U) | (8UL << 9U),
         (2UL << 18U) | (4UL << 14U) | (4UL << 5U) | (4UL)},
        {TIKU_LTDC_PIXEL_FORMAT_L8, 1UL, 7UL,
         (8UL << 14U), (1UL << 18U) | (8UL << 14U) | (8UL << 5U)},
        {TIKU_LTDC_PIXEL_FORMAT_AL44, 1UL, 7UL,
         (4UL << 5U) | 4UL | (4UL << 14U),
         (1UL << 18U) | (4UL << 14U) | (4UL << 5U)},
        {TIKU_LTDC_PIXEL_FORMAT_AL88, 2UL, 7UL,
         (8UL << 5U) | 8UL | (8UL << 14U),
         (2UL << 18U) | (8UL << 14U) | (8UL << 5U)},
    };
    tiku_ltdc_handle_t handle;
    tiku_ltdc_global_config_t global = valid_global_config();
    size_t i;

    test_mmio_reset();
    assert(tiku_ltdc_init(&handle) == TIKU_LTDC_OK);
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_OK);
    for (i = 0U; i < (sizeof(formats) / sizeof(formats[0])); ++i) {
        tiku_ltdc_layer_config_t layer =
            valid_layer_config(formats[i].format, formats[i].bytes_per_pixel);
        assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1,
                                         &layer) == TIKU_LTDC_OK);
        assert(test_mmio_read(STM32N6_LTDC_LPFCR(0U)) == formats[i].pfcr);
        assert(test_mmio_read(STM32N6_LTDC_LFPF0R(0U)) == formats[i].fpf0r);
        assert(test_mmio_read(STM32N6_LTDC_LFPF1R(0U)) == formats[i].fpf1r);
        assert(test_mmio_read(STM32N6_LTDC_LCFBLR(0U)) ==
               ((layer.pitch_bytes << 16U) |
                (8UL * formats[i].bytes_per_pixel + 7UL)));
        assert(test_mmio_read(STM32N6_LTDC_LCFBLNR(0U)) == 4UL);
        assert(test_mmio_read(STM32N6_LTDC_LWHPCR(0U)) ==
               ((146UL << 16U) | 139UL));
        assert(test_mmio_read(STM32N6_LTDC_LWVPCR(0U)) ==
               ((41UL << 16U) | 38UL));
        assert(test_mmio_read(STM32N6_LTDC_LCACR(0U)) == 127UL);
        assert(test_mmio_read(STM32N6_LTDC_LDCCR(0U)) == 0xA1B2C3D4UL);
        assert(test_mmio_read(STM32N6_LTDC_LBFCR(0U)) ==
               (STM32N6_LTDC_BFCR_BF1_PAXCA | STM32N6_LTDC_BFCR_BF2_CA));
        assert(test_mmio_read(STM32N6_LTDC_SRCR) == STM32N6_LTDC_SRCR_IMR);
    }

    handle.state = TIKU_LTDC_STATE_ACTIVE;
    {
        tiku_ltdc_layer_config_t layer =
            valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
        assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_2,
                                         &layer) == TIKU_LTDC_OK);
        assert(test_mmio_read(STM32N6_LTDC_SRCR) == STM32N6_LTDC_SRCR_VBR);
        assert(handle.layer_configured[1] == 1U);
    }
    assert(tiku_ltdc_deinit(&handle) == TIKU_LTDC_OK);
}

static void test_invalid_layer_config_has_no_writes(void) {
    tiku_ltdc_handle_t handle;
    tiku_ltdc_global_config_t global = valid_global_config();
    tiku_ltdc_layer_config_t layer =
        valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);

    test_mmio_reset();
    assert(tiku_ltdc_init(&handle) == TIKU_LTDC_OK);
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_OK);
    write_count = 0U;

    assert(tiku_ltdc_configure_layer(&handle, (tiku_ltdc_layer_t)-1,
                                     &layer) == TIKU_LTDC_ERR_ARG);
    assert(write_count == 0U);
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_COUNT,
                                     &layer) == TIKU_LTDC_ERR_ARG);
    assert(write_count == 0U);

    layer.x_end = global.active_width + 1UL;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1,
                                     &layer) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.pitch_bytes = 15UL;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1,
                                     &layer) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.framebuffer_lines = 3UL;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1,
                                     &layer) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config((tiku_ltdc_pixel_format_t)0x55, 2UL);
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1,
                                     &layer) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    assert(tiku_ltdc_deinit(&handle) == TIKU_LTDC_OK);
}

static void test_null_and_invalid_inputs_have_no_writes(void) {
    tiku_ltdc_handle_t handle;
    tiku_ltdc_global_config_t global = valid_global_config();
    tiku_ltdc_layer_config_t layer =
        valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    tiku_ltdc_state_t state;
    tiku_ltdc_error_flags_t errors;
    size_t writes_before;

    test_mmio_reset();
    assert(tiku_ltdc_init(&handle) == TIKU_LTDC_OK);
    write_count = 0U;
    assert(tiku_ltdc_configure(0, &global) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_configure(&handle, 0) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_configure_layer(0, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, 0) ==
           TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_set_layer_framebuffer(0, TIKU_LTDC_LAYER_1,
                                          0x20001000UL) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_start(0) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_stop(0) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_deinit(0) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_get_state(0, &state) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_get_state(&handle, 0) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_get_error_flags(0, &errors) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_get_error_flags(&handle, 0) == TIKU_LTDC_ERR_ARG);
    assert(tiku_ltdc_register_error_callback(0, test_error_callback) ==
           TIKU_LTDC_ERR_ARG);
    assert(write_count == 0U);

    global = valid_global_config();
    global.vsync_polarity = (tiku_ltdc_polarity_t)2;
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    global = valid_global_config();
    global.data_enable_polarity = (tiku_ltdc_polarity_t)-1;
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    global = valid_global_config();
    global.pixel_clock_polarity = (tiku_ltdc_pixel_clock_polarity_t)2;
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    global = valid_global_config();
    global.dithering_enabled = 2U;
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);

    /* Valid configuration enables layer validation; invalid enum and window,
     * pitch, and line-count values must still be rejected before MMIO writes. */
    global = valid_global_config();
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_OK);
    write_count = 0U;

    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.blending_factor1 = (tiku_ltdc_blend_factor_t)2;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.blending_factor2 = (tiku_ltdc_blend_factor_t)-1;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.x_start = layer.x_end;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.y_end = 0UL;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.pitch_bytes = 65536UL;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    layer.framebuffer_lines = 65536UL;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);

    global = valid_global_config();
    global.active_width = 16383UL;
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_OK);
    write_count = 0U;
    layer = valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_ARGB8888, 4UL);
    layer.x_end = global.active_width;
    layer.pitch_bytes = 65535UL;
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1, &layer) ==
           TIKU_LTDC_ERR_RANGE);
    assert(write_count == 0U);

    writes_before = write_count;
    assert(tiku_ltdc_deinit(&handle) == TIKU_LTDC_OK);
    assert(write_count > writes_before);
}

static void test_framebuffer_and_scanout(void) {
    tiku_ltdc_handle_t handle;
    tiku_ltdc_handle_t second_handle;
    tiku_ltdc_global_config_t global = valid_global_config();
    tiku_ltdc_layer_config_t layer =
        valid_layer_config(TIKU_LTDC_PIXEL_FORMAT_RGB565, 2UL);
    tiku_ltdc_state_t state;
    tiku_ltdc_error_flags_t errors;
    size_t writes_before;
    uint32_t error_sources = STM32N6_LTDC_ISR_FUIF |
                             STM32N6_LTDC_ISR_TERRIF;

    test_mmio_reset();
    callback_reset();
    assert(tiku_ltdc_init(&handle) == TIKU_LTDC_OK);
    assert(tiku_ltdc_init(&second_handle) == TIKU_LTDC_ERR_STATE);
    assert(tiku_ltdc_configure(&handle, &global) == TIKU_LTDC_OK);

    writes_before = write_count;
    assert(tiku_ltdc_start(&handle) == TIKU_LTDC_ERR_STATE);
    assert(write_count == writes_before);
    assert(tiku_ltdc_set_layer_framebuffer(&handle, TIKU_LTDC_LAYER_1,
                                           0x20001000UL) ==
           TIKU_LTDC_ERR_STATE);
    assert(write_count == writes_before);
    assert(tiku_ltdc_configure_layer(&handle, TIKU_LTDC_LAYER_1,
                                     &layer) == TIKU_LTDC_OK);

    writes_before = write_count;
    assert(tiku_ltdc_start(&handle) == TIKU_LTDC_ERR_STATE);
    assert(tiku_ltdc_stop(&handle) == TIKU_LTDC_ERR_STATE);
    assert(write_count == writes_before);

    writes_before = write_count;
    assert(tiku_ltdc_set_layer_framebuffer(&handle, TIKU_LTDC_LAYER_COUNT,
                                           0x20001000UL) == TIKU_LTDC_ERR_ARG);
    assert(write_count == writes_before);
#if UINTPTR_MAX > UINT32_MAX
    assert(tiku_ltdc_set_layer_framebuffer(
               &handle, TIKU_LTDC_LAYER_1,
               (uintptr_t)UINT32_MAX + (uintptr_t)1U) == TIKU_LTDC_ERR_RANGE);
    assert(write_count == writes_before);
#endif

    assert(tiku_ltdc_set_layer_framebuffer(&handle, TIKU_LTDC_LAYER_1,
                                           (uintptr_t)0x20001000UL) ==
           TIKU_LTDC_OK);
    assert(test_mmio_read(STM32N6_LTDC_LCFBAR(0U)) == 0x20001000UL);
    assert(test_mmio_read(STM32N6_LTDC_SRCR) == STM32N6_LTDC_SRCR_IMR);
    assert(handle.framebuffer_set[0] != 0U);
    assert(handle.framebuffer[0] == (uintptr_t)0x20001000UL);

    assert(tiku_ltdc_start(&handle) == TIKU_LTDC_OK);
    assert(handle.state == TIKU_LTDC_STATE_ACTIVE);
    assert((test_mmio_read(STM32N6_LTDC_GCR) & STM32N6_LTDC_GCR_LTDCEN) != 0UL);
    assert(test_mmio_read(STM32N6_LTDC_IER) == error_sources);
    assert(test_mmio_read(STM32N6_LTDC_IER2) == error_sources);
    writes_before = write_count;
    assert(tiku_ltdc_start(&handle) == TIKU_LTDC_ERR_STATE);
    assert(write_count == writes_before);

    assert(tiku_ltdc_set_layer_framebuffer(&handle, TIKU_LTDC_LAYER_1,
                                           (uintptr_t)0x20002000UL) ==
           TIKU_LTDC_OK);
    assert(test_mmio_read(STM32N6_LTDC_LCFBAR(0U)) == 0x20002000UL);
    assert(test_mmio_read(STM32N6_LTDC_SRCR) == STM32N6_LTDC_SRCR_VBR);
    assert(handle.framebuffer[0] == (uintptr_t)0x20002000UL);

    test_mmio_write(STM32N6_LTDC_ISR,
                    STM32N6_LTDC_ISR_RRIF | STM32N6_LTDC_ISR_FUIF);
    test_mmio_write(STM32N6_LTDC_ISR2, STM32N6_LTDC_ISR_TERRIF);
    assert(tiku_ltdc_stop(&handle) == TIKU_LTDC_OK);
    assert(handle.state == TIKU_LTDC_STATE_CONFIGURED);
    assert((test_mmio_read(STM32N6_LTDC_GCR) & STM32N6_LTDC_GCR_LTDCEN) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_IER) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_IER2) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_ISR) == 0UL);
    assert(test_mmio_read(STM32N6_LTDC_ISR2) == 0UL);
    assert(handle.layer_configured[0] != 0U);
    assert(handle.framebuffer_set[0] != 0U);
    assert(handle.framebuffer[0] == (uintptr_t)0x20002000UL);
    writes_before = write_count;
    assert(tiku_ltdc_stop(&handle) == TIKU_LTDC_ERR_STATE);
    assert(write_count == writes_before);
    assert(tiku_ltdc_get_state(&handle, &state) == TIKU_LTDC_OK);
    assert(state == TIKU_LTDC_STATE_CONFIGURED);
    assert(tiku_ltdc_get_error_flags(&handle, &errors) == TIKU_LTDC_OK);
    assert(errors == 0UL);
    assert(tiku_ltdc_register_error_callback(&handle, test_error_callback) ==
           TIKU_LTDC_OK);

    assert(tiku_ltdc_start(&handle) == TIKU_LTDC_OK);
    assert(handle.state == TIKU_LTDC_STATE_ACTIVE);
    assert(test_mmio_read(STM32N6_LTDC_IER) == error_sources);
    assert(test_mmio_read(STM32N6_LTDC_IER2) == error_sources);
    assert(tiku_ltdc_stop(&handle) == TIKU_LTDC_OK);
    assert(tiku_ltdc_deinit(&handle) == TIKU_LTDC_OK);
    assert(tiku_ltdc_init(&second_handle) == TIKU_LTDC_OK);
    assert(tiku_ltdc_deinit(&second_handle) == TIKU_LTDC_OK);
}

static void test_irq_bank_routing_and_errors(void) {
    tiku_ltdc_handle_t handle;
    uint32_t writes_before;
    tiku_ltdc_error_flags_t errors;

    test_mmio_reset();
    callback_reset();
    assert(tiku_ltdc_init(&handle) == TIKU_LTDC_OK);

    test_mmio_write(STM32N6_LTDC_ISR,
                    STM32N6_LTDC_ISR_RRIF | STM32N6_LTDC_ISR_LIF);
    test_mmio_write(STM32N6_LTDC_ISR2,
                    STM32N6_LTDC_ISR_RRIF | STM32N6_LTDC_ISR_LIF);
    writes_before = (uint32_t)write_count;
    tiku_stm32n6_ltdc_lo_isr();
    assert(write_count == (size_t)writes_before + 1U);
    assert(writes[write_count - 1U].address == STM32N6_LTDC_ICR);
    assert(writes[write_count - 1U].value == STM32N6_LTDC_ISR_RRIF);
    assert(test_mmio_read(STM32N6_LTDC_ISR) == STM32N6_LTDC_ISR_LIF);
    assert(test_mmio_read(STM32N6_LTDC_ISR2) ==
           (STM32N6_LTDC_ISR_RRIF | STM32N6_LTDC_ISR_LIF));
    writes_before = (uint32_t)write_count;
    tiku_stm32n6_ltdc_lo_isr();
    assert(write_count == (size_t)writes_before);

    tiku_stm32n6_ltdc_up_isr();
    assert(writes[write_count - 1U].address == STM32N6_LTDC_ICR2);
    assert(writes[write_count - 1U].value == STM32N6_LTDC_ISR_RRIF);
    assert(test_mmio_read(STM32N6_LTDC_ISR2) == STM32N6_LTDC_ISR_LIF);
    tiku_stm32n6_ltdc_up_isr();
    assert(writes[write_count - 1U].value == STM32N6_LTDC_ISR_RRIF);

    assert(tiku_ltdc_register_error_callback(&handle, test_error_callback) ==
           TIKU_LTDC_OK);
    test_mmio_write(STM32N6_LTDC_ISR,
                    STM32N6_LTDC_ISR_FUIF | STM32N6_LTDC_ISR_FUWIF |
                        STM32N6_LTDC_ISR_CRCIF);
    tiku_stm32n6_ltdc_lo_err_isr();
    assert(writes[write_count - 1U].address == STM32N6_LTDC_ICR);
    assert(writes[write_count - 1U].value == STM32N6_LTDC_ISR_FUIF);
    assert(test_mmio_read(STM32N6_LTDC_ISR) ==
           (STM32N6_LTDC_ISR_FUWIF | STM32N6_LTDC_ISR_CRCIF));
    assert(callback_count == 1UL);
    assert(callback_handle == &handle);
    assert(callback_errors == TIKU_LTDC_ERROR_FIFO_UNDERRUN);

    test_mmio_write(STM32N6_LTDC_ISR2,
                    STM32N6_LTDC_ISR_TERRIF | STM32N6_LTDC_ISR_FUWIF);
    tiku_stm32n6_ltdc_up_err_isr();
    assert(writes[write_count - 1U].address == STM32N6_LTDC_ICR2);
    assert(writes[write_count - 1U].value == STM32N6_LTDC_ISR_TERRIF);
    assert(test_mmio_read(STM32N6_LTDC_ISR2) == STM32N6_LTDC_ISR_FUWIF);
    assert(callback_count == 2UL);
    assert(callback_errors == TIKU_LTDC_ERROR_TRANSFER);
    assert(tiku_ltdc_get_error_flags(&handle, &errors) == TIKU_LTDC_OK);
    assert(errors == (TIKU_LTDC_ERROR_FIFO_UNDERRUN |
                      TIKU_LTDC_ERROR_TRANSFER));

    writes_before = (uint32_t)write_count;
    tiku_stm32n6_ltdc_lo_err_isr();
    tiku_stm32n6_ltdc_up_err_isr();
    assert(write_count == (size_t)writes_before);
    assert(callback_count == 2UL);
    assert(tiku_ltdc_deinit(&handle) == TIKU_LTDC_OK);
}

int main(void) {
    test_init_and_deinit();
    test_global_configuration();
    test_layer_formats_and_reload();
    test_invalid_layer_config_has_no_writes();
    test_null_and_invalid_inputs_have_no_writes();
    test_framebuffer_and_scanout();
    test_irq_bank_routing_and_errors();
    puts("STM32N6 LTDC host register tests passed");
    return 0;
}
