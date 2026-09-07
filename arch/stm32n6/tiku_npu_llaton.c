/*
 * TikuOS LL-ATON relocatable-model adapter.
 *
 * Model locations are intentionally distinct:
 *   - drivers/stm32n6/npu/models/ : optional build-embedded compiler output
 *   - /data/npu/                   : deployed combined network_rel.bin files
 *   - tests/npu/fixtures/          : malformed/fault-injection fixtures only
 *
 * The source image is always mapped read-only through the /data store. COPY
 * installation and all runtime relocation work happen in the linker-owned
 * NPU tier. No TN6P envelope, EC blob address, or manual relocation is used.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <interfaces/npu/tiku_npu.h>
#include <kernel/fs/tiku_model.h>
#include <kernel/memory/tiku_mem.h>
#include <kernel/process/tiku_process.h>
#include <kernel/vfs/tree/tiku_vfs_tree_data.h>

#include "tiku_device_select.h"
#include "tiku_npu_llaton.h"
#include "tiku_stm32n6_regs.h"

#include "ll_aton_reloc_network.h"
#include "ll_aton_rt_user_api.h"
#include "ll_aton_version.h"

extern uint8_t __tier_npu_start;
extern uint8_t __tier_npu_end;

typedef struct {
    ll_aton_reloc_info info;
    ll_aton_reloc_config config;
    NN_Instance_TypeDef instance;
    tiku_arena_t arena;
    const uint8_t *source;
    size_t source_len;
    int last_error;
    struct tiku_process *owner;
    uint8_t runtime_ready;
    uint8_t running;
} tiku_llaton_state_t;

static tiku_llaton_state_t ll_state;
static tiku_npu_model_t *ll_model;
static void (*volatile ll_irq_handler)(void);
static uint8_t ll_worker_started;
static uint8_t ll_runtime_initialized;

void SCB_InvalidateICache_by_Addr(void *addr, int32_t size)
{
    (void)addr;
    (void)size;
    TIKU_REG32(STM32N6_SCB_ICIALLU) = 0u;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static void tiku_llaton_worker_step(void);
static void tiku_llaton_finish(LL_ATON_RT_RetValues_t ret);

TIKU_PROCESS(npu_llaton_worker, "npu-llaton");

/* The process declaration macro expands a static thread function followed by
 * the process object. Define its body here, after the object declaration. */
static PT_THREAD(tiku_process_thread_npu_llaton_worker(struct pt *process_pt,
                                                       tiku_event_t ev,
                                                       tiku_event_data_t data))
{
    (void)data;
    TIKU_PROCESS_BEGIN();
    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
        if (ev == TIKU_EVENT_NPU_WAKE) {
            tiku_llaton_worker_step();
        }
    }
    TIKU_PROCESS_END();
}

