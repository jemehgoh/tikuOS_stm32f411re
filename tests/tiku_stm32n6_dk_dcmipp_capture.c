/* STM32N6570-DK full-pipeline capture test: IMX335 -> CSI-2 -> 192x192.
 *
 * Powers the camera module (VDDIO4 pad supply first, then ST's PC8/PD2 edge
 * order), programs the IMX335 over I2C, configures the CSI-2 receiver and the
 * Pipe1 ISP (demosaic) with its pixel pipe (centre 1536x1536 crop, 8x downsize
 * to 192x192 RGB565), starts the sensor, lets it stream briefly and then arms
 * a one-shot snapshot on Pipe1. It waits for the frame-complete event and
 * checks the captured pixels. A statistics line and a small contrast-stretched
 * ASCII preview are printed so the image can be judged from the serial log.
 *
 * The buffer is pre-filled with a pattern the pipe cannot produce, so a frame
 * the DCMIPP reports but never wrote shows up as unwritten pixels. The test
 * fails on unwritten pixels, a perfectly constant frame, a bus or overrun
 * error, or a timeout. A merely dark frame passes with a warning: the sensor
 * has no auto-exposure here.
 *
 * Pixels reach memory only because the DCMIPP peripheral is configured secure
 * and privileged (see tiku_dcmipp_master_attr_configure); without it RISAF
 * drops the writes and the frame completes with the buffer untouched.
 *
 * The capture runs on Pipe1 like ST's DCMIPP examples. The coupled Pipe2 path
 * (ISP output feeding the ancillary pipe) was never verified.
 *
 * Lane control errors while the sensor brings its lanes up are counted and
 * reported but are not fatal: the capture itself decides the result.
 *
 * Build: make BOARD=stm32n6570_dk TIKU_STM32N6_DK_DCMIPP_CAPTURE_TEST_ENABLE=1 */
#include <stddef.h>
#include <stdint.h>

#include <kernel/process/tiku_process.h>
#include <kernel/timers/tiku_timer.h>

#include <applications/tiku_stm32n6570_imx335.h>

#include <arch/stm32n6/tiku_cache_arch.h>
#include <arch/stm32n6/tiku_cpu_common.h>
#include <arch/stm32n6/tiku_dcmipp_arch.h>
#include <arch/stm32n6/tiku_device_select.h>
#include <arch/stm32n6/tiku_gpio_arch.h>
#include <arch/stm32n6/tiku_i2c_arch.h>
#include <arch/stm32n6/tiku_stm32n6_regs.h>

#if defined(TIKU_STM32N6_DK_DCMIPP_CAPTURE_TEST_ENABLE) && \
    TIKU_STM32N6_DK_DCMIPP_CAPTURE_TEST_ENABLE

#if !defined(TIKU_BOARD_STM32N6570_DK)
#error "The STM32N6 DK DCMIPP capture test requires the STM32N6570-DK board"
#endif

/* IMX335 profile is 2592x1944. The largest centre crop whose downsize to 192
 * stays within the pipe's limit of 8x (ratio 8192..65535) is 1536x1536. */
#define SENSOR_WIDTH        2592U
#define SENSOR_HEIGHT       1944U
#define CROP_SIZE           1536U
#define CROP_X              ((SENSOR_WIDTH - CROP_SIZE) / 2U)
#define CROP_Y              ((SENSOR_HEIGHT - CROP_SIZE) / 2U)
#define OUT_WIDTH           192U
#define OUT_HEIGHT          192U
#define OUT_PIXELS          (OUT_WIDTH * OUT_HEIGHT)
#define OUT_PITCH           (OUT_WIDTH * 2U) /* RGB565 */
#define OUT_BYTES           (OUT_PITCH * OUT_HEIGHT)
/* ratio = ceil(8192 * (src - 1) / dst), divider = ceil(1024 * dst / src). */
#define DOWNSIZE_RATIO      ((8192U * (CROP_SIZE - 1U) + OUT_WIDTH - 1U) / \
                             OUT_WIDTH)
#define DOWNSIZE_DIV        ((1024U * OUT_WIDTH + CROP_SIZE - 1U) / CROP_SIZE)

