/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Author: Jeremy Goh
 *
 * tiku_dcmipp_arch.h - STM32N6 DCMIPP driver contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32N6_DCMIPP_ARCH_H_
#define TIKU_STM32N6_DCMIPP_ARCH_H_

#include <stdint.h>

#include <kernel/process/tiku_process.h>

#define TIKU_DCMIPP_OK                    0
#define TIKU_DCMIPP_ERR_INVALID         -1
#define TIKU_DCMIPP_ERR_DISABLED        -2
#define TIKU_DCMIPP_ERR_PIPE_DISABLED   -3
#define TIKU_DCMIPP_ERR_BUSY            -4
#define TIKU_DCMIPP_ERR_NOT_ALLOWED     -5
#define TIKU_DCMIPP_ERR_QUEUE_FULL      -6
#define TIKU_DCMIPP_ERR_NOT_IMPLEMENTED -7

#ifndef TIKU_DCMIPP_EVENT_ID
#define TIKU_DCMIPP_EVENT_ID ((tiku_event_t)(TIKU_EVENT_USER + 1U))
#endif

/**
 * @brief DCMIPP capture pipe identifier.
 *
 * PIPE0 is the dump pipe, PIPE1 is the ISP/main pipe, and PIPE2 is the
 * ancillary pipe. The enum value is also the hardware pipe index.
 */
typedef enum {
    PIPE0 = 0,
    PIPE1 = 1,
    PIPE2 = 2,
    DCMIPP_PIPE0 = PIPE0,
    DCMIPP_PIPE1 = PIPE1,
    DCMIPP_PIPE2 = PIPE2,
    DCMIPP_PIPE_COUNT = 3,
    DCMIPP_PIPE_NONE = 0xff
} tiku_dcmipp_pipe_t;

/** @brief DCMIPP input source selector. */
typedef enum {
    DCMIPP_INPUT_PARALLEL = 0,
    DCMIPP_INPUT_CSI = 1
} tiku_dcmipp_input_t;

/** @brief Pipe2 source selection controlled by the Pipe1 PIPEDIFF bit. */
typedef enum {
    DCMIPP_PIPEDIFF_COUPLED = 0,
    DCMIPP_PIPEDIFF_INDEPENDENT = 1
} tiku_dcmipp_pipe_diff_source_t;

/** @brief One-shot or free-running capture mode. */
typedef enum {
    DCMIPP_CAPTURE_SNAPSHOT = 0,
    DCMIPP_CAPTURE_CONTINUOUS = 1
} tiku_dcmipp_capture_mode_t;

/** @brief DCMIPP event class delivered on TIKU_DCMIPP_EVENT_ID. */
typedef enum {
    DCMIPP_EVENT_NONE = 0,
    DCMIPP_EVENT_FRAME_COMPLETE,
    DCMIPP_EVENT_VSYNC,
    DCMIPP_EVENT_LINE,
    DCMIPP_EVENT_LIMIT,
    DCMIPP_EVENT_ERROR,
    DCMIPP_EVENT_CSI_SOF,
    DCMIPP_EVENT_CSI_EOF,
    DCMIPP_EVENT_CSI_SHORT_PACKET,
    DCMIPP_EVENT_CSI_TIMER,
    DCMIPP_EVENT_CSI_LINE_BYTE
} tiku_dcmipp_event_type_t;

/** @brief DCMIPP/CSI error classifier used in posted events. */
typedef enum {
    DCMIPP_ERROR_NONE = 0,
    DCMIPP_ERROR_AXI_TRANSFER,
    DCMIPP_ERROR_PARALLEL_SYNC,
    DCMIPP_ERROR_PIPE_OVERRUN,
    DCMIPP_ERROR_CSI_SYNC,
    DCMIPP_ERROR_CSI_WATCHDOG,
    DCMIPP_ERROR_CSI_SHORT_PACKET,
    DCMIPP_ERROR_CSI_ID,
    DCMIPP_ERROR_CSI_CORRECTED_ECC,
    DCMIPP_ERROR_CSI_ECC,
    DCMIPP_ERROR_CSI_CRC,
    DCMIPP_ERROR_CSI_CLOCK_CHANGER_FIFO,
    DCMIPP_ERROR_CSI_DPHY_LANE
} tiku_dcmipp_error_code_t;

/**
 * @brief Destination addresses for DCMIPP output planes.
 *
 * @p count is the number of valid entries in @p plane and must be 1..3. The
 * same type is used by capture start and double-buffer address update so
 * multi-planar formats share one address contract.
 */
