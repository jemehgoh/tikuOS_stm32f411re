/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 *
 * Author: Jeremy Goh
 *
 * tiku_npu_arch.c - STM32N6 Neural-ART fixed embedded-model backend.
 *
 * The NPU tier is a linker-owned tail of AXISRAM.  Keeping it out of the
 * general SRAM and NVM tiers prevents a future runtime from making model
 * workspace compete with OS allocations or persistent storage.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <kernel/memory/tiku_mem.h>
#include <interfaces/npu/tiku_npu.h>
#include <kernel/fs/tiku_model.h>
#include <kernel/vfs/tree/tiku_vfs_tree_data.h>

#include "tiku_npu_arch.h"
#include "tiku_cache_arch.h"
#include "tiku_sram_arch.h"
#include "tiku_stm32n6_regs.h"

/* Smallest generated Neural-ART epoch blob used by the IRQ self-test. It is
 * kept in ordinary image read-only storage, never in the reserved NPU tier. */
static const uint64_t npu_epoch_selftest_blob[8]
    __attribute__((aligned(8), used)) = {
    0x00000003ca057a7aULL,
    0x0000035d0000033dULL,
    0x000000000000000cULL,
};

typedef struct {
    volatile uint32_t control;
    volatile uint32_t version;
    volatile uint32_t address;
    volatile uint32_t interrupt;
} tiku_npu_epoch_controller_t;

extern uint8_t __axisram_start;
extern uint8_t __tier_sram_start;
extern uint8_t __tier_sram_end;
extern uint8_t __tier_npu_start;
extern uint8_t __tier_npu_end;
extern uint8_t __uninit_start;
extern uint8_t __uninit_end;

static volatile tiku_npu_epoch_controller_t *epoch_controller;
static uint32_t npu_clock_readback;
static uint8_t npu_initialized;
static volatile uint32_t npu_epoch_irq_count;
static volatile uint32_t npu_epoch_trigger_cycle;
static volatile uint32_t npu_epoch_last_latency_cycles;
static volatile struct tiku_process *npu_run_owner;
static volatile const tiku_npu_model_t *npu_active_model;
static tiku_npu_model_t *npu_loaded_model;

#define STM32N6_NPU_IRQ_PRIORITY       1U
#define STM32N6_NPU_IRQ_TIMEOUT_CYCLES 150000U
#define STM32N6_NPU_IRQ_SETTLE_CYCLES  2000U

static void npu_epoch_ack(void);

static uint32_t npu_cycles(void)
{
    return TIKU_REG32(STM32N6_DWT_CYCCNT);
}