#define CAPTURE_TIMEOUT_TICKS   (3U * TIKU_CLOCK_SECOND)
/* Streaming time before the snapshot is armed. */
#define CAPTURE_SETTLE_TICKS    (TIKU_CLOCK_SECOND / 2U)
/* Below this green spread the frame is reported as dark (a warning, not a
 * failure): the sensor has no auto-exposure here, so a dim scene is nearly
 * black. A perfectly constant frame, or any pixel left unwritten, fails. */
#define DARK_PIXEL_SPREAD       8U
/* Written to the buffer before the capture; seeing it afterwards means the
 * pipe never wrote that pixel. */
#define CAPTURE_FILL_PATTERN    0xA5A5U
#define PREVIEW_COLS            48U
#define PREVIEW_ROWS            24U

/* Corrected single-bit ECC errors are harmless; the other CSI errors are
 * counted and reported. */
#define CSI_FRAME_IT0       (STM32N6_CSI_IT_SOF0 | STM32N6_CSI_IT_EOF0)
#define CSI_ERROR_IT0       (STM32N6_CSI_IT_SYNCERR | STM32N6_CSI_IT_WDERR | \
                             STM32N6_CSI_IT_SPKTERR | STM32N6_CSI_IT_IDERR | \
                             STM32N6_CSI_IT_ECCERR | STM32N6_CSI_IT_CRCERR | \
                             STM32N6_CSI_IT_CCFIFOF)
#define CSI_BENIGN_IT0      STM32N6_CSI_IT_CECCERR
#define CSI_LANE_IT1        STM32N6_CSI_IER1_ALL
#define CSI_ALL_IT0         (CSI_FRAME_IT0 | CSI_ERROR_IT0 | CSI_BENIGN_IT0)

static uint16_t capture_buffer[OUT_PIXELS]
    __attribute__((section(".axisram"), aligned(64)));

static struct tiku_timer capture_timer;
TIKU_PROCESS(stm32n6_dk_dcmipp_capture, "STM32N6 DK DCMIPP capture");

typedef enum {
    CAPTURE_STAGE_I2C = 1,
    CAPTURE_STAGE_SENSOR_INIT,
    CAPTURE_STAGE_ENABLE,
    CAPTURE_STAGE_CSI,
    CAPTURE_STAGE_CSI_IRQ,
    CAPTURE_STAGE_ISP_SETUP,
    CAPTURE_STAGE_PIPE_CONFIG,
    CAPTURE_STAGE_POST_CONFIG,
    CAPTURE_STAGE_PIPE_ENABLE,
    CAPTURE_STAGE_ISP_ENABLE,
    CAPTURE_STAGE_POST_ENABLE,
    CAPTURE_STAGE_INTERRUPTS,
    CAPTURE_STAGE_INPUT_ENABLE,
    CAPTURE_STAGE_START_CAPTURE,
    CAPTURE_STAGE_SENSOR_START,
    CAPTURE_STAGE_TIMEOUT,
    CAPTURE_STAGE_PIPE_ERROR,
    CAPTURE_STAGE_FLAT_FRAME,
    CAPTURE_STAGE_NOT_WRITTEN,
} capture_stage_t;

static uint8_t capture_sensor_streaming;

#define CAPTURE_STEP(stage_id, call)                 \
    do {                                             \
        int rc_ = (call);                            \
        if (rc_ != TIKU_DCMIPP_OK) {                 \
            *failed_stage = (stage_id);              \
            *error_code = rc_;                       \
            return -1;                               \
        }                                            \
    } while (0)

