/* STM32N6570-DK full-screen TFT RGB color-cycle process. */
#include <stddef.h>
#include <stdint.h>

#include <kernel/process/tiku_process.h>
#include <kernel/timers/tiku_timer.h>

#include <arch/stm32n6/tiku_cache_arch.h>
#include <arch/stm32n6/tiku_cpu_common.h>
#include <arch/stm32n6/tiku_device_select.h>
#include <arch/stm32n6/tiku_gpio_arch.h>
#include <arch/stm32n6/tiku_ltdc_arch.h>

#if defined(TIKU_STM32N6_DK_TFT_COLORCYCLE_TEST_ENABLE) && \
    TIKU_STM32N6_DK_TFT_COLORCYCLE_TEST_ENABLE

#if !defined(TIKU_BOARD_STM32N6570_DK)
#error "The STM32N6 DK TFT color-cycle test requires the STM32N6570-DK board"
#endif

#define TFT_WIDTH              800U
#define TFT_HEIGHT             480U
#define TFT_BYTES_PER_PIXEL    2U
#define TFT_PITCH_BYTES        (TFT_WIDTH * TFT_BYTES_PER_PIXEL)
#define TFT_FRAMEBUFFER_BYTES  (TFT_PITCH_BYTES * TFT_HEIGHT)
#define TFT_PIXEL_CLOCK_HZ     25000000UL
#define TFT_RED_RGB565         0xF800U
#define TFT_GREEN_RGB565       0x07E0U
#define TFT_BLUE_RGB565        0x001FU

#ifndef TFT_COLOR_ROTATION_SECONDS
#define TFT_COLOR_ROTATION_SECONDS 2UL
#endif
#define TFT_COLOR_ROTATION_TICKS \
    ((tiku_clock_time_t)(TFT_COLOR_ROTATION_SECONDS * TIKU_CLOCK_SECOND))
#define TFT_COLOR_COUNT         3U

static uint16_t tft_color_framebuffer[TFT_WIDTH * TFT_HEIGHT]
    __attribute__((section(".axisram"), aligned(32)));

static tiku_ltdc_handle_t tft_ltdc;
static struct tiku_timer tft_color_timer;
static uint8_t tft_color_index;
TIKU_PROCESS(stm32n6_dk_tft_colorcycle, "STM32N6 DK TFT color cycle");

static const uint16_t tft_colors[TFT_COLOR_COUNT] = {
    TFT_RED_RGB565,
    TFT_GREEN_RGB565,
    TFT_BLUE_RGB565,
};
static const char *const tft_color_names[TFT_COLOR_COUNT] = {
    "RED",
    "GREEN",
    "BLUE",
};

typedef enum {
    TFT_STAGE_CLOCK = 1,
    TFT_STAGE_CLOCK_RATE,
    TFT_STAGE_LTDC_INIT,
    TFT_STAGE_GLOBAL_CONFIG,
    TFT_STAGE_LAYER_CONFIG,
    TFT_STAGE_FRAMEBUFFER,
    TFT_STAGE_START,
    TFT_STAGE_SCANOUT_ERRORS,
} tft_stage_t;

static void tft_pins_init(void)
{
    /* RGB, sync, data-enable, and pixel-clock pins use LTDC AF14. */
    static const uint8_t pins[][2] = {
        {6U, 0U}, {3U, 9U}, {3U, 15U}, {1U, 4U}, {7U, 4U}, {0U, 15U},
        {6U, 11U}, {3U, 8U}, {6U, 12U}, {6U, 1U}, {0U, 1U}, {0U, 0U},
        {1U, 15U}, {1U, 12U}, {1U, 11U}, {6U, 8U}, {6U, 15U}, {0U, 7U},
        {1U, 2U}, {6U, 6U}, {7U, 3U}, {7U, 6U}, {0U, 8U}, {0U, 2U},
        {1U, 14U}, {6U, 13U}, {4U, 11U}, {1U, 13U},
    };
    size_t i;

    for (i = 0U; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        tiku_stm32n6_gpio_init_alt(pins[i][0], pins[i][1], 14U);
    }
}