static int range_ok(uintptr_t base, size_t length, uintptr_t limit)
{
    return base <= limit && length <= (size_t)(limit - base);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rel_off(uint32_t value)
{
    return value & UINT32_C(0x0fffffff);
}

static int image_string(const uint8_t *image, size_t length, uint32_t value)
{
    size_t at = (size_t)rel_off(value);

    if (at >= length) return 0;
    while (at < length && image[at] != 0u) at++;
    return at < length;
}

/* Header geometry mirrors ST's ai_reloc_bin_hdr. This preflight is needed
 * because ll_aton_reloc_get_info() receives a pointer, not a length. */
static int image_preflight(const uint8_t *image, size_t length)
{
    uintptr_t desc;
    unsigned i;

    if (image == NULL || length < 104u ||
        (((uintptr_t)image) & 3u) != 0u || rd32(image) != AI_RELOC_MAGIC) {
        return 0;
    }
    uint32_t data_data = rel_off(rd32(image + 16u));
    uint32_t data_end = rel_off(rd32(image + 20u));
    uint32_t bss_end = rel_off(rd32(image + 24u));
    uint32_t got_end = rel_off(rd32(image + 32u));
    uint32_t rel_start = rel_off(rd32(image + 36u));
    uint32_t rel_end = rel_off(rd32(image + 40u));
    uint32_t params_start = rel_off(rd32(image + 44u));
    uint32_t params_offset = rel_off(rd32(image + 48u));
    uint32_t ctx = rel_off(rd32(image + 100u));

    if ((data_data & 3u) != 0u || data_data > length ||
        data_end < data_data || data_end > length ||
        bss_end < data_end || bss_end > length ||
        got_end < data_data || got_end > length ||
        rel_end < got_end || rel_end > length ||
        (params_offset != 0u && params_offset >= length) ||
        ctx > length - data_data || data_data + ctx > length - 72u) {
        return 0;
    }

    uint32_t ctx_at = data_data + ctx;
    /* The runtime dereferences these context strings during get_info and
     * install.  Validate their file-relative offsets before entering ST code,
     * whose public get_info API has no length parameter. */
    if (!image_string(image, length, rd32(image + ctx_at + 20u)) ||
        !image_string(image, length, rd32(image + ctx_at + 36u))) {
        return 0;
    }
    /* Entry points are offsets into the relocatable image.  Optional entries
     * may be zero, but a nonzero entry must remain inside the code image. */
    for (uint32_t vec = 0u; vec < 13u; vec++) {
        uint32_t entry = rel_off(rd32(image + 52u + vec * 4u));
        /* Function offsets carry the Thumb bit in bit 0. */
        if (entry != 0u && ((entry & ~1u) >= rel_end ||
                            ((entry & ~1u) & 3u) != 0u)) return 0;
    }

    /* Each relocation word is itself an encoded address of the word that the
     * ST installer will rewrite.  Validate the site list before install: a
     * damaged site otherwise becomes an unchecked dereference inside the
     * vendor routine, which has no image-length argument. */
    if (rel_end < rel_start || rel_start > length || rel_end > length) return 0;
    for (desc = (uintptr_t)image + rel_start;
         desc < (uintptr_t)image + rel_end; desc += 4u) {
        uint32_t site = rd32((const uint8_t *)desc);
        uint32_t site_off = rel_off(site);
        uint32_t site_kind = site & UINT32_C(0xf0000000);
        if ((site_kind != UINT32_C(0x20000000) &&
             site_kind != UINT32_C(0x40000000)) ||
            site_off > length - 4u) return -1;
    }

    /* The descriptor array is at data_data + params_start and is terminated
     * by a zero flags/name record. Bound the same ten entries as ST's loader. */
    desc = (uintptr_t)image + data_data + params_start;
    if (params_start > length - data_data || desc < (uintptr_t)image ||
        (uintptr_t)(desc - (uintptr_t)image) > length - 16u) {
        return 0;
    }
    for (i = 0u; i < 10u; i++) {
        size_t at = (size_t)(desc - (uintptr_t)image) + i * 20u;
        if (at > length - 20u) return 0;
        if (rd32(image + at) == 0u && rd32(image + at + 4u) == 0u) break;
    }
    return i < 10u;
}

static int map_source(const char *path, const uint8_t **source, size_t *length)
{
    static const char prefix[] = "/data/npu/";
    tiku_tfs_t *fs;
    tiku_model_t mapped;

    if (path == NULL || source == NULL || length == NULL ||
        strncmp(path, prefix, sizeof(prefix) - 1u) != 0) {
        return TIKU_NPU_ERR_NOT_FOUND;
    }
    fs = tiku_vfs_tree_data_store();
    if (fs == NULL || tiku_model_open(fs, path + 6u, &mapped) != TIKU_MODEL_OK) {
        return TIKU_NPU_ERR_NOT_FOUND;
    }
    *source = mapped.base;
    *length = mapped.len;
    return TIKU_NPU_OK;
}

static int tensor_type(Buffer_DataType_TypeDef type, uint8_t *out)
{
    switch (type) {
    case DataType_INT8: *out = TIKU_NPU_TENSOR_INT8; return 1;
    case DataType_UINT8: *out = TIKU_NPU_TENSOR_UINT8; return 1;
    case DataType_INT16: *out = TIKU_NPU_TENSOR_INT16; return 1;
    case DataType_INT32: *out = TIKU_NPU_TENSOR_INT32; return 1;
    case DataType_FLOAT: *out = TIKU_NPU_TENSOR_FLOAT32; return 1;
    default: return 0;
    }
}

static int convert_tensor(const LL_Buffer_InfoTypeDef *src,
                          tiku_npu_tensor_t *dst)
{
    if (src == NULL || src->name == NULL || src->mem_shape == NULL ||
        src->mem_ndims > TIKU_NPU_MODEL_MAX_RANK ||
        !tensor_type(src->type, &dst->type) ||
        LL_Buffer_len(src) == 0u) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    uint16_t rank = src->mem_ndims;
    dst->rank = (uint8_t)rank;
    dst->flags = 0u;
    dst->zero_point = 0;
    for (uint16_t i = 0u; i < TIKU_NPU_MODEL_MAX_RANK; i++) {
        dst->shape[i] = (i < rank) ? src->mem_shape[i] : 0u;
        if (i < rank && dst->shape[i] == 0u) return TIKU_NPU_ERR_ARGUMENT;
    }
    if (src->offset != NULL && (src->type == DataType_INT8 ||
                                src->type == DataType_UINT8)) {
        dst->zero_point = src->offset[0];
    }
    return TIKU_NPU_OK;
}

static int cache_io(const NN_Instance_TypeDef *instance,
                    tiku_npu_model_io_t *io)
{
    memset(io, 0, sizeof(*io));
    for (uint16_t i = 0u; i < TIKU_NPU_MODEL_MAX_IO; i++) {
        const LL_Buffer_InfoTypeDef *b =
            ll_aton_reloc_get_input_buffers_info(instance, (int32_t)i);
        if (b == NULL) break;
        if (convert_tensor(b, &io->inputs[io->input_count]) != TIKU_NPU_OK)
            return TIKU_NPU_ERR_ARGUMENT;
        io->input_count++;
    }
    for (uint16_t i = 0u; i < TIKU_NPU_MODEL_MAX_IO; i++) {
        const LL_Buffer_InfoTypeDef *b =
            ll_aton_reloc_get_output_buffers_info(instance, (int32_t)i);
        if (b == NULL) break;
        if (convert_tensor(b, &io->outputs[io->output_count]) != TIKU_NPU_OK)
            return TIKU_NPU_ERR_ARGUMENT;
        io->output_count++;
    }
    return (io->input_count != 0u && io->output_count != 0u)
               ? TIKU_NPU_OK : TIKU_NPU_ERR_ARGUMENT;
}

static int validate_pools(const uint8_t *source, size_t source_len)
{
    uintptr_t npu_begin = (uintptr_t)&__tier_npu_start;
    uintptr_t npu_end = (uintptr_t)&__tier_npu_end;
    int i;

    for (i = 0; i < 10; i++) {
        ll_aton_reloc_mem_pool_desc *d =
            ll_aton_reloc_get_mem_pool_desc((uintptr_t)source, i);
        uint32_t type;
        if (d == NULL) break;
        type = AI_RELOC_MPOOL_GET_TYPE(d->flags);
        if (!image_string(source, source_len, (uint32_t)(uintptr_t)d->name)) {
            return TIKU_NPU_ERR_HEADER;
        }
        if (d->size > UINT32_MAX - 31u ||
            (d->foff > source_len || d->size > source_len - d->foff)) {
            return TIKU_NPU_ERR_HEADER;
        }
        if (AI_RELOC_MPOOL_IS_RELOC(d->flags)) {
            /* Initial adapter has no external activation or parameter pool. */
            if (AI_RELOC_MPOOL_GET_ID(d->flags) != 0u) return TIKU_NPU_ERR_ARGUMENT;
        } else if (type == AI_RELOC_MPOOL_TYPE_COPY ||
                   type == AI_RELOC_MPOOL_TYPE_RESET) {
            uintptr_t dst = (uintptr_t)d->dst;
            size_t span = (size_t)((d->size + 31u) & ~31u);
            int in_npu = dst >= npu_begin && range_ok(dst, span, npu_end);
            int in_board_nvm =
                dst >= (uintptr_t)TIKU_DEVICE_FRAM_START &&
                dst <= (uintptr_t)TIKU_DEVICE_FRAM_END &&
                span <= (size_t)TIKU_DEVICE_FRAM_END - dst + 1u;

            /* RESET is activation memory and must never target the NOR.
             * COPY may target the board's declared mapped model pool; the
             * board memory bring-up owns that mapping before NPU init. */
            if ((!in_npu && (type == AI_RELOC_MPOOL_TYPE_RESET)) ||
                (!in_npu && !in_board_nvm)) return TIKU_NPU_ERR_ARGUMENT;
        }
    }
    return i == 10 ? TIKU_NPU_ERR_HEADER : TIKU_NPU_OK;
}

/* Helper function to handle runtime loading errors */
static void handle_load_runtime_error(tiku_npu_model_t *model, uint8_t installed) {
    if (installed) {
        LL_ATON_RT_DeInit_Network(&ll_state.instance);
    }
    (void)tiku_tier_npu_reset();
    memset(&ll_state.instance, 0, sizeof(ll_state.instance));
    model->container_loaded = 0u;
    model->backend_state = &ll_state;
    ll_state.running = 0u;
    ll_state.owner = NULL;
}

int tiku_npu_model_bind(tiku_npu_model_t *model, const char *path)
{
    const uint8_t *source;
    size_t length;
    uint32_t capacity;
    int rc;

    if (model == NULL || path == NULL) {
        TIKU_PRINTF("Model or path is NULL\n");
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (model->container_loaded || ll_model != NULL) {
        TIKU_PRINTF("Model is already loaded or ll_model is not NULL\n");
        return TIKU_NPU_ERR_BUSY;
    }
    if (strlen(path) >= TIKU_NPU_MODEL_PATH_MAX) {
        TIKU_PRINTF("Model path is too long\n");
        return TIKU_NPU_ERR_ARGUMENT;
    }
    rc = map_source(path, &source, &length);
    if (rc != TIKU_NPU_OK) {
        TIKU_PRINTF("Failed to map source with error: %d\n", rc);
        return rc;
    }
    rc = image_preflight(source, length);
    if (rc < 0) {
        TIKU_PRINTF("Failed to preflight image with error: %d\n", rc);
        return TIKU_NPU_ERR_RELOCATION;
    }
    if (rc == 0) {
        TIKU_PRINTF("Invalid image header\n");
        return TIKU_NPU_ERR_HEADER;
    }
    memset(&ll_state, 0, sizeof(ll_state));
    ll_state.runtime_ready = ll_runtime_initialized;
    capacity = model->slot_capacity != 0u ? model->slot_capacity :
               (uint32_t)TIKU_TIER_NPU_SIZE;
    rc = ll_aton_reloc_get_info((uintptr_t)source, &ll_state.info);
    if (rc != 0 ||
        AI_RELOC_RT_GET_MAJOR(ll_state.info.variant) != AI_RELOC_RT_VERSION_MAJOR ||
        AI_RELOC_RT_GET_MINOR(ll_state.info.variant) != AI_RELOC_RT_VERSION_MINOR ||
        AI_RELOC_RT_GET_CPUID(ll_state.info.variant) != AI_RELOC_ARM_CORTEX_M55 ||
        AI_RELOC_RT_GET_COMPILER(ll_state.info.variant) != AI_RELOC_TOOLCHAIN_GCC_EMBEDDED ||
        AI_RELOC_RT_DBG_INFO(ll_state.info.variant) == 0u ||
        AI_RELOC_RT_ASYNC_MODE(ll_state.info.variant) == 0u ||
        ll_state.info.rt_version !=
            ((uint32_t)LL_ATON_VERSION_MAJOR << 24 |
             (uint32_t)LL_ATON_VERSION_MINOR << 16 |
             (uint32_t)LL_ATON_VERSION_MICRO << 8) ||
        ll_state.info.params_off == 0u ||
        ll_state.info.ext_ram_sz != 0u || ll_state.info.rt_ram_copy == 0u ||
        ll_state.info.rt_ram_copy > capacity) {
        TIKU_PRINTF("Invalid model info\n");
        return (rc == 0 && ll_state.info.rt_ram_copy > capacity)
                   ? TIKU_NPU_ERR_CAPACITY : TIKU_NPU_ERR_HEADER;
    }
    rc = validate_pools(source, length);
    if (rc != TIKU_NPU_OK) {
        TIKU_PRINTF("Failed to validate pools with error: %d\n", rc);
        return rc;
    }
    ll_state.source = source;
    ll_state.source_len = length;
    strncpy(model->container_path, path, sizeof(model->container_path) - 1u);
    model->container_path[sizeof(model->container_path) - 1u] = '\0';
    model->container_bound = 1u;
    model->container_loaded = 0u;
    model->backend_state = &ll_state;
    return TIKU_NPU_OK;
}

int tiku_npu_model_load(tiku_npu_model_t *model)
{
    int rc;
    uint8_t installed = 0u;

    if (model == NULL) {
        TIKU_PRINTF("Model is NULL\n");
        return TIKU_NPU_ERR_ARGUMENT;
    } 
    if (!model->container_bound || model->backend_state != &ll_state) {
        TIKU_PRINTF("Model is not bound or backend state mismatch\n");
        return TIKU_NPU_ERR_STATE;
    }
    if (model->container_loaded || ll_model != NULL) {
        TIKU_PRINTF("Model is already loaded or another model is loaded\n");
        return TIKU_NPU_ERR_BUSY;
    }
    if (!ll_state.runtime_ready) {
        TIKU_PRINTF("Runtime is not ready\n");
        return TIKU_NPU_ERR_STATE;
    }
    rc = (int)tiku_tier_arena_create(&ll_state.arena, TIKU_MEM_NPU,
                                     ll_state.info.rt_ram_copy, 0u);
    if (rc != TIKU_MEM_OK) {
        TIKU_PRINTF("Failed to create arena with error: %d\n", rc);
        return TIKU_NPU_ERR_CAPACITY;
    }
    memset(&ll_state.instance, 0, sizeof(ll_state.instance));
    ll_state.config = (ll_aton_reloc_config){
        .exec_ram_addr = (uintptr_t)ll_state.arena.buf,
        .exec_ram_size = (uint32_t)ll_state.arena.capacity,
        .ext_ram_addr = 0u,
        .ext_ram_size = 0u,
        .ext_param_addr = 0u,
        .mode = AI_RELOC_RT_LOAD_MODE_COPY,
    };
    rc = ll_aton_reloc_install((uintptr_t)ll_state.source, &ll_state.config,
                               &ll_state.instance);
    if (rc != 0 || ll_aton_reloc_is_valid(&ll_state.instance) != 1) {
        TIKU_PRINTF("Failed to install relocatable model with error: %d\n", rc);
        handle_load_runtime_error(model, installed);
        return TIKU_NPU_ERR_RUNTIME;
    }
    installed = 1u;
    LL_ATON_RT_Init_Network(&ll_state.instance);
    if (ll_aton_reloc_is_valid(&ll_state.instance) != 1 ||
        cache_io(&ll_state.instance, &model->io) != TIKU_NPU_OK) {
        TIKU_PRINTF("Failed to cache IO or validate instance\n");
        handle_load_runtime_error(model, installed);
        return TIKU_NPU_ERR_RUNTIME;
    }

    model->container_loaded = 1u;
    ll_model = model;
    ll_state.last_error = TIKU_NPU_OK;
    ll_state.running = 0u;
    return TIKU_NPU_OK;
}

int tiku_npu_model_unload(tiku_npu_model_t *model)
{
    if (model == NULL) return TIKU_NPU_ERR_ARGUMENT;
    if (!model->container_loaded) return TIKU_NPU_OK;
    if (ll_model != model || ll_state.running) return TIKU_NPU_ERR_BUSY;
    LL_ATON_RT_DeInit_Network(&ll_state.instance);
    if (tiku_tier_npu_reset() != TIKU_MEM_OK) return TIKU_NPU_ERR_STATE;
    memset(&ll_state.instance, 0, sizeof(ll_state.instance));
    memset(&model->io, 0, sizeof(model->io));
    model->container_loaded = 0u;
    ll_model = NULL;
    return TIKU_NPU_OK;
}

int tiku_npu_model_io(const tiku_npu_model_t *model, tiku_npu_model_io_t *out)
{
    if (model == NULL || out == NULL) return TIKU_NPU_ERR_ARGUMENT;
    if (!model->container_loaded) return TIKU_NPU_ERR_STATE;
    *out = model->io;
    return TIKU_NPU_OK;
}

int tiku_npu_model_last_error(const tiku_npu_model_t *model)
{
    if (model == NULL || model->backend_state != &ll_state) return TIKU_NPU_ERR_ARGUMENT;
    return ll_state.last_error;
}

static void tiku_llaton_worker_step(void)
{
    LL_ATON_RT_RetValues_t ret;
    if (ll_model == NULL || !ll_state.running) return;
    ret = LL_ATON_RT_RunEpochBlock(&ll_state.instance);
    if (ret == LL_ATON_RT_WFE) return;
    if (ret == LL_ATON_RT_NO_WFE) {
        (void)tiku_process_post(&npu_llaton_worker, TIKU_EVENT_NPU_WAKE,
                                (tiku_event_data_t)ll_model);
        return;
    }
    tiku_llaton_finish(ret);
}

static void tiku_llaton_finish(LL_ATON_RT_RetValues_t ret)
{
    if (ll_model == NULL || !ll_state.running) return;
    if (ret == LL_ATON_RT_DONE) {
        LL_ATON_RT_Reset_Network(&ll_state.instance);
        ll_state.last_error = TIKU_NPU_OK;
    } else {
        ll_state.last_error = TIKU_NPU_ERR_RUNTIME;
    }
    ll_state.running = 0u;
    if (ll_state.owner != NULL) {
        (void)tiku_process_post(ll_state.owner, TIKU_EVENT_NPU_DONE,
                                (tiku_event_data_t)ll_model);
    }
    ll_state.owner = NULL;
}

int tiku_npu_run(const tiku_npu_model_t *model, const void *in[], void *out[])
{
    uint16_t i;
    LL_ATON_RT_RetValues_t ret;
    struct tiku_process *owner = TIKU_PROCESS_CURRENT();

    if (model == NULL || in == NULL || out == NULL || model != ll_model ||
        !model->container_loaded || owner == NULL || ll_state.running)
        return TIKU_NPU_ERR_STATE;
    if (!ll_worker_started) {
        tiku_process_start(&npu_llaton_worker, NULL);
        ll_worker_started = 1u;
    }
    for (i = 0u; i < model->io.input_count; i++) {
        const LL_Buffer_InfoTypeDef *desc =
            ll_aton_reloc_get_input_buffers_info(&ll_state.instance, i);
        if (in[i] == NULL || desc == NULL ||
            ((uintptr_t)in[i] & 3u) != 0u || LL_ATON_Set_User_Input_Buffer(
                &ll_state.instance, i, (void *)in[i],
                LL_Buffer_len(desc)) !=
                LL_ATON_User_IO_NOERROR) return TIKU_NPU_ERR_ARGUMENT;
    }
    for (i = 0u; i < model->io.output_count; i++) {
        const LL_Buffer_InfoTypeDef *desc =
            ll_aton_reloc_get_output_buffers_info(&ll_state.instance, i);
        if (out[i] == NULL || desc == NULL ||
            ((uintptr_t)out[i] & 3u) != 0u || LL_ATON_Set_User_Output_Buffer(
                &ll_state.instance, i, out[i],
                LL_Buffer_len(desc)) !=
                LL_ATON_User_IO_NOERROR) return TIKU_NPU_ERR_ARGUMENT;
    }
    ll_state.owner = owner;
    ll_state.running = 1u;
    ret = LL_ATON_RT_RunEpochBlock(&ll_state.instance);
    if (ret == LL_ATON_RT_DONE) {
        tiku_llaton_finish(ret);
    } else if (ret != LL_ATON_RT_WFE && ret != LL_ATON_RT_NO_WFE) {
        ll_state.running = 0u;
        ll_state.last_error = TIKU_NPU_ERR_RUNTIME;
        ll_state.owner = NULL;
        return TIKU_NPU_ERR_RUNTIME;
    } else if (ret == LL_ATON_RT_NO_WFE) {
        (void)tiku_process_post(&npu_llaton_worker, TIKU_EVENT_NPU_WAKE,
                                (tiku_event_data_t)model);
    }
    return TIKU_NPU_OK;
}

void tiku_llaton_osal_install_irq(uint32_t line, void (*handler)(void))
{
    if (line == 0u) ll_irq_handler = handler;
}

void tiku_llaton_osal_remove_irq(uint32_t line)
{
    if (line == 0u) ll_irq_handler = NULL;
}

void tiku_llaton_osal_enable_irq(uint32_t line)
{
    if (line == 0u) {
        /* LL-ATON owns the ATON interrupt controller. TikuOS owns only the
         * Cortex-M routing and NVIC state for the ATON standard line. */
        TIKU_REG8(STM32N6_NVIC_IPR(STM32N6_IRQ_NPU_END_OF_EPOCH)) =
            (uint8_t)(1U << 4);
        TIKU_REG32(STM32N6_NVIC_ICPR(STM32N6_IRQ_NPU_END_OF_EPOCH / 32u)) =
            1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32u);
#if defined(CPU_IN_SECURE_STATE)
        /* A secure image must keep the IRQ on the secure vector table. */
        TIKU_REG32(STM32N6_NVIC_ITNS(STM32N6_IRQ_NPU_END_OF_EPOCH / 32u)) &=
            ~(1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32u));
#endif
        __asm__ volatile ("dsb\n\tisb" ::: "memory");
        TIKU_REG32(STM32N6_NVIC_ISER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32u)) =
            1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32u);
    }
}