/* Power the module and program the sensor, leaving it in standby. */
static int capture_sensor_init(capture_stage_t *failed_stage, int *error_code)
{
    const tiku_i2c_config_t i2c = {
        .speed = TIKU_I2C_SPEED_STANDARD,
    };
    int rc;

    /* PC8 (camera reset) and the I2C pads sit in the VDDIO4 pad bank, which
     * reads 0 and drives nothing until its supply is declared valid. */
    TIKU_REG32(STM32N6_PWR_SVMCR1) |= STM32N6_PWR_SVMCR1_VDDIO4SV;
    tiku_cpu_stm32n6_delay_ms(1U);

    /* Same edge order as ST's BSP_CAMERA_HwReset: PC8 low, PD2 low, PC8 high,
     * PD2 high, 100 ms apart. PC8 must rise before PD2. */
    TIKU_BOARD_CAMERA_INIT();
    tiku_cpu_stm32n6_delay_ms(100U);
    TIKU_BOARD_CAMERA_RESET_RELEASE();      /* PC8 high */
    tiku_cpu_stm32n6_delay_ms(100U);
    TIKU_BOARD_CAMERA_ENABLE();             /* PD2 high */
    tiku_cpu_stm32n6_delay_ms(100U);

    rc = tiku_i2c_init(&i2c);
    if (rc != TIKU_I2C_OK) {
        *failed_stage = CAPTURE_STAGE_I2C;
        *error_code = rc;
        return -1;
    }
    rc = tiku_stm32n6570_imx335_init();
    if (rc != TIKU_IMX335_OK) {
        *failed_stage = CAPTURE_STAGE_SENSOR_INIT;
        *error_code = rc;
        return -1;
    }
    return 0;
}

/* Configure the receiver and the Pipe1 ISP + pixel pipe (crop, downsize,
 * RGB565 packer) and start the sensor streaming. The snapshot is armed later
 * (see capture_arm). */
static int capture_pipeline_start(capture_stage_t *failed_stage,
                                  int *error_code)
{
    const tiku_dcmipp_csi_config_t csi = {
        .lane_count = TIKU_DCMIPP_CSI_TWO_DATA_LANES,
        .lane_mapping = TIKU_DCMIPP_CSI_LANES_PHYSICAL,
        .phy_bitrate = TIKU_DCMIPP_CSI_PHY_BT_1600,
        .data_type = TIKU_DCMIPP_CSI_DT_RAW10,
        .virtual_channel = 0U,
    };
    const tiku_dcmipp_pipe_config_t pipe1 = {
        .DataTypeMode = 0U,
        .DataTypeIDA = 0x2BU,
        .DataTypeIDB = 0x2BU,
        .frame_rate = 0U,
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
    };
    const tiku_dcmipp_isp_config_t isp = {
        .demosaic = 1U, /* RGGB, linear interpolation. */
    };
    const tiku_dcmipp_postproc_config_t pipe1_post = {
        .crop_start = CROP_X | (CROP_Y << 16),
        .crop_size = CROP_SIZE | (CROP_SIZE << 16),
        .downsize_control = DOWNSIZE_DIV | (DOWNSIZE_DIV << 16),
        .downsize_ratio = DOWNSIZE_RATIO | (DOWNSIZE_RATIO << 16),
        .downsize_destination_size = OUT_WIDTH | (OUT_HEIGHT << 16),
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
        .pitch = {OUT_PITCH, 0U, 0U},
    };
    uint32_t i;
    int rc;

    /* Fill the buffer with a pattern the pipe cannot produce, push it to
     * memory, then drop the cache lines. After the capture any pixel still
     * holding the pattern was never written. */
    for (i = 0U; i < OUT_PIXELS; ++i) {
        capture_buffer[i] = CAPTURE_FILL_PATTERN;
    }
    tiku_stm32n6_dcache_clean(capture_buffer, OUT_BYTES);
    tiku_stm32n6_dcache_invalidate(capture_buffer, OUT_BYTES);

    CAPTURE_STEP(CAPTURE_STAGE_ENABLE, tiku_dcmipp_arch_enable());
    CAPTURE_STEP(CAPTURE_STAGE_CSI, tiku_dcmipp_arch_config_csi(&csi));
    CAPTURE_STEP(CAPTURE_STAGE_CSI_IRQ,
                 tiku_dcmipp_arch_csi_interrupt_enable(CSI_ALL_IT0,
                                                       CSI_LANE_IT1));
    CAPTURE_STEP(CAPTURE_STAGE_ISP_SETUP, tiku_dcmipp_arch_isp_setup(&isp));
    CAPTURE_STEP(CAPTURE_STAGE_PIPE_CONFIG,
                 tiku_dcmipp_arch_pipe_configure(PIPE1, &pipe1));
    CAPTURE_STEP(CAPTURE_STAGE_POST_CONFIG,
                 tiku_dcmipp_arch_postprocessing_configure(PIPE1,
                                                           &pipe1_post));
    CAPTURE_STEP(CAPTURE_STAGE_PIPE_ENABLE,
                 tiku_dcmipp_arch_pipe_enable(PIPE1));
    CAPTURE_STEP(CAPTURE_STAGE_ISP_ENABLE, tiku_dcmipp_arch_isp_enable());
    CAPTURE_STEP(CAPTURE_STAGE_POST_ENABLE,
                 tiku_dcmipp_arch_postprocessing_enable(PIPE1));
    CAPTURE_STEP(CAPTURE_STAGE_INTERRUPTS,
                 tiku_dcmipp_arch_pipe_interrupt_enable(
                     PIPE1, STM32N6_DCMIPP_IT_PIPE1_FRAME |
                                STM32N6_DCMIPP_IT_PIPE1_OVR |
                                STM32N6_DCMIPP_IT_AXI_TRANSFER_ERR));
    CAPTURE_STEP(CAPTURE_STAGE_INPUT_ENABLE, tiku_dcmipp_arch_enable_input());

    /* Start streaming now, arm later: the first frames after a start are often
     * unexposed or blank, so the snapshot waits until the stream has settled.
     * ST's examples also start the snapshot with the sensor already
     * streaming. */
    rc = tiku_stm32n6570_imx335_start();
    if (rc != TIKU_IMX335_OK) {
        *failed_stage = CAPTURE_STAGE_SENSOR_START;
        *error_code = rc;
        return -1;
    }
    capture_sensor_streaming = 1U;
    return 0;
}