static void npu_cycle_counter_enable(void)
{
    TIKU_REG32(STM32N6_SCB_DEMCR) |= STM32N6_SCB_DEMCR_TRCENA;
    TIKU_REG32(STM32N6_DWT_LAR) = STM32N6_DWT_LAR_KEY;
    TIKU_REG32(STM32N6_DWT_CTRL) |= STM32N6_DWT_CTRL_CYCCNTENA;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_internal_clock_enable(void)
{
    /* Mirror the ordering used by ST's LL_ATON_Init(): clear the ATON
     * pipeline, enable the clock controller, then open the accelerator and
     * block gates.  In particular, BGATES bit 25 clocks EPOCHCTRL0. */
    TIKU_REG32(STM32N6_ATON_CLKCTRL_CTRL) = STM32N6_ATON_CLKCTRL_CTRL_CLR;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");

    TIKU_REG32(STM32N6_ATON_CLKCTRL_CTRL) = STM32N6_ATON_CLKCTRL_CTRL_EN;
    TIKU_REG32(STM32N6_ATON_CLKCTRL_AGATES0) = 0xffffffffUL;
    TIKU_REG32(STM32N6_ATON_CLKCTRL_AGATES1) = 0xffffffffUL;
    TIKU_REG32(STM32N6_ATON_CLKCTRL_BGATES) = 0xffffffffUL;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_reset_release(void)
{
    /* The boot ROM leaves the NPU domain in reset on this SRAM-boot path.
     * Keep the outer RCC clock on while asserting and releasing reset, as the
     * STM32N6 reference configuration does. */
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) |= STM32N6_RCC_AHB5RSTR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) &= ~STM32N6_RCC_AHB5RSTR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_cacheaxi_enable(void)
{
    /* ST's NPU_Config() enables CACHEAXI, resets it, disables sleep gating,
     * and HAL_CACHEAXI_Init() finally sets CR1.EN. */
    TIKU_REG32(STM32N6_NPU_RCC_AHB5ENR) |= STM32N6_RCC_AHB5ENR_CACHEAXI;
    TIKU_REG32(STM32N6_RCC_AHB5LPENR) &= ~STM32N6_RCC_AHB5LPENR_CACHEAXI;
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) |= STM32N6_RCC_AHB5RSTR_CACHEAXI;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(STM32N6_RCC_AHB5RSTR) &= ~STM32N6_RCC_AHB5RSTR_CACHEAXI;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    TIKU_REG32(STM32N6_CACHEAXI_CR1) |= STM32N6_CACHEAXI_CR1_EN;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void npu_rif_configure(void)
{
    /* Match ST's secure NPU_Validation configuration: NPU bus transactions
     * use trusted CID 1 and secure privileged attributes, while NPU control
     * registers remain secure privileged. */
    uint32_t attr = TIKU_REG32(STM32N6_RIFSC_RIMC_ATTR1);

    attr &= ~(STM32N6_RIFSC_RIMC_ATTR_MCID |
              STM32N6_RIFSC_RIMC_ATTR_SEC |
              STM32N6_RIFSC_RIMC_ATTR_PRIV);
    attr |= STM32N6_RIFSC_RIMC_NPU_CID1 |
            STM32N6_RIFSC_RIMC_ATTR_SEC |
            STM32N6_RIFSC_RIMC_ATTR_PRIV;
    TIKU_REG32(STM32N6_RIFSC_RIMC_ATTR1) = attr;

    TIKU_REG32(STM32N6_RIFSC_RISC_SECCFGR3) |= STM32N6_RIFSC_NPU_BIT;
    TIKU_REG32(STM32N6_RIFSC_RISC_PRIVCFGR3) |= STM32N6_RIFSC_NPU_BIT;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

void tiku_stm32n6_npu_end_of_epoch_isr(void)
{
    uint32_t status = TIKU_REG32(STM32N6_EPOCHCTRL_IRQ);
    struct tiku_process *owner = NULL;
    const tiku_npu_model_t *model = NULL;

    if ((status & STM32N6_EPOCHCTRL_IRQ_DONE) != 0U) {
        npu_epoch_last_latency_cycles = npu_cycles() - npu_epoch_trigger_cycle;
        npu_epoch_irq_count++;
        owner = (struct tiku_process *)npu_run_owner;
        model = (const tiku_npu_model_t *)npu_active_model;
        npu_run_owner = NULL;
        npu_active_model = NULL;
    }

    /* Neural-ART source first, then its interrupt-controller latch. */
    if (status != 0U) {
        TIKU_REG32(STM32N6_EPOCHCTRL_IRQ) = status;
    }
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTCLR) = STM32N6_NPU_INTCTRL_EPOCH0_INT;
    __asm__ volatile ("dsb" ::: "memory");

    /* The process pointer is captured at submit time. Passing it to the
     * process queue is a unicast post; NULL would broadcast the completion. */
    if (owner != NULL && model != NULL) {
        (void)tiku_process_post(owner, TIKU_EVENT_NPU_DONE,
                                (tiku_event_data_t)model);
    }
}

static int npu_model_is_valid(const tiku_npu_model_t *model,
                              const void *in[], void *out[])
{
    uint16_t i;

    /* A loaded VFS model has no runtime address tables to patch.  Its
     * micro-instructions already contain the addresses selected by the model
     * build, so the caller supplies the same fixed IO buffers as for an
     * embedded model and this path only checks that they are present. */
    if (model != NULL && model->container_loaded) {
        if (npu_loaded_model != model || model->extent_base == NULL ||
            in == NULL || out == NULL || model->epoch_blob == NULL ||
            model->epoch_blob_bytes == 0u ||
            (model->epoch_blob_bytes & 7u) != 0u ||
            (((uintptr_t)model->epoch_blob) & 7u) != 0u ||
            model->container_input_count == 0u ||
            model->container_output_count == 0u ||
            model->container_input_count > TIKU_NPU_MODEL_MAX_IO ||
            model->container_output_count > TIKU_NPU_MODEL_MAX_IO) {
            return 0;
        }
        for (i = 0u; i < model->container_input_count; i++) {
            if (in[i] == NULL ||
                (model->input_addresses != NULL &&
                 (uintptr_t)in[i] != model->input_addresses[i])) {
                return 0;
            }
        }
        for (i = 0u; i < model->container_output_count; i++) {
            if (out[i] == NULL ||
                (model->output_addresses != NULL &&
                 (uintptr_t)out[i] != model->output_addresses[i])) {
                return 0;
            }
        }
        return 1;
    }

    if (model == NULL || in == NULL || out == NULL ||
        model->magic != TIKU_NPU_MODEL_MAGIC ||
        model->abi_version != TIKU_NPU_MODEL_ABI_VERSION ||
        (model->flags & TIKU_NPU_MODEL_F_FIXED) == 0U ||
        model->epoch_blob == NULL || model->epoch_blob_bytes == 0U ||
        (model->epoch_blob_bytes & 7U) != 0U ||
        (((uintptr_t)model->epoch_blob) & 7U) != 0U ||
        model->input_count == 0U || model->output_count == 0U ||
        model->input_addresses == NULL || model->output_addresses == NULL ||
        model->input_sizes == NULL || model->output_sizes == NULL) {
        return 0;
    }

    if (model->input_count > TIKU_NPU_MODEL_MAX_IO ||
        model->output_count > TIKU_NPU_MODEL_MAX_IO) {
        return 0;
    }

    for (i = 0U; i < model->input_count; i++) {
        if (in[i] == NULL || model->input_sizes[i] == 0U ||
            (uintptr_t)in[i] != model->input_addresses[i]) {
            return 0;
        }
    }
    for (i = 0U; i < model->output_count; i++) {
        if (out[i] == NULL || model->output_sizes[i] == 0U ||
            (uintptr_t)out[i] != model->output_addresses[i]) {
            return 0;
        }
    }
    return 1;
}

/*---------------------------------------------------------------------------*/
/* VFS MODEL CONTAINER                                                       */
/*---------------------------------------------------------------------------*/

static int npu_model_map_source(const char *path,
                                 const uint8_t **source,
                                 size_t *source_len)
{
    static const char data_prefix[] = "/data/";
    tiku_tfs_t *fs;
    tiku_model_t mapped;

    if (path == NULL || source == NULL || source_len == NULL ||
        strncmp(path, data_prefix, sizeof data_prefix - 1u) != 0) {
        return TIKU_NPU_ERR_NOT_FOUND;
    }

    fs = tiku_vfs_tree_data_store();
    if (fs == NULL) {
        return TIKU_NPU_ERR_IO;
    }
    /* Match RA8P1: the generic model layer maps a RAW model in place and
     * leaves backend-specific header validation to the NPU port.  TN6P is not
     * the generic AXM relocation magic, so tiku_model_open() returns it as a
     * RAW model without interpreting the payload. */
    if (tiku_model_open(fs, path + (sizeof data_prefix - 1u), &mapped) !=
        TIKU_MODEL_OK) {
        return TIKU_NPU_ERR_NOT_FOUND;
    }
    *source = mapped.base;
    *source_len = mapped.len;
    return TIKU_NPU_OK;
}

/* The wire format is deliberately decoded a byte at a time.  The /data VFS
 * is a byte store, not a C-struct store, and this keeps the format independent
 * of target alignment, compiler padding, and host endianness. */
#define NPU_CONTAINER_OFF_MAGIC       0u
#define NPU_CONTAINER_OFF_VERSION     4u
#define NPU_CONTAINER_OFF_HEADER      6u
#define NPU_CONTAINER_OFF_FLAGS       8u
#define NPU_CONTAINER_OFF_WEIGHTS    12u
#define NPU_CONTAINER_OFF_PARAMS     16u
#define NPU_CONTAINER_OFF_ACTIVATION 20u
#define NPU_CONTAINER_OFF_ECBLOB     24u
#define NPU_CONTAINER_OFF_INPUTS      28u
#define NPU_CONTAINER_OFF_OUTPUTS     30u
#define NPU_CONTAINER_OFF_DESCS       32u
#define NPU_CONTAINER_OFF_TOTAL       36u
#define NPU_CONTAINER_OFF_WEIGHTS_AT 40u
#define NPU_CONTAINER_OFF_PARAMS_AT   44u
#define NPU_CONTAINER_OFF_ACTIVATION_AT 48u
#define NPU_CONTAINER_OFF_ECBLOB_AT   52u
#define NPU_CONTAINER_DESC_AT         56u

/* One tensor record: type, rank, flags, reserved, 8 dimensions, and zero
 * point.  Quantisation scale/multiplier data stays opaque inside the
 * compiler-produced model payload, as it does for the RA8P1 port. */
#define NPU_CONTAINER_DESC_TYPE       0u
#define NPU_CONTAINER_DESC_RANK       1u
#define NPU_CONTAINER_DESC_FLAGS      2u
#define NPU_CONTAINER_DESC_SHAPE      4u
#define NPU_CONTAINER_DESC_ZERO      36u

static uint16_t npu_model_rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t npu_model_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int npu_model_range_ok(uint32_t offset, uint32_t length,
                              uint32_t payload_at, uint32_t total)
{
    return offset >= payload_at &&
           offset <= total && length <= total - offset;
}

static int npu_model_tensor_decode(const uint8_t *p, tiku_npu_tensor_t *out)
{
    uint8_t i;

    out->type = p[NPU_CONTAINER_DESC_TYPE];
    out->rank = p[NPU_CONTAINER_DESC_RANK];
    out->flags = npu_model_rd16(p + NPU_CONTAINER_DESC_FLAGS);
    if (out->type < TIKU_NPU_TENSOR_INT8 ||
        out->type > TIKU_NPU_TENSOR_FLOAT32 ||
        out->rank > TIKU_NPU_MODEL_MAX_RANK ||
        out->flags != 0u) {
        return 0;
    }
    for (i = 0u; i < TIKU_NPU_MODEL_MAX_RANK; i++) {
        out->shape[i] = npu_model_rd32(
            p + NPU_CONTAINER_DESC_SHAPE + (size_t)i * sizeof(uint32_t));
        if (i < out->rank && out->shape[i] == 0u) {
            return 0;
        }
    }

    out->zero_point = (int32_t)npu_model_rd32(p + NPU_CONTAINER_DESC_ZERO);
    return 1;
}

int tiku_npu_model_bind(tiku_npu_model_t *model, const char *vfs_path)
{
    const uint8_t *source;
    tiku_npu_tensor_t inputs[TIKU_NPU_MODEL_MAX_IO];
    tiku_npu_tensor_t outputs[TIKU_NPU_MODEL_MAX_IO];
    uint32_t sizes[4];
    uint32_t offsets[4];
    uint32_t total;
    uint16_t header_bytes;
    uint16_t desc_bytes;
    uint16_t input_count;
    uint16_t output_count;
    size_t expected_desc_bytes;
    size_t expected_header_bytes;
    size_t desc_at;
    size_t source_len;
    int rc;
    uint16_t i;

    if (model == NULL || vfs_path == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (model->container_loaded) {
        return TIKU_NPU_ERR_BUSY;
    }
    if (strlen(vfs_path) >= TIKU_NPU_MODEL_PATH_MAX) {
        return TIKU_NPU_ERR_ARGUMENT;
    }

    /* A failed rebind must never leave a previously valid model runnable.  A
     * slot is metadata-only after this call, so invalidate any legacy fixed
     * model fields before touching the VFS. */
    model->magic = 0u;
    model->abi_version = 0u;
    model->flags = 0u;
    model->epoch_blob = NULL;
    model->epoch_blob_bytes = 0u;
    model->input_count = 0u;
    model->output_count = 0u;
    model->input_addresses = NULL;
    model->output_addresses = NULL;
    model->input_sizes = NULL;
    model->output_sizes = NULL;
    model->estimated_latency_cycles = 0u;
    model->container_bound = 0u;
    model->weights_bytes = 0u;
    model->params_bytes = 0u;
    model->activation_bytes = 0u;
    model->ecblob_bytes = 0u;
    model->weights_offset = 0u;
    model->params_offset = 0u;
    model->activation_offset = 0u;
    model->ecblob_offset = 0u;
    model->container_total_bytes = 0u;
    model->container_input_count = 0u;
    model->container_output_count = 0u;
    model->container_path[0] = '\0';
    model->extent_base = NULL;
    model->extent_offset = 0u;
    model->extent_bytes = 0u;

    /* Match RA8P1's store-backed loader: map the model through the mounted
     * data store, then let this port validate its own RAW/TN6P header.  This
     * keeps model loading independent of the shell-facing /data namespace. */
    rc = npu_model_map_source(vfs_path, &source, &source_len);
    if (rc != TIKU_NPU_OK) {
        return rc;
    }
    if (source_len < TIKU_NPU_MODEL_CONTAINER_BASE_BYTES) {
        return TIKU_NPU_ERR_HEADER;
    }

    if (npu_model_rd32(source + NPU_CONTAINER_OFF_MAGIC) !=
            TIKU_NPU_MODEL_CONTAINER_MAGIC ||
        npu_model_rd16(source + NPU_CONTAINER_OFF_VERSION) !=
            TIKU_NPU_MODEL_CONTAINER_VERSION) {
        return TIKU_NPU_ERR_HEADER;
    }
    header_bytes = npu_model_rd16(source + NPU_CONTAINER_OFF_HEADER);
    input_count = npu_model_rd16(source + NPU_CONTAINER_OFF_INPUTS);
    output_count = npu_model_rd16(source + NPU_CONTAINER_OFF_OUTPUTS);
    desc_bytes = npu_model_rd16(source + NPU_CONTAINER_OFF_DESCS);
    expected_desc_bytes = ((size_t)input_count + (size_t)output_count) *
                          TIKU_NPU_MODEL_CONTAINER_TENSOR_BYTES;
    expected_header_bytes = TIKU_NPU_MODEL_CONTAINER_BASE_BYTES +
                            expected_desc_bytes;
    if (input_count > TIKU_NPU_MODEL_MAX_IO ||
        output_count > TIKU_NPU_MODEL_MAX_IO ||
        expected_desc_bytes > UINT16_MAX ||
        desc_bytes != (uint16_t)expected_desc_bytes ||
        header_bytes != (uint16_t)expected_header_bytes ||
        npu_model_rd16(source + 34u) != 0u ||
        npu_model_rd32(source + NPU_CONTAINER_OFF_FLAGS) !=
            TIKU_NPU_MODEL_CONTAINER_F_NONE) {
        return TIKU_NPU_ERR_HEADER;
    }

    if (source_len < expected_header_bytes) {
        return TIKU_NPU_ERR_HEADER;
    }

    sizes[0] = npu_model_rd32(source + NPU_CONTAINER_OFF_WEIGHTS);
    sizes[1] = npu_model_rd32(source + NPU_CONTAINER_OFF_PARAMS);
    sizes[2] = npu_model_rd32(source + NPU_CONTAINER_OFF_ACTIVATION);
    sizes[3] = npu_model_rd32(source + NPU_CONTAINER_OFF_ECBLOB);
    offsets[0] = npu_model_rd32(source + NPU_CONTAINER_OFF_WEIGHTS_AT);
    offsets[1] = npu_model_rd32(source + NPU_CONTAINER_OFF_PARAMS_AT);
    offsets[2] = npu_model_rd32(source + NPU_CONTAINER_OFF_ACTIVATION_AT);
    offsets[3] = npu_model_rd32(source + NPU_CONTAINER_OFF_ECBLOB_AT);
    total = npu_model_rd32(source + NPU_CONTAINER_OFF_TOTAL);
    if (total < header_bytes ||
        sizes[0] > TIKU_NPU_MODEL_MAX_WEIGHTS_BYTES ||
        sizes[1] > TIKU_NPU_MODEL_MAX_PARAMS_BYTES ||
        sizes[2] > TIKU_NPU_MODEL_MAX_ACTIVATION_BYTES ||
        sizes[3] > TIKU_NPU_MODEL_MAX_ECBLOB_BYTES ||
        model->slot_capacity == 0u ||
        (uint64_t)sizes[0] + sizes[1] + sizes[2] + sizes[3] >
            (uint64_t)model->slot_capacity) {
        return TIKU_NPU_ERR_CAPACITY;
    }
    for (i = 0u; i < 4u; i++) {
        if (sizes[i] != 0u &&
            !npu_model_range_ok(offsets[i], sizes[i], header_bytes, total)) {
            return TIKU_NPU_ERR_HEADER;
        }
    }

    memset(inputs, 0, sizeof inputs);
    memset(outputs, 0, sizeof outputs);
    desc_at = NPU_CONTAINER_DESC_AT;
    for (i = 0u; i < input_count; i++) {
        if (!npu_model_tensor_decode(source + desc_at, &inputs[i])) {
            return TIKU_NPU_ERR_HEADER;
        }
        desc_at += TIKU_NPU_MODEL_CONTAINER_TENSOR_BYTES;
    }
    for (i = 0u; i < output_count; i++) {
        if (!npu_model_tensor_decode(source + desc_at, &outputs[i])) {
            return TIKU_NPU_ERR_HEADER;
        }
        desc_at += TIKU_NPU_MODEL_CONTAINER_TENSOR_BYTES;
    }

    model->weights_bytes = sizes[0];
    model->params_bytes = sizes[1];
    model->activation_bytes = sizes[2];
    model->ecblob_bytes = sizes[3];
    model->weights_offset = offsets[0];
    model->params_offset = offsets[1];
    model->activation_offset = offsets[2];
    model->ecblob_offset = offsets[3];
    model->container_total_bytes = total;
    model->container_input_count = input_count;
    model->container_output_count = output_count;
    memcpy(model->container_inputs, inputs, sizeof inputs);
    memcpy(model->container_outputs, outputs, sizeof outputs);
    memcpy(model->container_path, vfs_path, strlen(vfs_path) + 1u);
    model->container_bound = 1u;
    return TIKU_NPU_OK;
}

int tiku_npu_model_load(tiku_npu_model_t *model)
{
    const uint8_t *source;
    size_t source_len;
    tiku_arena_t arena;
    uint8_t *destination;
    uintptr_t npu_start;
    size_t copied;
    int rc;

    if (model == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (model->container_loaded || npu_loaded_model != NULL) {
        return TIKU_NPU_ERR_BUSY;
    }
    if (!model->container_bound || model->container_path[0] == '\0') {
        return TIKU_NPU_ERR_STATE;
    }
    if (!npu_initialized || epoch_controller == NULL) {
        return TIKU_NPU_ERR_STATE;
    }
    if (npu_active_model != NULL ||
        (TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) &
         STM32N6_EPOCHCTRL_CTRL_RUNNING) != 0U) {
        return TIKU_NPU_ERR_BUSY;
    }

    rc = npu_model_map_source(model->container_path, &source, &source_len);
    if (rc != TIKU_NPU_OK) {
        return rc;
    }
    if (source_len < (size_t)model->container_total_bytes) {
        return TIKU_NPU_ERR_HEADER;
    }
    if (source_len < TIKU_NPU_MODEL_CONTAINER_BASE_BYTES ||
        npu_model_rd32(source + NPU_CONTAINER_OFF_MAGIC) !=
            TIKU_NPU_MODEL_CONTAINER_MAGIC ||
        npu_model_rd16(source + NPU_CONTAINER_OFF_VERSION) !=
            TIKU_NPU_MODEL_CONTAINER_VERSION ||
        npu_model_rd32(source + NPU_CONTAINER_OFF_TOTAL) !=
            model->container_total_bytes) {
        return TIKU_NPU_ERR_HEADER;
    }
    if (model->container_total_bytes == 0u ||
        model->container_total_bytes > model->slot_capacity ||
        model->ecblob_bytes == 0u ||
        (model->ecblob_bytes & 7u) != 0u ||
        (model->ecblob_offset & 7u) != 0u) {
        return TIKU_NPU_ERR_CAPACITY;
    }

    rc = (int)tiku_tier_arena_create(
        &arena, TIKU_MEM_NPU,
        (tiku_mem_arch_size_t)model->container_total_bytes, 0u);
    if (rc != TIKU_MEM_OK) {
        return TIKU_NPU_ERR_CAPACITY;
    }
    destination = (uint8_t *)arena.buf;

    /* Copy in bounded pieces so the loader's transfer working set is fixed;
     * the source remains the VFS mapping and no section is relocated. */
    copied = 0u;
    while (copied < (size_t)model->container_total_bytes) {
        size_t chunk = (size_t)model->container_total_bytes - copied;
        if (chunk > 256u) {
            chunk = 256u;
        }
        memcpy(destination + copied, source + copied, chunk);
        copied += chunk;
    }
    tiku_stm32n6_dcache_clean(destination, copied);

    npu_start = (uintptr_t)&__tier_npu_start;
    if ((uintptr_t)destination < npu_start ||
        (uintptr_t)destination - npu_start > UINT32_MAX) {
        (void)tiku_tier_npu_reset();
        return TIKU_NPU_ERR_CAPACITY;
    }

    model->extent_base = destination;
    model->extent_offset = (uint32_t)((uintptr_t)destination - npu_start);
    model->extent_bytes = model->container_total_bytes;
    model->magic = TIKU_NPU_MODEL_MAGIC;
    model->abi_version = TIKU_NPU_MODEL_ABI_VERSION;
    model->flags = TIKU_NPU_MODEL_F_FIXED;
    model->epoch_blob = (const uint64_t *)(destination +
                                           model->ecblob_offset);
    model->epoch_blob_bytes = model->ecblob_bytes;
    model->input_count = model->container_input_count;
    model->output_count = model->container_output_count;
    model->input_addresses = NULL;
    model->output_addresses = NULL;
    model->input_sizes = NULL;
    model->output_sizes = NULL;
    model->estimated_latency_cycles = 0u;
    model->container_loaded = 1u;
    npu_loaded_model = model;
    return TIKU_NPU_OK;
}

int tiku_npu_model_unload(tiku_npu_model_t *model)
{
    if (model == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (!model->container_loaded) {
        return TIKU_NPU_OK;
    }
    if (npu_active_model == model ||
        (TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) &
         STM32N6_EPOCHCTRL_CTRL_RUNNING) != 0U) {
        return TIKU_NPU_ERR_BUSY;
    }
    if (npu_loaded_model != model) {
        return TIKU_NPU_ERR_STATE;
    }
    if (tiku_tier_npu_reset() != TIKU_MEM_OK) {
        return TIKU_NPU_ERR_STATE;
    }

    model->magic = 0u;
    model->abi_version = 0u;
    model->flags = 0u;
    model->epoch_blob = NULL;
    model->epoch_blob_bytes = 0u;
    model->input_count = 0u;
    model->output_count = 0u;
    model->input_addresses = NULL;
    model->output_addresses = NULL;
    model->input_sizes = NULL;
    model->output_sizes = NULL;
    model->estimated_latency_cycles = 0u;
    model->extent_base = NULL;
    model->extent_offset = 0u;
    model->extent_bytes = 0u;
    model->container_loaded = 0u;
    npu_loaded_model = NULL;
    return TIKU_NPU_OK;
}

int tiku_npu_model_io(const tiku_npu_model_t *model,
                      tiku_npu_model_io_t *out)
{
    if (model == NULL || out == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (!model->container_bound) {
        return TIKU_NPU_ERR_STATE;
    }
    out->input_count = model->container_input_count;
    out->output_count = model->container_output_count;
    memcpy(out->inputs, model->container_inputs, sizeof out->inputs);
    memcpy(out->outputs, model->container_outputs, sizeof out->outputs);
    return TIKU_NPU_OK;
}

int tiku_npu_run(const tiku_npu_model_t *model,
                 const void *in[], void *out[])
{
    struct tiku_process *owner = TIKU_PROCESS_CURRENT();

    if (!npu_model_is_valid(model, in, out)) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (!npu_initialized || epoch_controller == NULL || owner == NULL) {
        return TIKU_NPU_ERR_STATE;
    }
    if (npu_active_model != NULL ||
        (TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) &
         STM32N6_EPOCHCTRL_CTRL_RUNNING) != 0U) {
        return TIKU_NPU_ERR_BUSY;
    }

    /* A prior DONE may still be latched at the controller boundary. Clear it
     * before publishing the new owner, so an old IRQ can never be attributed
     * to this submission. No wait or poll is performed here. */
    npu_epoch_ack();
    npu_run_owner = owner;
    npu_active_model = model;
    npu_epoch_trigger_cycle = npu_cycles();
    TIKU_REG32(STM32N6_EPOCHCTRL_ADDR) =
        (uint32_t)(uintptr_t)model->epoch_blob;
    /* EN is a command pulse. AUTOCLR leaves the controller ready for the next
     * submission after the epoch stops; neither bit waits for the NPU. */
    TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) =
        STM32N6_EPOCHCTRL_CTRL_AUTOCLR | STM32N6_EPOCHCTRL_CTRL_EN;
    __asm__ volatile ("dsb" ::: "memory");
    return TIKU_NPU_OK;
}

static void npu_epoch_ack(void)
{
    uint32_t status = TIKU_REG32(STM32N6_EPOCHCTRL_IRQ);

    if (status != 0U) {
        TIKU_REG32(STM32N6_EPOCHCTRL_IRQ) = status;
    }
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTCLR) = STM32N6_NPU_INTCTRL_EPOCH0_INT;
    TIKU_REG32(STM32N6_NVIC_ICPR(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static int npu_epoch_reset(void)
{
    uint32_t start = npu_cycles();

    TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_CLR;
    while ((TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) & STM32N6_EPOCHCTRL_CTRL_CLR) != 0U) {
        if ((uint32_t)(npu_cycles() - start) > STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
            return TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
        }
    }
    TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_CONFCLR;
    while ((TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) & STM32N6_EPOCHCTRL_CTRL_CONFCLR) != 0U) {
        if ((uint32_t)(npu_cycles() - start) > STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
            return TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
        }
    }
    TIKU_REG32(STM32N6_EPOCHCTRL_ADDR) =
        (uint32_t)(uintptr_t)npu_epoch_selftest_blob;
    TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = 0U;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    return TIKU_NPU_EPOCH_TEST_OK;
}

static int npu_epoch_wait_stopped(uint32_t *stopped)
{
    uint32_t start = npu_cycles();

    *stopped = 0U;
    while ((TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) & STM32N6_EPOCHCTRL_CTRL_RUNNING) != 0U) {
        if ((uint32_t)(npu_cycles() - start) > STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
            return TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
        }
    }
    *stopped = 1U;
    return TIKU_NPU_EPOCH_TEST_OK;
}

