/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_ltdc_arch.h - STM32N657 LTDC architecture API.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32N6_LTDC_ARCH_H_
#define TIKU_STM32N6_LTDC_ARCH_H_

#include <stdint.h>

/** @brief Result returned by an LTDC operation. */
typedef enum {
    TIKU_LTDC_OK = 0,
    TIKU_LTDC_ERR_ARG,
    TIKU_LTDC_ERR_STATE,
    TIKU_LTDC_ERR_RANGE,
    TIKU_LTDC_ERR_CLOCK,
} tiku_ltdc_status_t;

/** @brief Requested panel pixel clock; the driver reports the selected rate. */
typedef struct {
    uint32_t pixel_clock_hz;
} tiku_ltdc_clock_config_t;

/** @brief Driver lifecycle and scanout state. */
typedef enum {
    TIKU_LTDC_STATE_RESET = 0,
    TIKU_LTDC_STATE_INIT,
    TIKU_LTDC_STATE_CONFIGURED,
    TIKU_LTDC_STATE_ACTIVE,
} tiku_ltdc_state_t;

/** @brief LTDC layer index. Layer 2 access depends on platform security setup. */
typedef enum {
    TIKU_LTDC_LAYER_1 = 0,
    TIKU_LTDC_LAYER_2 = 1,
    TIKU_LTDC_LAYER_COUNT = 2,
} tiku_ltdc_layer_t;

/** @brief Pixel formats supported by the STM32N657 LTDC API. */
typedef enum {
    TIKU_LTDC_PIXEL_FORMAT_ARGB8888 = 0x0,
    TIKU_LTDC_PIXEL_FORMAT_RGB888   = 0x6,
    TIKU_LTDC_PIXEL_FORMAT_RGB565   = 0x4,
    TIKU_LTDC_PIXEL_FORMAT_ARGB1555 = 0x7,
    TIKU_LTDC_PIXEL_FORMAT_ARGB4444 = 0x8,
    TIKU_LTDC_PIXEL_FORMAT_L8       = 0x9,
    TIKU_LTDC_PIXEL_FORMAT_AL44     = 0xA,
    TIKU_LTDC_PIXEL_FORMAT_AL88     = 0xB,
} tiku_ltdc_pixel_format_t;

/** @brief Active level for HSYNC, VSYNC, and data-enable. */
typedef enum {
    TIKU_LTDC_POLARITY_ACTIVE_LOW = 0,
    TIKU_LTDC_POLARITY_ACTIVE_HIGH = 1,
} tiku_ltdc_polarity_t;

/** @brief Pixel-clock sampling edge selection. */
typedef enum {
    TIKU_LTDC_PIXEL_CLOCK_NORMAL = 0,
    TIKU_LTDC_PIXEL_CLOCK_INVERTED = 1,
} tiku_ltdc_pixel_clock_polarity_t;

/** @brief Source factor used by the LTDC layer blender. */
typedef enum {
    TIKU_LTDC_BLEND_CONSTANT_ALPHA = 0,
    TIKU_LTDC_BLEND_PIXEL_ALPHA_X_CONSTANT_ALPHA = 1,
} tiku_ltdc_blend_factor_t;

/** @brief Accumulated errors reported by the LTDC error handler. */
typedef uint32_t tiku_ltdc_error_flags_t;
#define TIKU_LTDC_ERROR_FIFO_UNDERRUN (1UL << 0)
#define TIKU_LTDC_ERROR_TRANSFER      (1UL << 1)

/** @brief Global LTDC timing, polarity, background, and output configuration. */
typedef struct {
    /* Counts are unsigned so invalid cumulative sums can be rejected before
     * encoding the hardware's 16-bit cumulative-count-minus-one fields. */
    uint32_t active_width;
    uint32_t active_height;
    uint32_t hsync_width;
    uint32_t h_back_porch;
    uint32_t h_front_porch;
    uint32_t vsync_height;
    uint32_t v_back_porch;
    uint32_t v_front_porch;
    tiku_ltdc_polarity_t hsync_polarity;
    tiku_ltdc_polarity_t vsync_polarity;
    tiku_ltdc_polarity_t data_enable_polarity;
    tiku_ltdc_pixel_clock_polarity_t pixel_clock_polarity;
    uint32_t background_rgb;
    uint8_t dithering_enabled;
} tiku_ltdc_global_config_t;