/* Arm the one-shot snapshot on Pipe1; the next whole frame is captured. */
static int capture_arm(capture_stage_t *failed_stage, int *error_code)
{
    const tiku_dcmipp_plane_address_t output = {
        .plane = {(uintptr_t)capture_buffer, 0U, 0U},
        .count = 1U,
    };

    CAPTURE_STEP(CAPTURE_STAGE_START_CAPTURE,
                 tiku_dcmipp_arch_pipe_start_capture(
                     PIPE1, &output, 0U, DCMIPP_CAPTURE_SNAPSHOT));
    return 0;
}

/* Leave the sensor stopped and the module in reset and unpowered. */
static void capture_teardown(void)
{
    if (capture_sensor_streaming != 0U) {
        (void)tiku_stm32n6570_imx335_stop();
        capture_sensor_streaming = 0U;
    }
    (void)tiku_dcmipp_arch_csi_interrupt_disable(CSI_ALL_IT0, CSI_LANE_IT1);
    TIKU_BOARD_CAMERA_RESET_ASSERT();
    TIKU_BOARD_CAMERA_DISABLE();
    tiku_i2c_close();
}

/* Green is the only 6-bit channel of RGB565 and carries most of the luma. */
static unsigned capture_green(uint16_t pixel)
{
    return (unsigned)((pixel >> 5) & 0x3FU) << 2;
}

/* Green range of the last captured frame, so the preview can stretch its
 * contrast: a dark scene spans only a small part of 0..255. */
static unsigned capture_green_min;
static unsigned capture_green_max;
/* Pixels still holding the fill pattern, i.e. never written by the pipe. */
static uint32_t capture_unwritten;

/* Print min/max/mean of the green channel and a checksum. Returns the spread. */
static unsigned capture_stats(void)
{
    unsigned min = 255U;
    unsigned max = 0U;
    unsigned long long sum = 0ULL;
    uint32_t checksum = 2166136261UL;
    uint32_t unwritten = 0U;
    uint32_t i;

    for (i = 0U; i < OUT_PIXELS; ++i) {
        uint16_t pixel = capture_buffer[i];
        unsigned g = capture_green(pixel);

        if (pixel == CAPTURE_FILL_PATTERN) {
            ++unwritten;
        }
        if (g < min) {
            min = g;
        }
        if (g > max) {
            max = g;
        }
        sum += g;
        checksum = (checksum ^ pixel) * 16777619UL;
    }
    capture_green_min = min;
    capture_green_max = max;
    capture_unwritten = unwritten;
    TIKU_PRINTF("CAPTURE: green min=%u max=%u mean=%u unwritten_pixels=%u "
                "checksum=0x%x\n", min, max, (unsigned)(sum / OUT_PIXELS),
                (unsigned)unwritten, (unsigned)checksum);
    return max - min;
}

