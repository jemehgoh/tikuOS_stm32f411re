/* STM32N6570-DK DCMIPP CSI-2 -> 192x192 pipeline configuration test.
 *
 * Configures the CSI-2 receiver, Pipe1 ISP demosaic and coupled Pipe2 (centre
 * 1536x1536 crop, 8x downsize to 192x192 RGB565), then arms a snapshot. The
 * IMX335 is deliberately not started, so no frame is expected: the test passes
 * when every configuration call succeeds, the capture arms cleanly, and no
 * DCMIPP error event or frame arrives during the observation window.
 *
 * Build: make BOARD=stm32n6570_dk TIKU_STM32N6_DK_DCMIPP_CSI192_TEST_ENABLE=1 */
#include <stddef.h>
#include <stdint.h>

#include <kernel/process/tiku_process.h>
#include <kernel/timers/tiku_timer.h>

#include <arch/stm32n6/tiku_cache_arch.h>
#include <arch/stm32n6/tiku_dcmipp_arch.h>
#include <arch/stm32n6/tiku_device_select.h>
#include <arch/stm32n6/tiku_stm32n6_regs.h>

#if defined(TIKU_STM32N6_DK_DCMIPP_CSI192_TEST_ENABLE) && \
    TIKU_STM32N6_DK_DCMIPP_CSI192_TEST_ENABLE

#if !defined(TIKU_BOARD_STM32N6570_DK)
#error "The STM32N6 DK DCMIPP CSI192 test requires the STM32N6570-DK board"
#endif

/* IMX335 profile is 2592x1944. The largest centre crop whose downsize to 192
 * stays within the Pipe2 limit of 8x (ratio 8192..65535) is 1536x1536. */
#define SENSOR_WIDTH        2592U
#define SENSOR_HEIGHT       1944U
#define CROP_SIZE           1536U
#define CROP_X              ((SENSOR_WIDTH - CROP_SIZE) / 2U)
#define CROP_Y              ((SENSOR_HEIGHT - CROP_SIZE) / 2U)
#define OUT_WIDTH           192U
#define OUT_HEIGHT          192U
#define OUT_BYTES_PER_PIXEL 2U /* RGB565 */
#define OUT_PITCH           (OUT_WIDTH * OUT_BYTES_PER_PIXEL)
#define OUT_BYTES           (OUT_PITCH * OUT_HEIGHT)
/* ratio = ceil(8192 * (src - 1) / dst), divider = ceil(1024 * dst / src). */
#define DOWNSIZE_RATIO      ((8192U * (CROP_SIZE - 1U) + OUT_WIDTH - 1U) / \
                             OUT_WIDTH)
#define DOWNSIZE_DIV        ((1024U * OUT_WIDTH + CROP_SIZE - 1U) / CROP_SIZE)

#define OBSERVE_TICKS       (TIKU_CLOCK_SECOND / 2U)

static uint8_t csi192_buffer[OUT_BYTES]
    __attribute__((section(".axisram"), aligned(64)));

static struct tiku_timer csi192_timer;
TIKU_PROCESS(stm32n6_dk_dcmipp_csi192, "STM32N6 DK DCMIPP CSI192");

typedef enum {
    CSI192_STAGE_ENABLE = 1,
    CSI192_STAGE_CSI,
    CSI192_STAGE_PIPE_DIFF,
    CSI192_STAGE_AXI_PLUG,
    CSI192_STAGE_ISP_SETUP,
    CSI192_STAGE_PIPE1_CONFIG,
    CSI192_STAGE_PIPE2_POST,
    CSI192_STAGE_PIPE2_CONFIG,
    CSI192_STAGE_PIPE2_CROP,
    CSI192_STAGE_ISP_ENABLE,
    CSI192_STAGE_PIPE1_ENABLE,
    CSI192_STAGE_POST_ENABLE,
    CSI192_STAGE_INTERRUPTS,
    CSI192_STAGE_PIPE2_ENABLE,
    CSI192_STAGE_INPUT_ENABLE,
    CSI192_STAGE_START_CAPTURE,
    CSI192_STAGE_ERROR_EVENT,
    CSI192_STAGE_UNEXPECTED_FRAME,
} csi192_stage_t;

#define CSI192_STEP(stage_id, call)                  \
    do {                                             \
        int rc_ = (call);                            \
        if (rc_ != TIKU_DCMIPP_OK) {                 \
            *failed_stage = (stage_id);              \
            *error_code = rc_;                       \
            return -1;                               \
        }                                            \
    } while (0)

