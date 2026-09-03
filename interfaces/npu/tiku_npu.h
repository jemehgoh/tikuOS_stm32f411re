/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tiku_npu.h - the neural accelerator contract.
 *
 * Legacy targets use a named store model. STM32N6 uses a fixed, linked
 * Neural-ART model container and an event-driven submit path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_NPU_H_
#define TIKU_NPU_H_

#include <stddef.h>
#include <stdint.h>

#if defined(PLATFORM_STM32N6)
#include <kernel/process/tiku_process.h>
#endif

/** @brief Zero where no backend is compiled in, so callers can compile out. */
#ifndef TIKU_HAS_NPU
#define TIKU_HAS_NPU            0
#endif

#define TIKU_NPU_OK              0
#define TIKU_NPU_ERR_STATE      -1  /**< gated, or nothing loaded to run   */
#define TIKU_NPU_ERR_MODEL      -2  /**< no such model, or not for this part */
#define TIKU_NPU_ERR_TIMEOUT    -3  /**< submitted, never reached the end   */
#define TIKU_NPU_ERR_FAULT      -4  /**< the accelerator rejected the work  */
#define TIKU_NPU_ERR_BUSY       -5  /**< another fixed model is executing   */
#define TIKU_NPU_ERR_ARGUMENT   -6  /**< malformed model or tensor list     */
#define TIKU_NPU_ERR_NOT_FOUND  -7  /**< VFS model path does not exist     */
#define TIKU_NPU_ERR_HEADER     -8  /**< truncated or malformed container  */
#define TIKU_NPU_ERR_CAPACITY   -9  /**< container exceeds this model slot */
#define TIKU_NPU_ERR_IO        -10  /**< VFS/backend failure                */

#if defined(PLATFORM_STM32N6)

/** Magic and ABI version for a compiler-produced fixed-address model. */
#define TIKU_NPU_MODEL_MAGIC       UINT32_C(0x544E3650) /* 'TN6P' */
#define TIKU_NPU_MODEL_ABI_VERSION UINT16_C(1)

/** Model has absolute IO and epoch-blob addresses; no copy/relocation exists. */
#define TIKU_NPU_MODEL_F_FIXED     UINT16_C(1u << 0)

/*
 * A VFS model is a description of a future load, not a heap handle.  These
 * limits are part of the image ABI: a model slot is statically declared and
 * its eventual weights, parameters, activations, and ecblob must fit inside
 * the linker-owned NPU extent before a loader is ever allowed to run.
 *
 * The per-kind limits default to the whole slot so the sum check remains the
 * source of truth.  Builds with a tighter Neural-ART memory map can override
 * any of them with -D flags.
 */
#ifndef TIKU_NPU_MODEL_SLOT_BYTES
# ifdef TIKU_TIER_NPU_SIZE
#  define TIKU_NPU_MODEL_SLOT_BYTES ((uint32_t)(TIKU_TIER_NPU_SIZE))
# else
#  define TIKU_NPU_MODEL_SLOT_BYTES UINT32_C(524288)
# endif
#endif
#ifndef TIKU_NPU_MODEL_MAX_WEIGHTS_BYTES
#define TIKU_NPU_MODEL_MAX_WEIGHTS_BYTES TIKU_NPU_MODEL_SLOT_BYTES
#endif
#ifndef TIKU_NPU_MODEL_MAX_PARAMS_BYTES
#define TIKU_NPU_MODEL_MAX_PARAMS_BYTES TIKU_NPU_MODEL_SLOT_BYTES
#endif
#ifndef TIKU_NPU_MODEL_MAX_ACTIVATION_BYTES
#define TIKU_NPU_MODEL_MAX_ACTIVATION_BYTES TIKU_NPU_MODEL_SLOT_BYTES
#endif
#ifndef TIKU_NPU_MODEL_MAX_ECBLOB_BYTES
#define TIKU_NPU_MODEL_MAX_ECBLOB_BYTES TIKU_NPU_MODEL_SLOT_BYTES
#endif
#ifndef TIKU_NPU_MODEL_MAX_IO
#define TIKU_NPU_MODEL_MAX_IO 8u
#endif
#ifndef TIKU_NPU_MODEL_MAX_RANK
#define TIKU_NPU_MODEL_MAX_RANK 8u
#endif
#ifndef TIKU_NPU_MODEL_PATH_MAX
#define TIKU_NPU_MODEL_PATH_MAX 128u
#endif