/* ASCII picture, one character per (192/48)x(192/24) block of pixels. */
static void capture_preview(void)
{
    static const char ramp[] = " .:-=+*#%@";
    const uint32_t block_w = OUT_WIDTH / PREVIEW_COLS;
    const uint32_t block_h = OUT_HEIGHT / PREVIEW_ROWS;
    const unsigned range = capture_green_max > capture_green_min ?
                           capture_green_max - capture_green_min : 1U;
    uint32_t row;
    uint32_t col;

    for (row = 0U; row < PREVIEW_ROWS; ++row) {
        char line[PREVIEW_COLS + 1U];

        for (col = 0U; col < PREVIEW_COLS; ++col) {
            unsigned sum = 0U;
            uint32_t x;
            uint32_t y;

            for (y = 0U; y < block_h; ++y) {
                for (x = 0U; x < block_w; ++x) {
                    sum += capture_green(
                        capture_buffer[(row * block_h + y) * OUT_WIDTH +
                                       col * block_w + x]);
                }
            }
            sum /= block_w * block_h;
            /* Stretch the frame's own min..max over the whole ramp. */
            if (sum < capture_green_min) {
                sum = capture_green_min;
            }
            line[col] = ramp[((sum - capture_green_min) *
                              (sizeof(ramp) - 2U)) / range];
        }
        line[PREVIEW_COLS] = '\0';
        TIKU_PRINTF("CAPTURE: |%s|\n", line);
    }
}

/* One line on failure: whether Pipe1 ever counted a frame and consumed the
 * capture request (FCTCR bit 3 = CPTREQ), and its status flags. */
static void capture_diag_pipe1(void)
{
    uint32_t frames = 0U;

    (void)tiku_dcmipp_arch_get_frame_count(PIPE1, &frames);
    TIKU_PRINTF("CAPTURE: pipe1 frames=%u FCTCR=0x%x SR=0x%x CMSR2=0x%x\n",
                (unsigned)frames,
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1FCTCR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_P1SR),
                (unsigned)TIKU_REG32(STM32N6_DCMIPP_CMSR2));
}

/* Account for one DCMIPP event. Returns 1 when Pipe1 completed a frame, -1 on a
 * fatal bus or overrun error (its code goes to *code), otherwise 0. Lane
 * control and corrected-ECC errors are counted but never fatal. */
static int capture_event(tiku_event_data_t data, uint32_t *sof,
                         uint32_t *lane_errors, uint32_t *other_errors,
                         int *code)
{
    tiku_dcmipp_event_t event;

    if (tiku_dcmipp_arch_event_from_data(data, &event) != TIKU_DCMIPP_OK) {
        return 0;
    }
    if (event.type == DCMIPP_EVENT_FRAME_COMPLETE && event.pipe == PIPE1) {
        return 1;
    }
    if (event.type == DCMIPP_EVENT_CSI_SOF) {
        ++*sof;
    } else if (event.type == DCMIPP_EVENT_ERROR) {
        if (event.error == DCMIPP_ERROR_CSI_DPHY_LANE ||
            event.error == DCMIPP_ERROR_CSI_CORRECTED_ECC) {
            ++*lane_errors;
        } else if (event.error == DCMIPP_ERROR_AXI_TRANSFER ||
                   event.error == DCMIPP_ERROR_PIPE_OVERRUN) {
            *code = (int)event.error;
            return -1;
        } else {
            /* Typically one CSI sync error from arming mid-stream. */
            ++*other_errors;
        }
    }
    return 0;
}

static void capture_fail(capture_stage_t stage, int error_code)
{
    TIKU_PRINTF("CAPTURE: FAIL stage=%u code=%d\n", (unsigned)stage,
                error_code);
    TIKU_PRINTF("CAPTURE: result=FAIL\n");
}

