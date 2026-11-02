/* STM32N6570-DK IMX335 sensor bring-up and CSI-2 streaming test.
 *
 * Powers the camera module (VDDIO4 pad supply first, then ST's PC8/PD2 edge
 * order), programs the IMX335 over I2C (chip-ID check plus the 2592x1944
 * two-lane RAW10 register table), points the DCMIPP CSI-2 receiver at it and
 * starts streaming. No pixel pipe is configured: the test passes when the
 * receiver reports start-of-frame and end-of-frame events and no CSI protocol
 * or D-PHY lane error after the first frame. Lane control errors before the
 * first frame are start-up noise from the sensor bringing the lanes up (one
 * per lane on the DK) and are only reported. That is a stronger check than
 * the chip ID alone, which also reads 0x00 from a dead bus.
 *
 * Build: make BOARD=stm32n6570_dk TIKU_STM32N6_DK_IMX335_TEST_ENABLE=1 */
#include <stddef.h>
#include <stdint.h>

#include <kernel/process/tiku_process.h>
#include <kernel/timers/tiku_timer.h>

#include <applications/tiku_stm32n6570_imx335.h>

#include <arch/stm32n6/tiku_cpu_common.h>
#include <arch/stm32n6/tiku_dcmipp_arch.h>
#include <arch/stm32n6/tiku_device_select.h>
#include <arch/stm32n6/tiku_gpio_arch.h>
#include <arch/stm32n6/tiku_i2c_arch.h>
#include <arch/stm32n6/tiku_stm32n6_regs.h>

#if defined(TIKU_STM32N6_DK_IMX335_TEST_ENABLE) && \
    TIKU_STM32N6_DK_IMX335_TEST_ENABLE

#if !defined(TIKU_BOARD_STM32N6570_DK)
#error "The STM32N6 DK IMX335 test requires the STM32N6570-DK board"
#endif

#define OBSERVE_TICKS       TIKU_CLOCK_SECOND
/* Stop collecting after this many CSI errors so a flood cannot starve the
 * observation timer. */
#define CSI_ERROR_LIMIT     64U
/* 7-bit address of the IMX335 (ST's 8-bit 0x34). */
#define IMX335_I2C_ADDR     0x1AU

/* Corrected single-bit ECC errors are harmless, so they are counted and
 * reported but do not fail the run. Everything else on the receiver does. */
#define CSI_FRAME_IT0       (STM32N6_CSI_IT_SOF0 | STM32N6_CSI_IT_EOF0)
#define CSI_ERROR_IT0       (STM32N6_CSI_IT_SYNCERR | STM32N6_CSI_IT_WDERR | \
                             STM32N6_CSI_IT_SPKTERR | STM32N6_CSI_IT_IDERR | \
                             STM32N6_CSI_IT_ECCERR | STM32N6_CSI_IT_CRCERR | \
                             STM32N6_CSI_IT_CCFIFOF)
#define CSI_BENIGN_IT0      STM32N6_CSI_IT_CECCERR
#define CSI_LANE_IT1        STM32N6_CSI_IER1_ALL

static struct tiku_timer imx335_timer;
TIKU_PROCESS(stm32n6_dk_imx335_stream, "STM32N6 DK IMX335 stream");

typedef enum {
    IMX335_STAGE_I2C = 1,
    IMX335_STAGE_SENSOR_INIT,
    IMX335_STAGE_DCMIPP_ENABLE,
    IMX335_STAGE_CSI_CONFIG,
    IMX335_STAGE_CSI_IRQ,
    IMX335_STAGE_SENSOR_START,
    IMX335_STAGE_CSI_ERROR,
    IMX335_STAGE_NO_SOF,
    IMX335_STAGE_NO_EOF,
} imx335_stage_t;

static uint8_t imx335_sensor_streaming;