static int csi192_configure(csi192_stage_t *failed_stage, int *error_code)
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
    };
    const tiku_dcmipp_pipe_config_t pipe2 = {
        .frame_rate = 0U,
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
    };
    const tiku_dcmipp_isp_config_t isp = {
        .demosaic = 1U, /* RGGB, linear interpolation. */
    };
    const tiku_dcmipp_postproc_config_t pipe2_post = {
        .crop_start = CROP_X | (CROP_Y << 16),
        .crop_size = CROP_SIZE | (CROP_SIZE << 16),
        .downsize_control = DOWNSIZE_DIV | (DOWNSIZE_DIV << 16),
        .downsize_ratio = DOWNSIZE_RATIO | (DOWNSIZE_RATIO << 16),
        .downsize_destination_size = OUT_WIDTH | (OUT_HEIGHT << 16),
        .pixel_packer = TIKU_DCMIPP_PIXEL_PACKER_RGB565,
        .pitch = {OUT_PITCH, 0U, 0U},
    };
    const tiku_dcmipp_plane_address_t output = {
        .plane = {(uintptr_t)csi192_buffer, 0U, 0U},
        .count = 1U,
    };

    /* Drop any stale lines so they cannot be evicted over DCMIPP writes. */
    tiku_stm32n6_dcache_invalidate(csi192_buffer, sizeof(csi192_buffer));

    CSI192_STEP(CSI192_STAGE_ENABLE, tiku_dcmipp_arch_enable());
    CSI192_STEP(CSI192_STAGE_CSI, tiku_dcmipp_arch_config_csi(&csi));
    CSI192_STEP(CSI192_STAGE_PIPE_DIFF,
                tiku_dcmipp_arch_set_pipe_diff(DCMIPP_PIPEDIFF_COUPLED));
    CSI192_STEP(CSI192_STAGE_AXI_PLUG,
                tiku_dcmipp_arch_axi_plug_configure_pipe2());
    CSI192_STEP(CSI192_STAGE_ISP_SETUP, tiku_dcmipp_arch_isp_setup(&isp));
    CSI192_STEP(CSI192_STAGE_PIPE1_CONFIG,
                tiku_dcmipp_arch_pipe_configure(PIPE1, &pipe1));
    CSI192_STEP(CSI192_STAGE_PIPE2_POST,
                tiku_dcmipp_arch_postprocessing_configure(PIPE2,
                                                          &pipe2_post));
    CSI192_STEP(CSI192_STAGE_PIPE2_CONFIG,
                tiku_dcmipp_arch_pipe_configure(PIPE2, &pipe2));
    CSI192_STEP(CSI192_STAGE_PIPE2_CROP,
                tiku_dcmipp_arch_pipe_crop_rect(PIPE2, CROP_X, CROP_Y,
                                                CROP_SIZE, CROP_SIZE));
    CSI192_STEP(CSI192_STAGE_PIPE1_ENABLE,
                tiku_dcmipp_arch_pipe_enable(PIPE1));
    CSI192_STEP(CSI192_STAGE_ISP_ENABLE, tiku_dcmipp_arch_isp_enable());
    CSI192_STEP(CSI192_STAGE_POST_ENABLE,
                tiku_dcmipp_arch_postprocessing_enable(PIPE2));
    CSI192_STEP(CSI192_STAGE_INTERRUPTS,
                tiku_dcmipp_arch_pipe_interrupt_enable(
                    PIPE2, STM32N6_DCMIPP_IT_PIPE2_FRAME |
                               STM32N6_DCMIPP_IT_PIPE2_OVR |
                               STM32N6_DCMIPP_IT_AXI_TRANSFER_ERR));
    CSI192_STEP(CSI192_STAGE_PIPE2_ENABLE,
                tiku_dcmipp_arch_pipe_enable(PIPE2));
    CSI192_STEP(CSI192_STAGE_INPUT_ENABLE, tiku_dcmipp_arch_enable_input());
    CSI192_STEP(CSI192_STAGE_START_CAPTURE,
                tiku_dcmipp_arch_pipe_start_capture(
                    PIPE2, &output, 0U, DCMIPP_CAPTURE_SNAPSHOT));
    return 0;
}

static void csi192_fail(csi192_stage_t stage, int error_code)
{
    TIKU_PRINTF("CSI192: FAIL stage=%u code=%d\n", (unsigned)stage,
                error_code);
    TIKU_PRINTF("CSI192: result=FAIL\n");
}

TIKU_PROCESS_THREAD(stm32n6_dk_dcmipp_csi192, ev, data)
{
    static csi192_stage_t failed_stage;
    static int error_code;
    static int failed;

    TIKU_PROCESS_BEGIN();

    failed_stage = CSI192_STAGE_ENABLE;
    error_code = 0;
    failed = 0;

    if (csi192_configure(&failed_stage, &error_code) != 0) {
        csi192_fail(failed_stage, error_code);
    } else {
        static uint32_t frames;

        frames = 0U;
        TIKU_PRINTF("CSI192: configured crop=%ux%u out=%ux%u RGB565 "
                    "ratio=%u div=%u\n",
                    CROP_SIZE, CROP_SIZE, OUT_WIDTH, OUT_HEIGHT,
                    (unsigned)DOWNSIZE_RATIO, (unsigned)DOWNSIZE_DIV);

        /* No sensor is streaming: watch for error events until the timer
         * fires. A frame or an error here means the pipeline is misconfigured
         * or the CSI input is picking up noise. */
        tiku_timer_set_event(&csi192_timer, OBSERVE_TICKS);
        while (!failed) {
            TIKU_PROCESS_WAIT_EVENT();
            if (ev == TIKU_DCMIPP_EVENT_ID) {
                tiku_dcmipp_event_t event;

                if (tiku_dcmipp_arch_event_from_data(
                        (tiku_event_data_t)data, &event) == TIKU_DCMIPP_OK &&
                    event.type == DCMIPP_EVENT_ERROR) {
                    failed_stage = CSI192_STAGE_ERROR_EVENT;
                    error_code = (int)event.error;
                    failed = 1;
                }
            } else if (ev == TIKU_EVENT_TIMER) {
                break;
            }
        }

        if (!failed && tiku_dcmipp_arch_get_frame_count(PIPE2, &frames) ==
                           TIKU_DCMIPP_OK && frames != 0U) {
            failed_stage = CSI192_STAGE_UNEXPECTED_FRAME;
            error_code = (int)frames;
            failed = 1;
        }

        if (failed) {
            csi192_fail(failed_stage, error_code);
        } else {
            TIKU_PRINTF("CSI192: armed, no frames, no errors\n");
            TIKU_PRINTF("CSI192: result=PASS\n");
        }
    }

    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
    }

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&stm32n6_dk_dcmipp_csi192);

#endif /* TIKU_STM32N6_DK_DCMIPP_CSI192_TEST_ENABLE */
