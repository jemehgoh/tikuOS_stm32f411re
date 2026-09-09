/*
 * Tiku Operating System v0.06
 *
 * STM32N6 single-slot model store backed by the unclaimed XSPI NOR range.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32N6_MODEL_STORE_H_
#define TIKU_STM32N6_MODEL_STORE_H_

#include <stdint.h>

#include <kernel/fs/tiku_bigblob.h>

/** @brief Lifecycle of the optional chunked provisioning operation. */
typedef enum {
    TIKU_N6_MODEL_STORE_IDLE = 0,
    TIKU_N6_MODEL_STORE_WRITING,
    TIKU_N6_MODEL_STORE_DONE,
    TIKU_N6_MODEL_STORE_ERR_WRITE,
    TIKU_N6_MODEL_STORE_ERR_VERIFY
} tiku_n6_model_store_state_t;

/** @brief Describe the published production model, if one exists. */
int tiku_n6_model_store_info(tiku_bigblob_info_t *out);

/** @brief Verify the published model payload against its stored CRC. */
int tiku_n6_model_store_verify(void);

/**
 * @brief Return the verified model payload in the mapped XSPI window.
 *
 * The returned pointer remains valid until the next XSPI write/erase.  The
 * model backend never returns the 64 KiB bigblob header as model data.
 */
const void *tiku_n6_model_store_map(uint32_t *length);

/** @brief Start replacing the production model with a caller-owned payload. */
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