/** Magic/version for the little-endian VFS container header. */
#define TIKU_NPU_MODEL_CONTAINER_MAGIC TIKU_NPU_MODEL_MAGIC
#define TIKU_NPU_MODEL_CONTAINER_VERSION UINT16_C(2)
#define TIKU_NPU_MODEL_CONTAINER_F_NONE UINT32_C(0)

/** Fixed wire-header geometry; bind maps the object but only inspects this header. */
#define TIKU_NPU_MODEL_CONTAINER_BASE_BYTES UINT16_C(56)
#define TIKU_NPU_MODEL_CONTAINER_TENSOR_BYTES UINT16_C(40)
#define TIKU_NPU_MODEL_CONTAINER_HEADER_BYTES \
    ((size_t)TIKU_NPU_MODEL_CONTAINER_BASE_BYTES + \
     (size_t)(2u * TIKU_NPU_MODEL_MAX_IO) * \
     (size_t)TIKU_NPU_MODEL_CONTAINER_TENSOR_BYTES)

/** Tensor element type values carried by a container descriptor. */
typedef enum {
    TIKU_NPU_TENSOR_INT8 = 1,
    TIKU_NPU_TENSOR_UINT8,
    TIKU_NPU_TENSOR_INT16,
    TIKU_NPU_TENSOR_INT32,
    TIKU_NPU_TENSOR_FLOAT32
} tiku_npu_tensor_type_t;

/** One type/shape descriptor from the container header. */
typedef struct {
    uint8_t type;                         /**< tiku_npu_tensor_type_t       */
    uint8_t rank;                         /**< number of valid shape entries */
    uint16_t flags;                       /**< reserved for future metadata  */
    uint32_t shape[TIKU_NPU_MODEL_MAX_RANK];
    int32_t zero_point;                   /**< retained for integer IO metadata */
} tiku_npu_tensor_t;

/** Caller-owned destination for tiku_npu_model_io(); no allocation involved. */
typedef struct {
    uint16_t input_count;
    uint16_t output_count;
    tiku_npu_tensor_t inputs[TIKU_NPU_MODEL_MAX_IO];
    tiku_npu_tensor_t outputs[TIKU_NPU_MODEL_MAX_IO];
} tiku_npu_model_io_t;

/**
 * @brief Small container around ST Neural-ART generated static tables.
 *
 * The addresses in the IO arrays are the addresses already encoded in the
 * compiler's micro-instructions. Callers must pass those exact buffers;
 * changing them would require ST's ec_reloc path, which this backend does
 * not use.
 */
typedef struct tiku_npu_model {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t flags;
    const uint64_t *epoch_blob;
    uint32_t epoch_blob_bytes;
    uint16_t input_count;
    uint16_t output_count;
    const uintptr_t *input_addresses;
    const uintptr_t *output_addresses;
    const uint32_t *input_sizes;
    const uint32_t *output_sizes;
    uint32_t estimated_latency_cycles;

    /* VFS-container binding state.  The arrays are in the descriptor itself,
     * so bind/io remain static and allocation-free.  They are separate from
     * the legacy fixed-address fields above, which keep embedded models ABI
     * compatible with Step 3. */
    uint32_t slot_capacity;
    uint32_t weights_bytes;
    uint32_t params_bytes;
    uint32_t activation_bytes;
    uint32_t ecblob_bytes;
    uint32_t weights_offset;
    uint32_t params_offset;
    uint32_t activation_offset;
    uint32_t ecblob_offset;
    uint32_t container_total_bytes;
    uint16_t container_input_count;
    uint16_t container_output_count;
    uint8_t container_bound;
    uint8_t container_loaded;
    uint8_t container_reserved[2];
    char container_path[TIKU_NPU_MODEL_PATH_MAX];
    uint8_t *extent_base;
    uint32_t extent_offset;
    uint32_t extent_bytes;
    tiku_npu_tensor_t container_inputs[TIKU_NPU_MODEL_MAX_IO];
    tiku_npu_tensor_t container_outputs[TIKU_NPU_MODEL_MAX_IO];
} tiku_npu_model_t;

/**
 * @brief Statically declare one model slot.
 *
 * This mirrors TIKU_PROCESS(): the storage has static lifetime and the model
 * descriptor is not obtained from a heap allocator.  The capacity constants
 * above are compile-time properties of every declared slot.
 */
#define TIKU_NPU_MODEL(name) \
    static tiku_npu_model_t name = { \
        .slot_capacity = TIKU_NPU_MODEL_SLOT_BYTES \
    }

