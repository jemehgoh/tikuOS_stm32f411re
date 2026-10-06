/*
 * TikuOS LL-ATON relocatable-model adapter.
 *
 * Model locations are intentionally distinct:
 *   - drivers/stm32n6/npu/models/ : optional build-embedded compiler output
 *   - the N6 raw model slot       : deployed combined network_rel.bin files
 *   - tests/npu/fixtures/          : malformed/fault-injection fixtures only
 *
 * The source image is always mapped read-only through the OSPI model store. COPY
 * installation and all runtime relocation work happen in the linker-owned
 * NPU tier. No TN6P envelope, EC blob address, or manual relocation is used.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <interfaces/npu/tiku_npu.h>
#include <kernel/memory/tiku_mem.h>
#include <kernel/process/tiku_process.h>

#include "tiku_device_select.h"
#include "tiku_n6_model_store.h"
#include "tiku_npu_llaton.h"
#include "tiku_ospi_arch.h"
#include "tiku_stm32n6_regs.h"

#include "ll_aton_reloc_network.h"
#include "ll_aton_rt_user_api.h"
#include "ll_aton_version.h"

extern uint8_t __tier_npu_start;
extern uint8_t __tier_npu_end;

/* No external writable RAM is configured for the current STM32N6 product.
 * A board that adds one must provide all four values and initialize the
 * region before the NPU backend starts.  Keeping the capability here avoids
 * silently using the read-only OSPI model window or consuming general SRAM. */
#ifndef TIKU_STM32N6_EXT_RAM_BASE
#define TIKU_STM32N6_EXT_RAM_BASE 0u
#endif
#ifndef TIKU_STM32N6_EXT_RAM_SIZE
#define TIKU_STM32N6_EXT_RAM_SIZE 0u
#endif
#ifndef TIKU_STM32N6_EXT_RAM_WRITABLE
#define TIKU_STM32N6_EXT_RAM_WRITABLE 0
#endif
#ifndef TIKU_STM32N6_EXT_RAM_NPU_VISIBLE
#define TIKU_STM32N6_EXT_RAM_NPU_VISIBLE 0
#endif

typedef struct {
    uintptr_t addr;
    size_t capacity;
    uint8_t active;
} tiku_llaton_ext_ram_t;

typedef struct {
    ll_aton_reloc_info info;
    ll_aton_reloc_config config;
    NN_Instance_TypeDef instance;
    tiku_arena_t exec_arena;
    tiku_llaton_ext_ram_t ext_ram;
    tiku_n6_model_image_t image;
    uintptr_t file_ptr;
    uintptr_t file_params_ptr;
    size_t source_len;
    size_t params_len;
    int last_error;
    struct tiku_process *owner;
    uint8_t runtime_ready;
    uint8_t instance_installed;
    uint8_t network_initialized;
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
static void tiku_llaton_abort_submission(void);
static uint8_t tiku_llaton_post_wake(const tiku_npu_model_t *model,
                                     uint8_t from_irq);
static int validate_pools(const tiku_n6_model_image_t *image,
                          const ll_aton_reloc_info *rt);
static int release_runtime_resources(void);
static int handle_load_runtime_error(tiku_npu_model_t *model,
                                     int original_error);

static void clear_model_inspection(void)
{
    uint8_t runtime_ready = ll_state.runtime_ready;

    memset(&ll_state.info, 0, sizeof(ll_state.info));
    ll_state.file_ptr = 0u;
    ll_state.file_params_ptr = 0u;
    ll_state.source_len = 0u;
    ll_state.params_len = 0u;
    memset(&ll_state.image, 0, sizeof(ll_state.image));
    ll_state.runtime_ready = runtime_ready;
}

TIKU_PROCESS(npu_llaton_worker, "npu-llaton");

/* The process declaration macro expands a static thread function followed by
 * the process object. Define its body here, after the object declaration. */
static PT_THREAD(tiku_process_thread_npu_llaton_worker(struct pt *process_pt,
                                                       tiku_event_t ev,
                                                       tiku_event_data_t data))
{
    TIKU_PROCESS_BEGIN();
    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
        if (ev == TIKU_EVENT_NPU_WAKE) {
            if (tiku_event_npu_model(ev, data) == ll_model &&
                ll_model != NULL && ll_state.running) {
                tiku_llaton_worker_step();
            } else {
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
                ll_run_trace.ignored_wake_events++;
#endif
            }
        }
    }
    TIKU_PROCESS_END();
}