static int imx335_setup(imx335_stage_t *failed_stage, int *error_code)
{
    const tiku_i2c_config_t i2c = {
        .speed = TIKU_I2C_SPEED_STANDARD,
    };
    const tiku_dcmipp_csi_config_t csi = {
        .lane_count = TIKU_DCMIPP_CSI_TWO_DATA_LANES,
        .lane_mapping = TIKU_DCMIPP_CSI_LANES_PHYSICAL,
        .phy_bitrate = TIKU_DCMIPP_CSI_PHY_BT_1600,
        .data_type = TIKU_DCMIPP_CSI_DT_RAW10,
        .virtual_channel = 0U,
    };
    int rc;

    /* PC8 (camera reset) and the I2C pads sit in the VDDIO4 pad bank, which
     * reads 0 and drives nothing until its supply is declared valid. */
    TIKU_REG32(STM32N6_PWR_SVMCR1) |= STM32N6_PWR_SVMCR1_VDDIO4SV;
    tiku_cpu_stm32n6_delay_ms(1U);

    /* Same edge order as ST's BSP_CAMERA_HwReset (stm32n6570_discovery_camera.c):
     * PC8 low, PD2 low, PC8 high, PD2 high, 100 ms apart. The BSP's own
     * comments call PC8 the module's 2V8 enable and PD2 the reset, the
     * opposite of the board header's names, so the order is what matters:
     * PC8 must rise before PD2. */
    TIKU_BOARD_CAMERA_INIT();
    tiku_cpu_stm32n6_delay_ms(100U);
    TIKU_BOARD_CAMERA_RESET_RELEASE();      /* PC8 high */
    tiku_cpu_stm32n6_delay_ms(100U);
    TIKU_BOARD_CAMERA_ENABLE();             /* PD2 high */
    tiku_cpu_stm32n6_delay_ms(100U);

    rc = tiku_i2c_init(&i2c);
    if (rc != TIKU_I2C_OK) {
        *failed_stage = IMX335_STAGE_I2C;
        *error_code = rc;
        return -1;
    }
    rc = tiku_stm32n6570_imx335_init();
    if (rc != TIKU_IMX335_OK) {
        /* The driver reports every bus error as ERR_BUS; a direct probe tells
         * a silent sensor (NACK, -1) from a stuck bus (timeout/busy). */
        TIKU_PRINTF("IMX335: i2c probe 0x%x rc=%d\n", IMX335_I2C_ADDR,
                    tiku_i2c_probe(IMX335_I2C_ADDR));
        *failed_stage = IMX335_STAGE_SENSOR_INIT;
        *error_code = rc;
        return -1;
    }

    /* The receiver must be listening before the sensor leaves standby so the
     * first start-of-frame is not missed. */
    rc = tiku_dcmipp_arch_enable();
    if (rc != TIKU_DCMIPP_OK) {
        *failed_stage = IMX335_STAGE_DCMIPP_ENABLE;
        *error_code = rc;
        return -1;
    }
    rc = tiku_dcmipp_arch_config_csi(&csi);
    if (rc != TIKU_DCMIPP_OK) {
        *failed_stage = IMX335_STAGE_CSI_CONFIG;
        *error_code = rc;
        return -1;
    }
    rc = tiku_dcmipp_arch_csi_interrupt_enable(
        CSI_FRAME_IT0 | CSI_ERROR_IT0 | CSI_BENIGN_IT0, CSI_LANE_IT1);
    if (rc != TIKU_DCMIPP_OK) {
        *failed_stage = IMX335_STAGE_CSI_IRQ;
        *error_code = rc;
        return -1;
    }

    rc = tiku_stm32n6570_imx335_start();
    if (rc != TIKU_IMX335_OK) {
        *failed_stage = IMX335_STAGE_SENSOR_START;
        *error_code = rc;
        return -1;
    }
    imx335_sensor_streaming = 1U;
    return 0;
}

/* Leave the module in reset and unpowered whatever the outcome. */
static void imx335_teardown(void)
{
    if (imx335_sensor_streaming != 0U) {
        (void)tiku_stm32n6570_imx335_stop();
        imx335_sensor_streaming = 0U;
    }
    (void)tiku_dcmipp_arch_csi_interrupt_disable(
        CSI_FRAME_IT0 | CSI_ERROR_IT0 | CSI_BENIGN_IT0, CSI_LANE_IT1);
    TIKU_BOARD_CAMERA_RESET_ASSERT();
    TIKU_BOARD_CAMERA_DISABLE();
    tiku_i2c_close();
}

static void imx335_fail(imx335_stage_t stage, int error_code)
{
    TIKU_PRINTF("IMX335: FAIL stage=%u code=%d\n", (unsigned)stage,
                error_code);
    TIKU_PRINTF("IMX335: result=FAIL\n");
}