TIKU_PROCESS_THREAD(stm32n6_dk_dcmipp_capture, ev, data)
{
    static capture_stage_t failed_stage;
    static int error_code;
    static int failed;
    static int captured;
    static int settled;
    static uint32_t sof_count;
    static uint32_t lane_errors;
    static uint32_t other_errors;

    TIKU_PROCESS_BEGIN();

    failed_stage = CAPTURE_STAGE_I2C;
    error_code = 0;
    failed = 0;
    captured = 0;
    sof_count = 0U;
    lane_errors = 0U;
    other_errors = 0U;

    if (capture_sensor_init(&failed_stage, &error_code) != 0 ||
        capture_pipeline_start(&failed_stage, &error_code) != 0) {
        capture_teardown();
        capture_fail(failed_stage, error_code);
    } else {
        /* The build stamp shows which image is running (an SWD reset boots
         * the previous one first). Let the stream settle with the events being
         * consumed, then arm. */
        TIKU_PRINTF("CAPTURE: build %s %s, sensor streaming, settling\n",
                    __DATE__, __TIME__);
        tiku_timer_set_event(&capture_timer, CAPTURE_SETTLE_TICKS);
        settled = 0;
        while (!settled) {
            TIKU_PROCESS_WAIT_EVENT();
            if (ev == TIKU_DCMIPP_EVENT_ID) {
                (void)capture_event((tiku_event_data_t)data, &sof_count,
                                    &lane_errors, &other_errors, &error_code);
            } else if (ev == TIKU_EVENT_TIMER) {
                settled = 1;
            }
        }
        sof_count = 0U;

        if (capture_arm(&failed_stage, &error_code) != 0) {
            failed = 1;
        } else {
            TIKU_PRINTF("CAPTURE: snapshot armed on pipe1, waiting for a "
                        "%ux%u frame\n", OUT_WIDTH, OUT_HEIGHT);
            tiku_timer_set_event(&capture_timer, CAPTURE_TIMEOUT_TICKS);
        }
        while (!captured && !failed) {
            TIKU_PROCESS_WAIT_EVENT();
            if (ev == TIKU_DCMIPP_EVENT_ID) {
                int result = capture_event((tiku_event_data_t)data,
                                           &sof_count, &lane_errors,
                                           &other_errors, &error_code);

                if (result > 0) {
                    captured = 1;
                } else if (result < 0) {
                    failed_stage = CAPTURE_STAGE_PIPE_ERROR;
                    failed = 1;
                }
            } else if (ev == TIKU_EVENT_TIMER) {
                failed_stage = CAPTURE_STAGE_TIMEOUT;
                error_code = (int)sof_count;
                failed = 1;
            }
        }

        if (captured) {
            unsigned spread;

            /* Give any write still in flight time to land, then drop the cache
             * lines: DCMIPP wrote the buffer behind the cache's back. */
            tiku_cpu_stm32n6_delay_ms(100U);
            tiku_stm32n6_dcache_invalidate(capture_buffer, OUT_BYTES);
            TIKU_PRINTF("CAPTURE: frame complete, sof=%u lane_errors=%u "
                        "other_errors=%u\n", (unsigned)sof_count,
                        (unsigned)lane_errors, (unsigned)other_errors);

            spread = capture_stats();
            capture_preview();
            if (capture_unwritten != 0U) {
                /* The pipe reported a frame but left pixels untouched. */
                failed_stage = CAPTURE_STAGE_NOT_WRITTEN;
                error_code = (int)capture_unwritten;
                failed = 1;
            } else if (spread == 0U) {
                failed_stage = CAPTURE_STAGE_FLAT_FRAME;
                error_code = 0;
                failed = 1;
            } else if (spread < DARK_PIXEL_SPREAD) {
                TIKU_PRINTF("CAPTURE: warning: dark frame (green spread %u); "
                            "point the camera at a lit scene\n", spread);
            }
        }
        if (failed) {
            capture_diag_pipe1();
        }
        capture_teardown();

        if (failed) {
            if (failed_stage == CAPTURE_STAGE_TIMEOUT) {
                TIKU_PRINTF("CAPTURE: no frame in time, sof=%u "
                            "lane_errors=%u other_errors=%u\n",
                            (unsigned)sof_count, (unsigned)lane_errors,
                            (unsigned)other_errors);
            }
            capture_fail(failed_stage, error_code);
        } else {
            TIKU_PRINTF("CAPTURE: result=PASS\n");
        }
    }

    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
    }

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&stm32n6_dk_dcmipp_capture);

#endif /* TIKU_STM32N6_DK_DCMIPP_CAPTURE_TEST_ENABLE */