/** @brief Layer window, pixel layout, and blending configuration. */
typedef struct {
    /* Zero-based half-open window: [x_start, x_end) x [y_start, y_end). */
    uint32_t x_start;
    uint32_t x_end;
    uint32_t y_start;
    uint32_t y_end;
    tiku_ltdc_pixel_format_t pixel_format;
    uint32_t pitch_bytes;
    uint32_t framebuffer_lines;
    uint8_t constant_alpha;
    tiku_ltdc_blend_factor_t blending_factor1;
    tiku_ltdc_blend_factor_t blending_factor2;
    uint32_t default_argb;
} tiku_ltdc_layer_config_t;

struct tiku_ltdc_handle;
typedef struct tiku_ltdc_handle tiku_ltdc_handle_t;

/** @brief Called with error flags observed by the latest error interrupt. */
typedef void (*tiku_ltdc_error_callback_t)(tiku_ltdc_handle_t *handle,
                                            tiku_ltdc_error_flags_t errors);

/**
 * @brief Caller-owned LTDC state.
 *
 * The fields are public so the caller can allocate this object, but are
 * private to the driver by convention. Do not inspect or modify them.
 */
struct tiku_ltdc_handle {
    tiku_ltdc_state_t state;
    tiku_ltdc_global_config_t global_config;
    tiku_ltdc_layer_config_t layer_config[TIKU_LTDC_LAYER_COUNT];
    uintptr_t framebuffer[TIKU_LTDC_LAYER_COUNT];
    uint8_t global_configured;
    uint8_t layer_configured[TIKU_LTDC_LAYER_COUNT];
    uint8_t framebuffer_set[TIKU_LTDC_LAYER_COUNT];
    tiku_ltdc_error_callback_t error_callback;
    tiku_ltdc_error_flags_t error_flags;
};

/**
 * @brief Configure the dedicated PLL4 -> IC16 pixel-clock path.
 *
 * Call before tiku_ltdc_init(). The nearest legal output rate is returned in
 * actual_pixel_clock_hz; the caller must verify it is within the panel's
 * permitted range. Requests above the STM32N6 88 MHz LTDC limit are rejected.
 */
tiku_ltdc_status_t tiku_ltdc_clock_configure(
    const tiku_ltdc_clock_config_t *config,
    uint32_t *actual_pixel_clock_hz);

/** @brief Initialize a caller-owned handle after successful clock setup. */
tiku_ltdc_status_t tiku_ltdc_init(tiku_ltdc_handle_t *handle);

/** @brief Disable LTDC activity and return the handle to RESET. */
tiku_ltdc_status_t tiku_ltdc_deinit(tiku_ltdc_handle_t *handle);

/** @brief Apply global timing and output configuration. */
tiku_ltdc_status_t tiku_ltdc_configure(tiku_ltdc_handle_t *handle,
                                       const tiku_ltdc_global_config_t *config);

/** @brief Configure one layer using a zero-based, half-open window. */
tiku_ltdc_status_t tiku_ltdc_configure_layer(
    tiku_ltdc_handle_t *handle, tiku_ltdc_layer_t layer,
    const tiku_ltdc_layer_config_t *config);

/** @brief Set a layer framebuffer address (the LTDC register is 32-bit). */
tiku_ltdc_status_t tiku_ltdc_set_layer_framebuffer(
    tiku_ltdc_handle_t *handle, tiku_ltdc_layer_t layer, uintptr_t address);

/** @brief Start scanout from CONFIGURED state. */
tiku_ltdc_status_t tiku_ltdc_start(tiku_ltdc_handle_t *handle);

/** @brief Stop scanout while retaining the applied configuration. */
tiku_ltdc_status_t tiku_ltdc_stop(tiku_ltdc_handle_t *handle);

/** @brief Read the current lifecycle state. */
tiku_ltdc_status_t tiku_ltdc_get_state(const tiku_ltdc_handle_t *handle,
                                       tiku_ltdc_state_t *state);

/** @brief Read accumulated FIFO-underrun and transfer-error flags. */
tiku_ltdc_status_t tiku_ltdc_get_error_flags(
    const tiku_ltdc_handle_t *handle, tiku_ltdc_error_flags_t *errors);

/** @brief Register or clear the single error callback. */
tiku_ltdc_status_t tiku_ltdc_register_error_callback(
    tiku_ltdc_handle_t *handle, tiku_ltdc_error_callback_t callback);

/** @brief Service general LTDC status in both layer status banks. */
void tiku_ltdc_irq_handler(void);

/** @brief Service LTDC errors in both layer status banks. */
void tiku_ltdc_error_irq_handler(void);

#endif /* TIKU_STM32N6_LTDC_ARCH_H_ */
