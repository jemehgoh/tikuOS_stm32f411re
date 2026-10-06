/*
 * Tiku Operating System v0.06
 *
 * STM32N6 single-slot model store backed by the unclaimed OSPI NOR range.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32N6_MODEL_STORE_H_
#define TIKU_STM32N6_MODEL_STORE_H_

#include <stddef.h>
#include <stdint.h>

#include <interfaces/npu/tiku_npu.h>

#include <kernel/fs/tiku_bigblob.h>

/**
 * @brief The bounded image inputs owned by the STM32N6 model manager.
 *
 * The production image is a combined ST relocatable model. Consequently the
 * parameter image is absent and file_ptr points at the relocatable header at
 * the beginning of the raw OSPI slot.
 */
typedef struct {
    uintptr_t file_ptr;
    uintptr_t file_params_ptr;
    size_t file_bytes;
    size_t params_bytes;
} tiku_n6_model_image_t;

/** @brief Lifecycle of the optional chunked provisioning operation. */
typedef enum {
    TIKU_N6_MODEL_STORE_IDLE = 0,
    TIKU_N6_MODEL_STORE_WRITING,
    TIKU_N6_MODEL_STORE_DONE,
    TIKU_N6_MODEL_STORE_ERR_WRITE,
    TIKU_N6_MODEL_STORE_ERR_VERIFY
} tiku_n6_model_store_state_t;

/**
 * @brief Resolve the published model reference into a bounded raw image.
 *
 * This is the single model-manager contract used by the LL-ATON adapter.
 */
int tiku_n6_model_store_resolve(const char *model_ref,
                                tiku_n6_model_image_t *out);

/**
 * @brief Describe the raw production image, using the legacy result type.
 *
 * The returned length and CRC describe bytes at slot offset zero.  No
 * tiku_bigblob header is present on the STM32N6 slot.
 */
int tiku_n6_model_store_info(tiku_bigblob_info_t *out);

/** @brief Verify the raw production image against its generated manifest. */
int tiku_n6_model_store_verify(void);

/**
 * @brief Return the verified raw model in the mapped OSPI window.
 *
 * The returned pointer remains valid until the next OSPI write/erase.  The
 * model backend returns the ST relocatable image beginning at slot offset
 * zero; no storage header is skipped.
 */
const void *tiku_n6_model_store_map(uint32_t *length);

/**
 * @brief Start replacing the production raw image with caller-owned bytes.
 *
 * The input must match the generated production length and CRC.  This path
 * is maintenance-safe through the model interlock but is not power-loss
 * atomic.
 */
int tiku_n6_model_store_begin(const char *name, const void *source,
                              uint32_t length);

/**
 * @brief Advance provisioning by one bounded write step.
 *
 * @return 1 while more work remains, 0 when complete or failed.
 */
int tiku_n6_model_store_step(uint32_t *completed);

/** @brief Report whether a chunked provisioning write is active. */
int tiku_n6_model_store_busy(void);

/**
 * @brief Reserve or release the slot for the NPU model binding.
 *
 * The N6 backend uses this interlock so provisioning cannot erase a payload
 * that LL-ATON is still using as its mapped source.
 */
void tiku_n6_model_store_set_model_bound(int bound);

/** @brief Report the last provisioning result. */
tiku_n6_model_store_state_t tiku_n6_model_store_state(void);

#endif /* TIKU_STM32N6_MODEL_STORE_H_ */