static int range_ok(uintptr_t base, size_t length, uintptr_t limit)
{
    return base <= limit && length <= (size_t)(limit - base);
}

static int round_up_size(size_t value, size_t alignment, size_t *out)
{
    size_t remainder;

    if (out == NULL || alignment == 0u) return 0;
    remainder = value % alignment;
    if (remainder != 0u && value > SIZE_MAX - (alignment - remainder)) {
        return 0;
    }
    *out = value + (remainder == 0u ? 0u : alignment - remainder);
    return 1;
}

static int mapped_range_contains(uintptr_t base, size_t length,
                                 uintptr_t address, size_t span)
{
    if (address < base || address - base > length) return 0;
    return span <= length - (size_t)(address - base);
}

static int resolve_parameter_span(uintptr_t file_ptr, size_t file_bytes,
                                  uintptr_t file_params_ptr,
                                  size_t params_bytes, size_t params_offset,
                                  uintptr_t *params_ptr,
                                  size_t *params_length)
{
    if (params_ptr == NULL || params_length == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    *params_ptr = 0u;
    *params_length = 0u;

    if (params_offset != 0u) {
        if (file_ptr == 0u || (file_ptr & 3u) != 0u ||
            params_offset > file_bytes ||
            params_offset > (size_t)(UINTPTR_MAX - file_ptr) ||
            params_offset == file_bytes) {
            return TIKU_NPU_ERR_HEADER;
        }
        *params_ptr = file_ptr + (uintptr_t)params_offset;
        *params_length = file_bytes - params_offset;
        return TIKU_NPU_OK;
    }

    if (file_params_ptr == 0u || (file_params_ptr & 3u) != 0u ||
        params_bytes == 0u ||
        params_bytes > (size_t)(UINTPTR_MAX - file_params_ptr)) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    *params_ptr = file_params_ptr;
    *params_length = params_bytes;
    return TIKU_NPU_OK;
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

#define RELOC_IMAGE_HEADER_BYTES 104u
#define RELOC_IMAGE_CTX_BYTES    60u
#define RELOC_IMAGE_DESC_BYTES   20u
#define RELOC_IMAGE_MAX_DESCS    10u
#define RELOC_ADDR_CLASS_MASK    UINT32_C(0xf0000000)
#define RELOC_FLASH_CLASS        UINT32_C(0x20000000)
#define RELOC_RAM_CLASS          UINT32_C(0x40000000)

static int image_span_ok(size_t offset, size_t span, size_t length)
{
    return offset <= length && span <= length - offset;
}

static int image_offset(uint32_t encoded, uint32_t address_class,
                        size_t length, size_t *out)
{
    size_t offset;

    if ((encoded & RELOC_ADDR_CLASS_MASK) != address_class) return 0;
    offset = (size_t)rel_off(encoded);
    if ((offset & 3u) != 0u || !image_span_ok(offset, 0u, length)) return 0;
    *out = offset;
    return 1;
}

static int image_string(const uint8_t *image, size_t length, uint32_t value,
                        uint32_t address_class)
{
    size_t at = (size_t)rel_off(value);

    if ((value & RELOC_ADDR_CLASS_MASK) != address_class || at >= length) {
        return 0;
    }
    while (at < length && image[at] != 0u) at++;
    return at < length;
}

/* Header geometry mirrors ST's ai_reloc_bin_hdr. This preflight is needed
 * because ll_aton_reloc_get_info() receives a pointer, not a length. */
static int image_preflight(const uint8_t *image, size_t length,
                           uintptr_t params_ptr, size_t params_length)
{
    size_t data_start;
    size_t data_end;
    size_t data_data;
    size_t bss_start;
    size_t bss_end;
    size_t got_start;
    size_t got_end;
    size_t rel_start;
    size_t rel_end;
    size_t params_start;
    size_t params_offset;
    uintptr_t params_base;
    size_t ctx;
    size_t ctx_at;
    size_t desc_at;
    size_t params_length_bound;
    unsigned i;

    if (image == NULL || length < RELOC_IMAGE_HEADER_BYTES ||
        (((uintptr_t)image) & 3u) != 0u || rd32(image) != AI_RELOC_MAGIC) {
        return 0;
    }

    if (!image_offset(rd32(image + 8u), RELOC_RAM_CLASS, length,
                      &data_start) ||
        !image_offset(rd32(image + 12u), RELOC_RAM_CLASS, length,
                      &data_end) ||
        !image_offset(rd32(image + 16u), RELOC_FLASH_CLASS, length,
                      &data_data) ||
        !image_offset(rd32(image + 20u), RELOC_RAM_CLASS, length,
                      &bss_start) ||
        !image_offset(rd32(image + 24u), RELOC_RAM_CLASS, length,
                      &bss_end) ||
        !image_offset(rd32(image + 28u), RELOC_RAM_CLASS, length,
                      &got_start) ||
        !image_offset(rd32(image + 32u), RELOC_RAM_CLASS, length,
                      &got_end) ||
        !image_offset(rd32(image + 36u), RELOC_FLASH_CLASS, length,
                      &rel_start) ||
        !image_offset(rd32(image + 40u), RELOC_FLASH_CLASS, length,
                      &rel_end) ||
        !image_offset(rd32(image + 44u), RELOC_RAM_CLASS, length,
                      &params_start) ||
        !image_offset(rd32(image + 100u), RELOC_RAM_CLASS, length, &ctx)) {
        return 0;
    }

    /* These offsets live in separate address spaces in ST's image format:
     * data/bss/GOT/parameter-table offsets are RAM-relative, while the
     * read-only and relocation offsets are file-relative FLASH offsets. */
    if (data_start > data_end || data_end > bss_start ||
        bss_start > bss_end || data_start > got_start ||
        got_start > got_end || got_end > bss_start ||
        data_data < RELOC_IMAGE_HEADER_BYTES || data_data > rel_start ||
        rel_start > rel_end || !image_span_ok(rel_start,
                                               rel_end - rel_start,
                                               length)) {
        return 0;
    }

    /* params_offset is a raw file offset, not one of the encoded section
     * addresses.  A zero value denotes a split parameter image. */
    {
        uint32_t encoded_params_offset = rd32(image + 48u);
        int params_rc;

        if ((encoded_params_offset & RELOC_ADDR_CLASS_MASK) != 0u) {
            return 0;
        }
        params_offset = (size_t)encoded_params_offset;
        if ((params_offset & 3u) != 0u) return 0;
        params_rc = resolve_parameter_span((uintptr_t)image, length,
                                            params_ptr, params_length,
                                            params_offset, &params_base,
                                            &params_length_bound);
        if (params_rc != TIKU_NPU_OK) return 0;
    }

    if (!image_span_ok(data_data, ctx, length) ||
        !image_span_ok(data_data + ctx, RELOC_IMAGE_CTX_BYTES, length)) {
        return 0;
    }
    ctx_at = data_data + ctx;
    /* The runtime dereferences these context strings during get_info and
     * install.  Validate their file-relative offsets before entering ST code,
     * whose public get_info API has no length parameter. */
    if (!image_string(image, length, rd32(image + ctx_at + 20u),
                      RELOC_FLASH_CLASS) ||
        !image_string(image, length, rd32(image + ctx_at + 36u),
                      RELOC_FLASH_CLASS)) {
        return 0;
    }
    /* Entry points are offsets into the relocatable image.  Optional entries
     * may be zero, but a nonzero entry must remain inside the code image. */
    for (uint32_t vec = 0u; vec < 13u; vec++) {
        uint32_t encoded_entry = rd32(image + 52u + vec * 4u);
        uint32_t entry;

        if (encoded_entry == 0u) continue;
        if ((encoded_entry & RELOC_ADDR_CLASS_MASK) != RELOC_FLASH_CLASS) {
            return 0;
        }
        entry = rel_off(encoded_entry);
        /* Function offsets carry the Thumb bit in bit 0. */
        if ((entry & ~1u) == 0u || (entry & ~1u) >= rel_end ||
            ((entry & ~1u) & 3u) != 0u) {
            return 0;
        }
    }

    /* Each relocation word is itself an encoded address of the word that the
     * ST installer will rewrite.  Validate the site list before install: a
     * damaged site otherwise becomes an unchecked dereference inside the
     * vendor routine, which has no image-length argument. */
    if (rel_start < data_data || rel_end < rel_start ||
        ((rel_end - rel_start) & 3u) != 0u) {
        return 0;
    }
    for (size_t at = rel_start; at <= rel_end - 4u; at += 4u) {
        uint32_t site = rd32(image + at);
        uint32_t site_off = rel_off(site);
        uint32_t site_kind = site & RELOC_ADDR_CLASS_MASK;
        if ((site_kind != UINT32_C(0x20000000) &&
             site_kind != UINT32_C(0x40000000)) ||
            (site_off & 3u) != 0u || !image_span_ok(site_off, 4u, length)) {
            return -1;
        }
    }

    /* The descriptor array is at data_data + params_start and is terminated
     * by a zero flags/name record. Bound the same ten entries as ST's loader,
     * and reject a half-zero record before the vendor helper sees it. */
    if (params_start > length - data_data ||
        data_data + params_start > rel_start) {
        return 0;
    }
    desc_at = data_data + params_start;
    if (!image_span_ok(desc_at, RELOC_IMAGE_DESC_BYTES, length)) {
        return 0;
    }
    for (i = 0u; i < RELOC_IMAGE_MAX_DESCS; i++) {
        size_t at;
        uint32_t name;
        uint32_t flags;

        if ((size_t)i > (SIZE_MAX - desc_at) / RELOC_IMAGE_DESC_BYTES) {
            return 0;
        }
        at = desc_at + (size_t)i * RELOC_IMAGE_DESC_BYTES;
        if (!image_span_ok(at, RELOC_IMAGE_DESC_BYTES, length) ||
            at > rel_start || RELOC_IMAGE_DESC_BYTES > rel_start - at) {
            return 0;
        }
        name = rd32(image + at);
        flags = rd32(image + at + 4u);
        if (name == 0u && flags == 0u) break;
        if (name == 0u || flags == 0u ||
            !image_string(image, length, name, RELOC_FLASH_CLASS)) {
            return 0;
        }

        uint32_t pool_type = AI_RELOC_MPOOL_GET_TYPE(flags);
        uint32_t pool_dtype = AI_RELOC_MPOOL_GET_DTYPE(flags);
        uint32_t pool_attr = AI_RELOC_MPOOL_GET_ATTR(flags);
        uint32_t pool_id = AI_RELOC_MPOOL_GET_ID(flags);
        uint32_t foff = rd32(image + at + 8u);
        uint32_t size = rd32(image + at + 16u);
        size_t rounded_size;
        int has_initializer;

        if (pool_type < AI_RELOC_MPOOL_TYPE_RELOC ||
            pool_type > AI_RELOC_MPOOL_TYPE_RESET ||
            pool_dtype < AI_RELOC_MPOOL_DTYPE_PARAM ||
            pool_dtype > AI_RELOC_MPOOL_DTYPE_MIXED ||
            (pool_attr & ~(AI_RELOC_MPOOL_DATTR_READ |
                           AI_RELOC_MPOOL_DATTR_WRITE |
                           AI_RELOC_MPOOL_DATTR_CACHEABLE)) != 0u ||
            (pool_type == AI_RELOC_MPOOL_TYPE_RELOC && pool_id > 1u) ||
            size > UINT32_MAX - 7u) {
            return 0;
        }
        if (!round_up_size((size_t)size, 8u, &rounded_size)) return 0;
        has_initializer = pool_type == AI_RELOC_MPOOL_TYPE_COPY ||
                          (pool_type == AI_RELOC_MPOOL_TYPE_RELOC &&
                           pool_dtype == AI_RELOC_MPOOL_DTYPE_PARAM);
        if (has_initializer &&
            ((size_t)foff > params_length_bound ||
             rounded_size > params_length_bound - (size_t)foff)) {
            return 0;
        }
    }
    return i < RELOC_IMAGE_MAX_DESCS;
}

#if defined(TIKU_NPU_PREFLIGHT_TEST_ENABLE)
int tiku_npu_llaton_test_image_preflight(const void *image, size_t length,
                                         uintptr_t params_ptr,
                                         size_t params_length)
{
    return image_preflight((const uint8_t *)image, length, params_ptr,
                           params_length);
}
#endif

#if defined(TIKU_NPU_POOL_TEST_ENABLE)
int tiku_npu_llaton_test_validate_pools(const void *image, size_t length,
                                        uintptr_t params_ptr,
                                        size_t params_length,
                                        uint32_t params_offset,
                                        uint32_t ext_ram_size)
{
    tiku_n6_model_image_t model_image;
    ll_aton_reloc_info info;

    memset(&model_image, 0, sizeof(model_image));
    memset(&info, 0, sizeof(info));
    model_image.file_ptr = (uintptr_t)image;
    model_image.file_params_ptr = params_ptr;
    model_image.file_bytes = length;
    model_image.params_bytes = params_length;
    info.params_off = params_offset;
    info.ext_ram_sz = ext_ram_size;
    return validate_pools(&model_image, &info);
}
#endif

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
        if (i < rank && dst->shape[i] == 0u) { 
            return TIKU_NPU_ERR_ARGUMENT;
        }
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
    uintptr_t ram_begin = (uintptr_t)TIKU_DEVICE_RAM_START;
    uintptr_t ram_end = ram_begin + (uintptr_t)TIKU_DEVICE_RAM_SIZE;
    int i;

    for (i = 0; i < 10; i++) {
        ll_aton_reloc_mem_pool_desc *d =
            ll_aton_reloc_get_mem_pool_desc((uintptr_t)source, i);
        uint32_t type;
        if (d == NULL) break;
        type = AI_RELOC_MPOOL_GET_TYPE(d->flags);
        /* Descriptor fields are 32-bit encoded addresses even though the C
         * view exposes name as a pointer.  Read the on-media word directly;
         * casting d->name through uintptr_t is wrong on a 64-bit host and
         * also confuses the encoded address with the mapped pointer. */
        if ((uintptr_t)d < (uintptr_t)source ||
            (uintptr_t)d - (uintptr_t)source > source_len - 20u ||
            !image_string(source, source_len,
                          rd32((const uint8_t *)(uintptr_t)d))) {
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
            int in_runtime_ram = dst >= ram_begin &&
                                 range_ok(dst, span, ram_end);

            /* RESET is activation memory and remains restricted to the
             * linker-owned NPU tier.  COPY must target writable runtime RAM:
             * the XSPI memory-mapped NOR contains the model source, but its
             * address window is not a writable runtime destination for the
             * memcpy performed by LL-ATON. */
            if ((!in_npu && (type == AI_RELOC_MPOOL_TYPE_RESET)) ||
                (!in_runtime_ram && type == AI_RELOC_MPOOL_TYPE_COPY)) {
                return TIKU_NPU_ERR_ARGUMENT;
            }
        } else {
            return TIKU_NPU_ERR_HEADER;
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
    tiku_n6_model_store_set_model_bound(1);
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