/** Bind a VFS-resident container by parsing only its fixed header. */
int tiku_npu_model_bind(tiku_npu_model_t *model, const char *vfs_path);

/** Copy the bound container into the reserved NPU extent, without relocation. */
int tiku_npu_model_load(tiku_npu_model_t *model);

/** Release the model's NPU extent slice and make the slot reusable. */
int tiku_npu_model_unload(tiku_npu_model_t *model);

/** Copy parsed input/output metadata into caller-provided static storage. */
int tiku_npu_model_io(const tiku_npu_model_t *model,
                      tiku_npu_model_io_t *out);

/** Submit a build-embedded fixed-address model without waiting. */
int tiku_npu_run(const tiku_npu_model_t *model,
                 const void *in[], void *out[]);

/**
 * Submit and yield the calling protothread until NPU completion.
 * Must be used inside TIKU_PROCESS_THREAD, where `ev` and `process_pt` exist.
 */
#define tiku_npu_run_sync(model, in, out)                                  \
    do {                                                                    \
        int _tiku_npu_sync_rc = tiku_npu_run((model), (in), (out));         \
        if (_tiku_npu_sync_rc == TIKU_NPU_OK) {                             \
            TIKU_PROCESS_WAIT_EVENT_UNTIL(ev == TIKU_EVENT_NPU_DONE);       \
        }                                                                   \
    } while (0)

#else

/** @brief Models are named files in the store rather than linked-in arrays. */
#define TIKU_NPU_F_STORE_MODEL  (1u << 0)
/** @brief Integer quantised networks only; no float path exists. */
#define TIKU_NPU_F_INT_ONLY     (1u << 1)

typedef enum {
    TIKU_NPU_ABSENT = 0,    /**< no accelerator on this part      */
    TIKU_NPU_GATED,         /**< present, powered down            */
    TIKU_NPU_IDLE,          /**< released, nothing loaded         */
    TIKU_NPU_READY,         /**< a model is loaded and runnable   */
    TIKU_NPU_FAULTED        /**< a run failed; reload to recover  */
} tiku_npu_state_t;

/** @brief What the accelerator is, and what the loaded model asks of it. */
typedef struct {
    uint16_t macs;          /**< multiply-accumulates per cycle   */
    uint16_t shram_kb;      /**< the accelerator's own memory     */
    uint32_t arena;         /**< working buffer the model needs   */
    uint32_t in_bytes;      /**< 0 until a model is loaded        */
    uint32_t out_bytes;
} tiku_npu_info_t;

/**
 * @brief Which parts of this contract the backend actually implements.
 *
 * @return A mask of TIKU_NPU_F_*
 */
uint32_t tiku_npu_flags(void);

/**
 * @brief Where the accelerator is in its lifecycle.
 *
 * @return One of tiku_npu_state_t
 */
tiku_npu_state_t tiku_npu_state(void);

/**
 * @brief Power and release the accelerator.
 *
 * @return TIKU_NPU_OK, or TIKU_NPU_ERR_STATE
 */
int tiku_npu_start(void);

/**
 * @brief Return the accelerator to its powered-down state.
 */
void tiku_npu_stop(void);

/**
 * @brief Take a model from the store and make it the one that runs.
 *
 * @param name  File in the store, packed for this backend
 * @return TIKU_NPU_OK, or TIKU_NPU_ERR_MODEL
 */
int tiku_npu_load(const char *name);

/**
 * @brief Describe the accelerator and the loaded model.
 *
 * @param out  Filled on success
 * @return TIKU_NPU_OK, or TIKU_NPU_ERR_STATE
 */
int tiku_npu_info(tiku_npu_info_t *out);

/**
 * @brief The input buffer to fill before a run.
 *
 * @param len  Out: its size in bytes, or NULL
 * @return Pointer to write into, or NULL with no model loaded
 */
void *tiku_npu_input(uint32_t *len);

/**
 * @brief The output buffer, valid once a run has returned OK.
 *
 * @param len  Out: its size in bytes, or NULL
 * @return Pointer to read from, or NULL with no model loaded
 */
const void *tiku_npu_output(uint32_t *len);

/**
 * @brief Run the loaded model over the input buffer and block until it ends.
 *
 * @return TIKU_NPU_OK, ERR_STATE, ERR_TIMEOUT or ERR_FAULT
 */
int tiku_npu_run(void);

/**
 * @brief Runs completed since boot, for observability.
 *
 * @return The count
 */
uint32_t tiku_npu_runs(void);

#endif /* PLATFORM_STM32N6 */

#endif /* TIKU_NPU_H_ */