typedef struct {
    uintptr_t plane[3];
    uint8_t count;
} tiku_dcmipp_plane_address_t;

/**
 * @brief Common pipe configuration.
 *
 * This single structure is accepted for Pipe0, Pipe1, and Pipe2. DataTypeMode
 * maps to the DTMODE field for Pipe0 and Pipe1. When @p pipe is PIPE2,
 * DataTypeMode is ignored because Pipe2 hardware has no DTMODE field.
 */
typedef struct {
    uint32_t DataTypeMode;
    uint32_t DataTypeIDA;
    uint32_t DataTypeIDB;
    uint32_t frame_rate;
    uint32_t flow_selection_flags;
    uint32_t pixel_packer;
    uint32_t crop_start;
    uint32_t crop_size;
    uint32_t dump_limit;
} tiku_dcmipp_pipe_config_t;

/** @brief Crop-style component configuration. */
typedef struct {
    uint32_t start;
    uint32_t size;
} tiku_dcmipp_crop_config_t;

/** @brief Dump-limit component configuration for Pipe0. */
typedef struct {
    /** Dump limit in words; hardware accepts the low 24 bits. */
    uint32_t limit_words;
    /** Nonzero sets the Pipe0 dump-limit enable bit. */
    uint8_t enable;
} tiku_dcmipp_dump_limit_config_t;

/** @brief Generic decimation component configuration. */
typedef struct {
    uint8_t horizontal;
    uint8_t vertical;
    uint8_t enable;
} tiku_dcmipp_decimation_config_t;

/** @brief Pixel-packer component configuration. */
typedef struct {
    uint32_t control;
    uint32_t pitch[3];
} tiku_dcmipp_pixel_packer_config_t;

/**
 * @brief Shared ISP configuration.
 *
 * The STM32N6 has one ISP instance associated with Pipe1. Pipe2 may consume
 * that output when PIPEDIFF is coupled, but this configuration is still global
 * to the single ISP block and is therefore not pipe-parameterized.
 */
typedef struct {
    uint32_t stat_removal;
    uint32_t bad_pixel_removal;
    uint32_t black_level;
    uint32_t exposure1;
    uint32_t exposure2;
    uint32_t statistics[3];
    uint32_t statistics_window_start;
    uint32_t statistics_window_size;
    uint32_t demosaic;
} tiku_dcmipp_isp_config_t;

/**
 * @brief Pipe1/Pipe2 postprocessing configuration.
 *
 * Pipe1 maps @p color_conversion[0..6] to the Pipe1 color-conversion
 * registers and @p yuv_conversion[0] to P1YUVCR. Pipe2 has no color-conversion
 * register block, so color-conversion and YUV-conversion fields are ignored
 * when configuring PIPE2.
 */
typedef struct {
    uint32_t crop_start;
    uint32_t crop_size;
    uint32_t decimation;
    uint32_t downsize_control;
    uint32_t downsize_ratio;
    uint32_t downsize_destination_size;
    uint32_t roi_common;
    uint32_t color_conversion[9];
    uint32_t gamma;
    uint32_t yuv_conversion[7];
    uint32_t pixel_packer;
    uint32_t pitch[3];
} tiku_dcmipp_postproc_config_t;

/** @brief Event object posted by the DCMIPP/CSI interrupt path. */
typedef struct {
    tiku_dcmipp_event_type_t type;
    tiku_dcmipp_pipe_t pipe;
    uint8_t virtual_channel;
    tiku_dcmipp_error_code_t error;
    uint32_t raw_status;
} tiku_dcmipp_event_t;

/**
 * @brief Decode a DCMIPP event payload received from the process queue.
 *
 * The ISR posts TIKU_DCMIPP_EVENT_ID with a packed tiku_event_data_t payload
 * so the process queue does not hold pointers into ISR-owned storage. Call this
 * from process context when receiving TIKU_DCMIPP_EVENT_ID to recover the event
 * object.
 *
 * @param data Event payload delivered by tiku_process_post().
 * @param out Receives the decoded event; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_event_from_data(tiku_event_data_t data,
                                     tiku_dcmipp_event_t *out);

/** @brief Master-enable the DCMIPP peripheral. */
int tiku_dcmipp_arch_enable(void);

/** @brief Master-disable the DCMIPP peripheral. */
int tiku_dcmipp_arch_disable(void);