void tiku_llaton_osal_disable_irq(uint32_t line)
{
    if (line == 0u) {
        TIKU_REG32(STM32N6_NVIC_ICER(STM32N6_IRQ_NPU_END_OF_EPOCH / 32u)) =
            1UL << (STM32N6_IRQ_NPU_END_OF_EPOCH % 32u);
    }
}

void tiku_llaton_osal_init(void) { }
void tiku_llaton_osal_deinit(void) { ll_irq_handler = NULL; }
void tiku_llaton_osal_enter_cs(void) { tiku_llaton_osal_disable_irq(0u); }
void tiku_llaton_osal_exit_cs(void) { tiku_llaton_osal_enable_irq(0u); }
void tiku_llaton_osal_signal_event(void) { }

void tiku_npu_llaton_irq_bridge(void)
{
    if (ll_irq_handler != NULL) ll_irq_handler();
    if (ll_model != NULL && ll_state.running) {
        (void)tiku_process_post(&npu_llaton_worker, TIKU_EVENT_NPU_WAKE,
                                (tiku_event_data_t)ll_model);
    }
}

int tiku_npu_llaton_runtime_init(void)
{
    if (!ll_runtime_initialized) {
        LL_ATON_RT_RuntimeInit();
        ll_runtime_initialized = 1u;
    }
    ll_state.runtime_ready = ll_runtime_initialized;
    return TIKU_NPU_OK;
}