static void npu_epoch_irq_configure(void)
{
    npu_cycle_counter_enable();
    TIKU_REG32(STM32N6_NPU_INTCTRL_CTRL) = STM32N6_NPU_INTCTRL_CTRL_CLR;
    /* INTORMSK bits are masks: unmask only EPOCHCTRL0 completion. */
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTORMSK0) =
        ~STM32N6_NPU_INTCTRL_EPOCH0_INT;
    TIKU_REG32(STM32N6_NPU_INTCTRL_INTANDMSK0) = 0xffffffffUL;
    TIKU_REG32(STM32N6_NPU_INTCTRL_CTRL) = STM32N6_NPU_INTCTRL_CTRL_EN;
    /* STM32N6 CMSIS defines four implemented priority bits. */
    TIKU_REG8(STM32N6_NVIC_IPR(STM32N6_IRQ_NPU_END_OF_EPOCH)) =
        (uint8_t)(STM32N6_NPU_IRQ_PRIORITY << 4);
    TIKU_REG32(STM32N6_NVIC_ICPR(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    TIKU_REG32(STM32N6_NVIC_ISER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static int extents_overlap(uintptr_t a_start, uintptr_t a_end,
                           uintptr_t b_start, uintptr_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

static int npu_extent_is_valid(void)
{
    const uintptr_t axisram_start = (uintptr_t)&__axisram_start;
    const uintptr_t sram_start = (uintptr_t)&__tier_sram_start;
    const uintptr_t sram_end = (uintptr_t)&__tier_sram_end;
    const uintptr_t npu_start = (uintptr_t)&__tier_npu_start;
    const uintptr_t npu_end = (uintptr_t)&__tier_npu_end;
    const uintptr_t nvm_start = (uintptr_t)&__uninit_start;
    const uintptr_t nvm_end = (uintptr_t)&__uninit_end;

    if (npu_start < axisram_start || npu_end <= npu_start ||
        (npu_end - npu_start) != (uintptr_t)TIKU_TIER_NPU_SIZE ||
        ((npu_start | npu_end) & 31U) != 0U) {
        return 0;
    }
    if (sram_end < sram_start ||
        extents_overlap(sram_start, sram_end, npu_start, npu_end) ||
        extents_overlap(nvm_start, nvm_end, npu_start, npu_end)) {
        return 0;
    }
    return 1;
}

uint32_t tiku_npu_clock_readback(void)
{
    return TIKU_REG32(STM32N6_NPU_RCC_AHB5ENR);
}

int tiku_npu_power_enabled(void)
{
    return tiku_stm32n6_sram_npu_powered();
}

uintptr_t tiku_npu_epoch_controller_base(void)
{
    return (uintptr_t)epoch_controller;
}

int tiku_npu_init(void)
{
    tiku_mem_err_t attach_result;

    if (npu_initialized) {
        return TIKU_NPU_INIT_ALREADY;
    }

    /* The linker and this runtime check are intentionally redundant: the
     * latter catches a mismatched object/linker configuration before the
     * extent can be handed to the allocator. */
    if (!npu_extent_is_valid()) {
        return TIKU_NPU_INIT_ERR_CONFIG;
    }

    /* AXISRAM3..6 are the NPU memory banks.  tiku_sram_init() powers them up
     * before tiku_mem_init(); this is the boot-time power-domain readback. */
    if (!tiku_npu_power_enabled()) {
        return TIKU_NPU_INIT_ERR_POWER;
    }

    /* AHB5ENR.NPU is the Neural-ART/epoch-controller clock gate. */
    TIKU_REG32(STM32N6_NPU_RCC_AHB5ENR) |= STM32N6_RCC_AHB5ENR_NPU;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    npu_clock_readback = tiku_npu_clock_readback();
    if ((npu_clock_readback & STM32N6_RCC_AHB5ENR_NPU) == 0U) {
        return TIKU_NPU_INIT_ERR_CLOCK;
    }

    npu_reset_release();
    npu_cacheaxi_enable();
    npu_rif_configure();
    npu_internal_clock_enable();

    /* On this memory-mapped target, mapping establishes the typed view of the
     * fixed register window. */
    epoch_controller = (volatile tiku_npu_epoch_controller_t *)(uintptr_t)
        STM32N6_EPOCHCTRL_BASE;
    (void)epoch_controller->control;
    (void)epoch_controller->version;

    attach_result = tiku_tier_attach_npu(
        (void *)(uintptr_t)&__tier_npu_start,
        (tiku_mem_arch_size_t)TIKU_TIER_NPU_SIZE);
    if (attach_result != TIKU_MEM_OK) {
        return TIKU_NPU_INIT_ERR_ALLOC;
    }

    npu_epoch_irq_configure();

    npu_initialized = 1U;
    return TIKU_NPU_INIT_OK;
}

int tiku_npu_epoch_irq_selftest(uint32_t iterations,
                                tiku_npu_epoch_test_result_t *result)
{
    int rc = TIKU_NPU_EPOCH_TEST_OK;

    if (result == NULL || iterations == 0U) {
        return TIKU_NPU_EPOCH_TEST_ERR_ARG;
    }
    if (!npu_initialized || epoch_controller == NULL) {
        return TIKU_NPU_EPOCH_TEST_ERR_INIT;
    }

    *result = (tiku_npu_epoch_test_result_t){0};
    result->requested = iterations;
    result->timeout_cycles = STM32N6_NPU_IRQ_TIMEOUT_CYCLES;
    result->nvic_priority = STM32N6_NPU_IRQ_PRIORITY;
    result->latency_min_cycles = 0xffffffffUL;

    npu_cycle_counter_enable();
    npu_epoch_ack();

    for (uint32_t i = 0U; i < iterations; i++) {
        uint32_t before;
        uint32_t start;
        uint32_t stopped;

        npu_epoch_ack();
        rc = npu_epoch_reset();
        if (rc != TIKU_NPU_EPOCH_TEST_OK) {
            break;
        }

        before = npu_epoch_irq_count;
        start = npu_cycles();
        npu_epoch_trigger_cycle = start;
        TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_EN;
        __asm__ volatile ("dsb" ::: "memory");

        rc = npu_epoch_wait_stopped(&stopped);
        (void)stopped;
        if (rc != TIKU_NPU_EPOCH_TEST_OK) {
            break;
        }
        while (npu_epoch_irq_count < before + 1U) {
            if ((uint32_t)(npu_cycles() - start) >
                STM32N6_NPU_IRQ_TIMEOUT_CYCLES) {
                rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
                break;
            }
        }
        if (rc != TIKU_NPU_EPOCH_TEST_OK) {
            break;
        }
        {
            uint32_t quiet_start = npu_cycles();
            while ((uint32_t)(npu_cycles() - quiet_start) <
                   STM32N6_NPU_IRQ_SETTLE_CYCLES) {
                if (npu_epoch_irq_count != before + 1U) {
                    break;
                }
            }
        }
        if (npu_epoch_irq_count != before + 1U) {
            result->extra++;
        }
        result->completed++;
        if (npu_epoch_last_latency_cycles < result->latency_min_cycles) {
            result->latency_min_cycles = npu_epoch_last_latency_cycles;
        }
        if (npu_epoch_last_latency_cycles > result->latency_max_cycles) {
            result->latency_max_cycles = npu_epoch_last_latency_cycles;
        }
        result->latency_total_cycles += npu_epoch_last_latency_cycles;
    }

    result->missed = iterations - result->completed;
    if (result->completed != iterations || result->extra != 0U) {
        rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
    }

    /* Negative path: the epoch must execute and stop, but the disabled NVIC
     * line must not change the ISR counter. BC is only an execution check;
     * completion is observed exclusively through the ISR counter. */
    TIKU_REG32(STM32N6_NVIC_ICER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
    result->negative_before = npu_epoch_irq_count;
    if (npu_epoch_reset() == TIKU_NPU_EPOCH_TEST_OK) {
        uint32_t negative_bc_before = TIKU_REG32(STM32N6_EPOCHCTRL_BC);
        TIKU_REG32(STM32N6_EPOCHCTRL_CTRL) = STM32N6_EPOCHCTRL_CTRL_EN;
        __asm__ volatile ("dsb" ::: "memory");
        uint32_t stopped = 0U;
        if (npu_epoch_wait_stopped(&stopped) == TIKU_NPU_EPOCH_TEST_OK) {
            result->negative_epoch_ran =
                (stopped != 0U &&
                 TIKU_REG32(STM32N6_EPOCHCTRL_BC) != negative_bc_before);
        }
    } else {
        rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
    }
    result->negative_after = npu_epoch_irq_count;
    if (result->negative_after != result->negative_before ||
        result->negative_epoch_ran == 0U) {
        result->extra++;
        rc = TIKU_NPU_EPOCH_TEST_ERR_TIMEOUT;
    }
    npu_epoch_ack();
    TIKU_REG32(STM32N6_NVIC_ISER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32U)) =
        (1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32U));
    __asm__ volatile ("dsb\n\tisb" ::: "memory");

    return rc;
}