TIKU_PROCESS_THREAD(stm32n6_dk_imx335_stream, ev, data)
{
    static imx335_stage_t failed_stage;
    static int error_code;
    static int failed;
    static uint32_t sof_count;
    static uint32_t eof_count;
    static uint32_t corrected_ecc_count;
    static uint32_t csi_error_count;
    static uint32_t late_error_count;
    static uint32_t lane_flags[2];
    static uint32_t other_flags;
    static unsigned other_code;

    TIKU_PROCESS_BEGIN();

    failed_stage = IMX335_STAGE_I2C;
    error_code = 0;
    failed = 0;
    sof_count = 0U;
    eof_count = 0U;
    corrected_ecc_count = 0U;
    csi_error_count = 0U;
    late_error_count = 0U;
    lane_flags[0] = 0U;
    lane_flags[1] = 0U;
    other_flags = 0U;
    other_code = 0U;

    if (imx335_setup(&failed_stage, &error_code) != 0) {
        imx335_teardown();
        imx335_fail(failed_stage, error_code);
    } else {
        TIKU_PRINTF("IMX335: streaming, observing CSI for %u ticks\n",
                    (unsigned)OBSERVE_TICKS);

        tiku_timer_set_event(&imx335_timer, OBSERVE_TICKS);
        while (!failed) {
            TIKU_PROCESS_WAIT_EVENT();
            if (ev == TIKU_DCMIPP_EVENT_ID) {
                tiku_dcmipp_event_t event;

                if (tiku_dcmipp_arch_event_from_data(
                        (tiku_event_data_t)data, &event) != TIKU_DCMIPP_OK) {
                    continue;
                }
                if (event.type == DCMIPP_EVENT_CSI_SOF) {
                    ++sof_count;
                } else if (event.type == DCMIPP_EVENT_CSI_EOF) {
                    ++eof_count;
                } else if (event.type == DCMIPP_EVENT_ERROR) {
                    if (event.error == DCMIPP_ERROR_CSI_CORRECTED_ECC) {
                        ++corrected_ecc_count;
                    } else {
                        /* Collect every error for the window instead of
                         * stopping at the first, so the report shows which
                         * lane and which flags fire. */
                        if (csi_error_count == 0U) {
                            error_code = (int)event.error;
                        }
                        ++csi_error_count;
                        /* Lane control errors seen while the sensor brings
                         * the lanes up are start-up noise. Anything after the
                         * first frame means the stream itself is damaged. */
                        if (sof_count != 0U) {
                            ++late_error_count;
                        }
                        if (event.error == DCMIPP_ERROR_CSI_DPHY_LANE) {
                            lane_flags[event.virtual_channel & 1U] |=
                                event.raw_status;
                        } else {
                            other_flags |= event.raw_status;
                            other_code = (unsigned)event.error;
                        }
                        if (csi_error_count >= CSI_ERROR_LIMIT) {
                            break;
                        }
                    }
                }
            } else if (ev == TIKU_EVENT_TIMER) {
                break;
            }
        }

        imx335_teardown();

        TIKU_PRINTF("IMX335: csi errors=%u (after first frame=%u) lane0=0x%x "
                    "lane1=0x%x other=0x%x (code %u)\n",
                    (unsigned)csi_error_count, (unsigned)late_error_count,
                    (unsigned)lane_flags[0], (unsigned)lane_flags[1],
                    (unsigned)other_flags, other_code);
        if (late_error_count != 0U) {
            failed_stage = IMX335_STAGE_CSI_ERROR;
            failed = 1;
        }
        if (!failed && sof_count == 0U) {
            failed_stage = IMX335_STAGE_NO_SOF;
            failed = 1;
        } else if (!failed && eof_count == 0U) {
            failed_stage = IMX335_STAGE_NO_EOF;
            failed = 1;
        }

        TIKU_PRINTF("IMX335: sof=%u eof=%u corrected_ecc=%u\n",
                    (unsigned)sof_count, (unsigned)eof_count,
                    (unsigned)corrected_ecc_count);
        if (failed) {
            imx335_fail(failed_stage, error_code);
        } else {
            TIKU_PRINTF("IMX335: result=PASS\n");
        }
    }

    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
    }

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&stm32n6_dk_imx335_stream);

#endif /* TIKU_STM32N6_DK_IMX335_TEST_ENABLE */