/**
 * @brief Select the DCMIPP input source.
 *
 * @param input_type Parallel or CSI input.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_set_input(tiku_dcmipp_input_t input_type);

/** @brief Enable the selected DCMIPP input path. */
int tiku_dcmipp_arch_enable_input(void);

/** @brief Disable the selected DCMIPP input path. */
int tiku_dcmipp_arch_disable_input(void);

/**
 * @brief Configure the Pipe2 source controlled by PIPEDIFF.
 *
 * @param source Coupled Pipe1 ISP output or independent input source.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_set_pipe_diff(tiku_dcmipp_pipe_diff_source_t source);

/**
 * @brief Configure a DCMIPP pipe.
 *
 * The same config type is used for all three pipes. DataTypeMode configures
 * DTMODE on Pipe0 and Pipe1; when @p pipe is PIPE2, DataTypeMode is ignored
 * because Pipe2 hardware has no DTMODE field.
 *
 * @param pipe The pipe to be configured.
 * @param config Pipe configuration; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_configure(tiku_dcmipp_pipe_t pipe,
                                    const tiku_dcmipp_pipe_config_t *config);

/**
 * @brief Enable a pipe without starting capture.
 *
 * @param pipe The pipe to be enabled.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_enable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Disable a pipe.
 *
 * @param pipe The pipe to be disabled.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Start capture on an enabled pipe.
 *
 * @p virtual_channel is used for CSI-sourced pipes. For Pipe2 while PIPEDIFF
 * is coupled, Pipe2 consumes Pipe1 ISP output and @p virtual_channel is
 * ignored.
 *
 * @param pipe The pipe to start capture on.
 * @param address One to three output plane addresses.
 * @param virtual_channel CSI virtual channel, 0..3 when applicable.
 * @param capture_mode Snapshot or continuous capture.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_start_capture(tiku_dcmipp_pipe_t pipe,
                                        const tiku_dcmipp_plane_address_t *address,
                                        uint8_t virtual_channel,
                                        tiku_dcmipp_capture_mode_t capture_mode);

/**
 * @brief Configure a pipe's flow-selection FSCR extension bits.
 *
 * @param pipe Pipe selector.
 * @param flow_selection_flags Raw flow-selection flags to merge with pipe config.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_flow_selection_configure(tiku_dcmipp_pipe_t pipe,
                                                   uint32_t flow_selection_flags);

/**
 * @brief Disable a pipe's flow-selection extension bits.
 *
 * @param pipe Pipe selector.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_flow_selection_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Configure a pipe crop component.
 *
 * @param pipe Pipe selector.
 * @param config Crop configuration; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_crop_configure(tiku_dcmipp_pipe_t pipe,
                                         const tiku_dcmipp_crop_config_t *config);

/**
 * @brief Disable a pipe crop component and reset it to hardware defaults.
 *
 * @param pipe Pipe selector.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_crop_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Configure the Pipe0 dump-limit component.
 *
 * @param pipe Must be PIPE0.
 * @param config Dump-limit configuration; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_dump_limit_configure(tiku_dcmipp_pipe_t pipe,
                                               const tiku_dcmipp_dump_limit_config_t *config);

/**
 * @brief Disable the Pipe0 dump-limit component.
 *
 * @param pipe Must be PIPE0.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_dump_limit_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Configure a Pipe1/Pipe2 decimation component.
 *
 * @param pipe Must be PIPE1 or PIPE2.
 * @param config Decimation configuration; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_decimation_configure(tiku_dcmipp_pipe_t pipe,
                                               const tiku_dcmipp_decimation_config_t *config);

/**
 * @brief Disable a Pipe1/Pipe2 decimation component.
 *
 * @param pipe Must be PIPE1 or PIPE2.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_decimation_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Configure a pipe pixel-packer component.
 *
 * @param pipe Pipe selector.
 * @param config Pixel-packer configuration; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_pixel_packer_configure(tiku_dcmipp_pipe_t pipe,
                                                 const tiku_dcmipp_pixel_packer_config_t *config);

/**
 * @brief Disable a pipe pixel-packer component and reset it to hardware defaults.
 *
 * @param pipe Pipe selector.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_pixel_packer_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Configure a pipe status-output address.
 *
 * @param pipe Pipe selector.
 * @param address Status-output destination address.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_status_output_configure(tiku_dcmipp_pipe_t pipe,
                                                  uintptr_t address);

/**
 * @brief Disable a pipe status-output address.
 *
 * @param pipe Pipe selector.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_status_output_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Enable double-buffer mode for a pipe.
 *
 * @param pipe Pipe selector.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_double_buffer_enable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Update double-buffer destination addresses for a pipe.
 *
 * @param pipe Pipe selector.
 * @param addr One to three output plane addresses.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_double_buffer_update_address(tiku_dcmipp_pipe_t pipe,
                                                  const tiku_dcmipp_plane_address_t *addr);

/**
 * @brief Configure the shared Pipe1 ISP block.
 *
 * @param config ISP configuration; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_isp_setup(const tiku_dcmipp_isp_config_t *config);

/**
 * @brief Enable the shared ISP when Pipe1 or coupled Pipe2 makes it eligible.
 *
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_isp_enable(void);

/** @brief Disable the shared ISP block. */
int tiku_dcmipp_arch_isp_disable(void);