static int tft_configure(tft_stage_t *failed_stage, int *error_code)
{
    const tiku_ltdc_clock_config_t clock = {
        .pixel_clock_hz = TFT_PIXEL_CLOCK_HZ,
    };
    const tiku_ltdc_global_config_t global = {
        .active_width = TFT_WIDTH,
        .active_height = TFT_HEIGHT,
        .hsync_width = 4U,
        .h_back_porch = 4U,
        .h_front_porch = 4U,
        .vsync_height = 4U,
        .v_back_porch = 4U,
        .v_front_porch = 4U,
        .hsync_polarity = TIKU_LTDC_POLARITY_ACTIVE_LOW,
        .vsync_polarity = TIKU_LTDC_POLARITY_ACTIVE_LOW,
        .data_enable_polarity = TIKU_LTDC_POLARITY_ACTIVE_LOW,
        .pixel_clock_polarity = TIKU_LTDC_PIXEL_CLOCK_INVERTED,
        .background_rgb = 0UL,
        .dithering_enabled = 0U,
    };
    const tiku_ltdc_layer_config_t layer = {
        .x_start = 0U,
        .x_end = TFT_WIDTH,
        .y_start = 0U,
        .y_end = TFT_HEIGHT,
        .pixel_format = TIKU_LTDC_PIXEL_FORMAT_RGB565,
        .pitch_bytes = TFT_PITCH_BYTES,
        .framebuffer_lines = TFT_HEIGHT,
        .constant_alpha = 255U,
        .blending_factor1 = TIKU_LTDC_BLEND_CONSTANT_ALPHA,
        .blending_factor2 = TIKU_LTDC_BLEND_CONSTANT_ALPHA,
        .default_argb = 0xFF000000UL,
    };
    tiku_ltdc_status_t status;
    uint32_t actual_pixel_clock_hz = 0U;
    size_t i;

    TIKU_BOARD_LCD_INIT();
    tiku_cpu_stm32n6_delay_ms(10U);
    TIKU_BOARD_LCD_RESET_RELEASE();
    tiku_cpu_stm32n6_delay_ms(10U);
    tft_pins_init();

    status = tiku_ltdc_clock_configure(&clock, &actual_pixel_clock_hz);
    if (status != TIKU_LTDC_OK) {
        *failed_stage = TFT_STAGE_CLOCK;
        *error_code = (int)status;
        return -1;
    }
    if (actual_pixel_clock_hz != TFT_PIXEL_CLOCK_HZ) {
        *failed_stage = TFT_STAGE_CLOCK_RATE;
        *error_code = (int)actual_pixel_clock_hz;
        return -1;
    }

    status = tiku_ltdc_init(&tft_ltdc);
    if (status != TIKU_LTDC_OK) {
        *failed_stage = TFT_STAGE_LTDC_INIT;
        *error_code = (int)status;
        return -1;
    }
    status = tiku_ltdc_configure(&tft_ltdc, &global);
    if (status != TIKU_LTDC_OK) {
        *failed_stage = TFT_STAGE_GLOBAL_CONFIG;
        *error_code = (int)status;
        return -1;
    }
    status = tiku_ltdc_configure_layer(&tft_ltdc, TIKU_LTDC_LAYER_1,
                                       &layer);
    if (status != TIKU_LTDC_OK) {
        *failed_stage = TFT_STAGE_LAYER_CONFIG;
        *error_code = (int)status;
        return -1;
    }

    for (i = 0U; i < (size_t)(TFT_WIDTH * TFT_HEIGHT); ++i) {
        tft_color_framebuffer[i] = TFT_RED_RGB565;
    }
    tiku_stm32n6_dcache_clean(tft_color_framebuffer, TFT_FRAMEBUFFER_BYTES);

    status = tiku_ltdc_set_layer_framebuffer(
        &tft_ltdc, TIKU_LTDC_LAYER_1,
        (uintptr_t)tft_color_framebuffer);
    if (status != TIKU_LTDC_OK) {
        *failed_stage = TFT_STAGE_FRAMEBUFFER;
        *error_code = (int)status;
        return -1;
    }
    status = tiku_ltdc_start(&tft_ltdc);
    if (status != TIKU_LTDC_OK) {
        *failed_stage = TFT_STAGE_START;
        *error_code = (int)status;
        return -1;
    }
    return 0;
}