/**
 * @brief Configure Pipe1 or Pipe2 postprocessing.
 *
 * Pipe1 and Pipe2 own separate postprocessing instances, even when Pipe2 is
 * consuming the shared ISP output.
 *
 * @param pipe PIPE1 or PIPE2.
 * @param config Postprocessing configuration; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_postprocessing_configure(tiku_dcmipp_pipe_t pipe,
                                              const tiku_dcmipp_postproc_config_t *config);

/**
 * @brief Enable Pipe1 or Pipe2 postprocessing.
 *
 * @param pipe PIPE1 or PIPE2.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_postprocessing_enable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Disable Pipe1 or Pipe2 postprocessing.
 *
 * @param pipe PIPE1 or PIPE2.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_postprocessing_disable(tiku_dcmipp_pipe_t pipe);

/**
 * @brief Enable DCMIPP common or per-pipe interrupt sources.
 *
 * @param pipe Pipe selector.
 * @param interrupt_mask Mask using STM32N6_DCMIPP_IT_* bits.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_interrupt_enable(tiku_dcmipp_pipe_t pipe,
                                           uint32_t interrupt_mask);

/**
 * @brief Disable DCMIPP common or per-pipe interrupt sources.
 *
 * @param pipe Pipe selector.
 * @param interrupt_mask Mask using STM32N6_DCMIPP_IT_* bits.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_pipe_interrupt_disable(tiku_dcmipp_pipe_t pipe,
                                            uint32_t interrupt_mask);

/**
 * @brief Enable CSI interrupt sources.
 *
 * @param ier0_mask Mask for CSI IER0 using STM32N6_CSI_IT_* IER0 bits.
 * @param ier1_mask Mask for CSI IER1 using STM32N6_CSI_IT_* IER1 bits.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_csi_interrupt_enable(uint32_t ier0_mask,
                                          uint32_t ier1_mask);

/**
 * @brief Disable CSI interrupt sources.
 *
 * @param ier0_mask Mask for CSI IER0 using STM32N6_CSI_IT_* IER0 bits.
 * @param ier1_mask Mask for CSI IER1 using STM32N6_CSI_IT_* IER1 bits.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_csi_interrupt_disable(uint32_t ier0_mask,
                                           uint32_t ier1_mask);

/**
 * @brief Read the Pipe0 dump frame counter.
 *
 * The peripheral must be enabled. This API is valid only for PIPE0 because
 * Pipe0 exposes the dump counter register.
 *
 * @param pipe Must be PIPE0.
 * @param out Receives the counter; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_get_dump_count(tiku_dcmipp_pipe_t pipe, uint32_t *out);

/**
 * @brief Read the Pipe1/Pipe2 frame counter.
 *
 * The peripheral must be enabled. STM32N6 exposes one common frame counter
 * register, selected by CMCR.PSFC; this function selects PIPE1 or PIPE2 before
 * reading that common counter.
 *
 * @param pipe Must be PIPE1 or PIPE2.
 * @param out Receives the counter; must not be NULL.
 * @return TIKU_DCMIPP_OK, or a negative error.
 */
int tiku_dcmipp_arch_get_frame_count(tiku_dcmipp_pipe_t pipe, uint32_t *out);

/** @brief DCMIPP global interrupt entry, installed in the vector table. */
void tiku_stm32n6_dcmipp_isr(void);

/** @brief CSI global interrupt entry, installed in the vector table. */
void tiku_stm32n6_csi_isr(void);

#endif /* TIKU_STM32N6_DCMIPP_ARCH_H_ */