static void tft_fail(tft_stage_t stage, int error_code)
{
    TIKU_BOARD_LCD_BACKLIGHT_OFF();
    TIKU_BOARD_LCD_DISABLE();
    TIKU_BOARD_LCD_RESET_ASSERT();
    TIKU_PRINTF("TFT colorcycle: FAIL stage=%u code=%d\n",
                (unsigned)stage, error_code);
    TIKU_PRINTF("TFT colorcycle: result=FAIL\n");
}

static void tft_color_fill(uint16_t color)
{
    size_t i;

    for (i = 0U; i < (size_t)(TFT_WIDTH * TFT_HEIGHT); ++i) {
        tft_color_framebuffer[i] = color;
    }
    tiku_stm32n6_dcache_clean(tft_color_framebuffer, TFT_FRAMEBUFFER_BYTES);
}

PT_THREAD(tiku_process_thread_stm32n6_dk_tft_colorcycle(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    tft_stage_t failed_stage = TFT_STAGE_CLOCK;
    int error_code = 0;
    tiku_ltdc_error_flags_t errors = 0U;
    tiku_ltdc_status_t status;
    uint8_t failed = 0U;

    TIKU_PROCESS_BEGIN();

    if (tft_configure(&failed_stage, &error_code) != 0) {
        tft_fail(failed_stage, error_code);
        failed = 1U;
    } else {
        tiku_cpu_stm32n6_delay_ms(100U);
        TIKU_BOARD_LCD_ENABLE();
        tiku_cpu_stm32n6_delay_ms(100U);
        TIKU_BOARD_LCD_BACKLIGHT_ON();
        tft_color_index = 0U;
        TIKU_PRINTF("TFT colorcycle: image=%s resolution=%ux%u\n",
                    tft_color_names[tft_color_index],
                    TFT_WIDTH, TFT_HEIGHT);
        tiku_timer_set_event(&tft_color_timer, TFT_COLOR_ROTATION_TICKS);
        tiku_cpu_stm32n6_delay_ms(1000U);
        status = tiku_ltdc_get_error_flags(&tft_ltdc, &errors);
        if (status != TIKU_LTDC_OK || errors != 0U) {
            failed_stage = TFT_STAGE_SCANOUT_ERRORS;
            error_code = status == TIKU_LTDC_OK ? (int)errors : (int)status;
            tft_fail(failed_stage, error_code);
            tiku_timer_stop(&tft_color_timer);
            failed = 1U;
        } else {
            TIKU_PRINTF("TFT colorcycle: result=PASS\n");
        }
    }

    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
        if (failed != 0U || ev != TIKU_EVENT_TIMER ||
            data != (tiku_event_data_t)&tft_color_timer) {
            continue;
        }

        tft_color_index = (uint8_t)((tft_color_index + 1U) % TFT_COLOR_COUNT);
        tft_color_fill(tft_colors[tft_color_index]);
        TIKU_PRINTF("TFT colorcycle: image=%s resolution=%ux%u\n",
                    tft_color_names[tft_color_index], TFT_WIDTH, TFT_HEIGHT);

        errors = 0U;
        status = tiku_ltdc_get_error_flags(&tft_ltdc, &errors);
        if (status != TIKU_LTDC_OK || errors != 0U) {
            failed_stage = TFT_STAGE_SCANOUT_ERRORS;
            error_code = status == TIKU_LTDC_OK ? (int)errors : (int)status;
            tft_fail(failed_stage, error_code);
            failed = 1U;
            continue;
        }
        tiku_timer_reset(&tft_color_timer);
    }

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&stm32n6_dk_tft_colorcycle);

#endif /* TIKU_STM32N6_DK_TFT_COLORCYCLE_TEST_ENABLE */
