/*
 * TikuOS LL-ATON relocatable-model adapter.
 *
 * Model locations are intentionally distinct:
 *   - drivers/stm32n6/npu/models/ : optional build-embedded compiler output
 *   - the N6 raw model slot       : deployed combined network_rel.bin files
 *   - tests/npu/fixtures/          : malformed/fault-injection fixtures only
 *
 * The source image is always mapped read-only through the OSPI model store. COPY
 * installation and all writable runtime relocation work happen in the
 * boot-owned NPU root arena. No TN6P envelope, EC blob address, or manual
 * relocation is used.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <interfaces/npu/tiku_npu.h>
#include <kernel/memory/tiku_mem.h>
#include <kernel/process/tiku_process.h>

#include "tiku_n6_model_store.h"
#include "tiku_npu_llaton.h"
#include "tiku_npu_mem.h"
#include "tiku_stm32n6_regs.h"

#include "ll_aton_reloc_network.h"
#include "ll_aton_rt_user_api.h"
#include "ll_aton_version.h"

typedef struct {
    uintptr_t start;
    uintptr_t end;
    uint32_t flags;
} tiku_llaton_interval_t;

#define TIKU_LLATON_MAX_FIXED_REGIONS 10u

typedef struct {
    tiku_llaton_interval_t fixed[TIKU_LLATON_MAX_FIXED_REGIONS];
    uint8_t fixed_count;
} tiku_llaton_layout_t;

typedef struct {
    ll_aton_reloc_info info;
    ll_aton_reloc_config config;
    NN_Instance_TypeDef instance;
    tiku_arena_t exec_arena;
    tiku_arena_t ext_arena;
    tiku_arena_t fixed_arenas[TIKU_LLATON_MAX_FIXED_REGIONS];
    uint8_t fixed_arena_count;
    tiku_llaton_layout_t layout;
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

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
static tiku_npu_llaton_run_trace_t ll_run_trace;
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE) || \
    defined(TIKU_NPU_POOL_TEST_ENABLE) || \
    defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
static unsigned ll_test_ext_ram_reserve_attempts;
static unsigned ll_test_install_calls;
#endif

#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
static int ll_test_load_failure;
static unsigned ll_test_network_deinit_calls;
#endif

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
                          const ll_aton_reloc_info *rt,
                          tiku_llaton_layout_t *layout);
static int release_runtime_resources(void);
static int reserve_runtime_resources(void);
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

static int intervals_overlap(uintptr_t a_start, uintptr_t a_end,
                             uintptr_t b_start, uintptr_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

static void sort_fixed_intervals(tiku_llaton_layout_t *layout)
{
    unsigned i;

    if (layout == NULL) return;
    for (i = 1u; i < layout->fixed_count; i++) {
        tiku_llaton_interval_t entry = layout->fixed[i];
        unsigned at = i;

        while (at != 0u && layout->fixed[at - 1u].start > entry.start) {
            layout->fixed[at] = layout->fixed[at - 1u];
            at--;
        }
        layout->fixed[at] = entry;
    }
}

static int root_interval(uintptr_t *start, uintptr_t *end)
{
    const tiku_arena_t *root = tiku_stm32n6_npu_mem_root();
    uintptr_t root_start;

    if (root == NULL || start == NULL || end == NULL) {
        return 0;
    }
    root_start = (uintptr_t)root->buf;
    if (root->capacity > UINTPTR_MAX - root_start) {
        return 0;
    }
    *start = root_start;
    *end = root_start + (uintptr_t)root->capacity;
    return *end > *start;
}

static int validate_runtime_intervals(const tiku_llaton_layout_t *layout,
                                      uintptr_t exec_addr, size_t exec_size,
                                      uintptr_t ext_addr, size_t ext_size)
{
    uintptr_t root_start;
    uintptr_t root_end;
    uintptr_t exec_end;
    uintptr_t ext_end = ext_addr;
    size_t exec_span;
    size_t ext_span = 0u;
    unsigned i;

    if (layout == NULL || exec_size == 0u ||
        !root_interval(&root_start, &root_end) ||
        !round_up_size(exec_size, 32u, &exec_span) ||
        exec_addr < root_start || exec_addr > root_end ||
        exec_span > (size_t)(root_end - exec_addr)) {
        return TIKU_NPU_ERR_CAPACITY;
    }
    exec_end = exec_addr + (uintptr_t)exec_span;

    if (ext_size != 0u) {
        if (ext_addr == 0u ||
            !round_up_size(ext_size, 32u, &ext_span) ||
            ext_addr < root_start || ext_addr > root_end ||
            ext_span > (size_t)(root_end - ext_addr)) {
            return TIKU_NPU_ERR_CAPACITY;
        }
        ext_end = ext_addr + (uintptr_t)ext_span;
        if (intervals_overlap(exec_addr, exec_end, ext_addr, ext_end)) {
            return TIKU_NPU_ERR_ARGUMENT;
        }
    }

    for (i = 0u; i < layout->fixed_count; i++) {
        const tiku_llaton_interval_t *fixed = &layout->fixed[i];
        if (intervals_overlap(exec_addr, exec_end,
                              fixed->start, fixed->end) ||
            (ext_size != 0u && intervals_overlap(ext_addr, ext_end,
                                                 fixed->start, fixed->end))) {
            return TIKU_NPU_ERR_ARGUMENT;
        }
    }
    return TIKU_NPU_OK;
}

static int validate_dynamic_plan(const tiku_llaton_layout_t *layout,
                                 const ll_aton_reloc_info *rt)
{
    uintptr_t root_start;
    uintptr_t root_end;
    uintptr_t ext_addr = 0u;
    uintptr_t cursor;
    size_t exec_span;
    size_t ext_span = 0u;
    int rc;

    if (layout == NULL || rt == NULL || rt->rt_ram_copy == 0u ||
        !root_interval(&root_start, &root_end) ||
        !round_up_size((size_t)rt->rt_ram_copy, 32u, &exec_span) ||
        exec_span > (size_t)(root_end - root_start)) {
        return TIKU_NPU_ERR_CAPACITY;
    }
    if (rt->ext_ram_sz != 0u) {
        ext_addr = root_start + (uintptr_t)exec_span;
        if (!round_up_size((size_t)rt->ext_ram_sz, 32u, &ext_span)) {
            return TIKU_NPU_ERR_CAPACITY;
        }
    }
    rc = validate_runtime_intervals(layout, root_start,
                                    (size_t)rt->rt_ram_copy, ext_addr,
                                    (size_t)rt->ext_ram_sz);
    if (rc != TIKU_NPU_OK) return rc;

    cursor = root_start + (uintptr_t)exec_span + (uintptr_t)ext_span;
    for (unsigned i = 0u; i < layout->fixed_count; i++) {
        if (layout->fixed[i].start < cursor) {
            return TIKU_NPU_ERR_ARGUMENT;
        }
        if (layout->fixed[i].end > root_end) {
            return TIKU_NPU_ERR_CAPACITY;
        }
        cursor = layout->fixed[i].end;
    }
    return TIKU_NPU_OK;
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

typedef struct {
    uint32_t flags;
    uint32_t foff;
    uint32_t dst;
    uint32_t size;
} tiku_llaton_pool_descriptor_t;

typedef struct {
    tiku_llaton_pool_descriptor_t entries[RELOC_IMAGE_MAX_DESCS];
    uint8_t count;
} tiku_llaton_pool_descriptors_t;

static int image_span_ok(size_t offset, size_t span, size_t length)
{
    return offset <= length && span <= length - offset;
}

static int image_offset(uint32_t encoded, uint32_t address_class,
                        size_t length, size_t *out)
{
    size_t offset;

    if ((encoded & RELOC_ADDR_CLASS_MASK) != address_class) {
        return 0;
    }
    offset = (size_t)rel_off(encoded);
    if ((offset & 3u) != 0u || !image_span_ok(offset, 0u, length)) {
        return 0;
    }
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
    while (at < length && image[at] != 0u) {
        at++;
    }
    return at < length;
}

static int validate_pool_descriptors(
    const uint8_t *image, size_t length, size_t data_data,
    size_t params_start, size_t rel_start, size_t params_length,
    tiku_llaton_pool_descriptors_t *out)
{
    size_t descriptor_table;
    unsigned i;

    if (out != NULL) memset(out, 0, sizeof(*out));
    if (image == NULL || data_data > length ||
        params_start > length - data_data) {
        return TIKU_NPU_ERR_HEADER;
    }
    descriptor_table = data_data + params_start;
    if (descriptor_table > rel_start ||
        !image_span_ok(descriptor_table, RELOC_IMAGE_DESC_BYTES, length) ||
        RELOC_IMAGE_DESC_BYTES > rel_start - descriptor_table) {
        return TIKU_NPU_ERR_HEADER;
    }

    for (i = 0u; i < RELOC_IMAGE_MAX_DESCS; i++) {
        size_t at;
        size_t source_span;
        uint32_t name;
        uint32_t flags;
        uint32_t type;
        uint32_t dtype;
        uint32_t attr;
        uint32_t foff;
        uint32_t size;
        int has_initializer;

        if ((size_t)i >
            (SIZE_MAX - descriptor_table) / RELOC_IMAGE_DESC_BYTES) {
            return TIKU_NPU_ERR_HEADER;
        }
        at = descriptor_table + (size_t)i * RELOC_IMAGE_DESC_BYTES;
        if (!image_span_ok(at, RELOC_IMAGE_DESC_BYTES, length) ||
            at > rel_start || RELOC_IMAGE_DESC_BYTES > rel_start - at) {
            return TIKU_NPU_ERR_HEADER;
        }

        name = rd32(image + at);
        flags = rd32(image + at + 4u);
        if (name == 0u && flags == 0u) break;
        if (name == 0u || flags == 0u ||
            !image_string(image, length, name, RELOC_FLASH_CLASS)) {
            return TIKU_NPU_ERR_HEADER;
        }

        type = AI_RELOC_MPOOL_GET_TYPE(flags);
        dtype = AI_RELOC_MPOOL_GET_DTYPE(flags);
        attr = AI_RELOC_MPOOL_GET_ATTR(flags);
        foff = rd32(image + at + 8u);
        size = rd32(image + at + 16u);
        if (type < AI_RELOC_MPOOL_TYPE_RELOC ||
            type > AI_RELOC_MPOOL_TYPE_RESET ||
            dtype < AI_RELOC_MPOOL_DTYPE_PARAM ||
            dtype > AI_RELOC_MPOOL_DTYPE_MIXED ||
            (attr & ~(AI_RELOC_MPOOL_DATTR_READ |
                      AI_RELOC_MPOOL_DATTR_WRITE |
                      AI_RELOC_MPOOL_DATTR_CACHEABLE)) != 0u ||
            size == 0u || size > UINT32_MAX - 31u) {
            return TIKU_NPU_ERR_HEADER;
        }

        has_initializer = type == AI_RELOC_MPOOL_TYPE_COPY ||
                          (type == AI_RELOC_MPOOL_TYPE_RELOC &&
                           dtype == AI_RELOC_MPOOL_DTYPE_PARAM);
        if (has_initializer &&
            (!round_up_size((size_t)size, 8u, &source_span) ||
             (size_t)foff > params_length ||
             source_span > params_length - (size_t)foff)) {
            return TIKU_NPU_ERR_HEADER;
        }

        if (out != NULL) {
            out->entries[i].flags = flags;
            out->entries[i].foff = foff;
            out->entries[i].dst = rd32(image + at + 12u);
            out->entries[i].size = size;
        }
    }
    if (i == RELOC_IMAGE_MAX_DESCS) return TIKU_NPU_ERR_HEADER;
    if (out != NULL) out->count = (uint8_t)i;
    return TIKU_NPU_OK;
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
    size_t params_length_bound;

    if (image == NULL ||
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
    // for (uint32_t vec = 0u; vec < 13u; vec++) {
    for (uint32_t vec = 0u; vec < 12u; vec++) {
        uint32_t encoded_entry = rd32(image + 52u + vec * 4u);

        if (encoded_entry == 0u) continue;
        if ((encoded_entry & RELOC_ADDR_CLASS_MASK) != RELOC_FLASH_CLASS) {
            return 0;
        }
        uint32_t entry = rel_off(encoded_entry);
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

    return validate_pool_descriptors(image, length, data_data, params_start,
                                     rel_start, params_length_bound, NULL) ==
           TIKU_NPU_OK;
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

static int tensor_type(Buffer_DataType_TypeDef type, uint8_t *out)
{
    switch (type) {
    case DataType_INT8:
        *out = TIKU_NPU_TENSOR_INT8;
        return 1;
    case DataType_UINT8:
        *out = TIKU_NPU_TENSOR_UINT8;
        return 1;
    case DataType_INT16:
        *out = TIKU_NPU_TENSOR_INT16;
        return 1;
    case DataType_INT32:
        *out = TIKU_NPU_TENSOR_INT32;
        return 1;
    case DataType_FLOAT:
        *out = TIKU_NPU_TENSOR_FLOAT32;
        return 1;
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
        const LL_Buffer_InfoTypeDef *buffer_info =
            ll_aton_reloc_get_input_buffers_info(instance, (int32_t)i);
        if (buffer_info == NULL || buffer_info->name == NULL) {
            break;
        }
        if (convert_tensor(buffer_info, &io->inputs[io->input_count]) != TIKU_NPU_OK) {
            return TIKU_NPU_ERR_ARGUMENT;
        }
        io->input_count++;
    }
    for (uint16_t i = 0u; i < TIKU_NPU_MODEL_MAX_IO; i++) {
        const LL_Buffer_InfoTypeDef *buffer_info =
            ll_aton_reloc_get_output_buffers_info(instance, (int32_t)i);
        if (buffer_info == NULL || buffer_info->name == NULL) {
            break;
        }
        if (convert_tensor(buffer_info, &io->outputs[io->output_count]) != TIKU_NPU_OK) {
            return TIKU_NPU_ERR_ARGUMENT;
        }
        io->output_count++;
    }
    return (io->input_count != 0u && io->output_count != 0u)
               ? TIKU_NPU_OK : TIKU_NPU_ERR_ARGUMENT;
}

static int validate_pool_intervals(
    const tiku_llaton_pool_descriptors_t *descriptors,
    const ll_aton_reloc_info *rt, uintptr_t npu_begin, uintptr_t npu_end,
    tiku_llaton_layout_t *layout)
{
    uint8_t seen_relative[2] = { 0u, 0u };
    unsigned i;

    if (descriptors == NULL || rt == NULL || npu_end <= npu_begin) {
        return TIKU_NPU_ERR_HEADER;
    }
    if (layout != NULL) memset(layout, 0, sizeof(*layout));

    for (i = 0u; i < descriptors->count; i++) {
        const tiku_llaton_pool_descriptor_t *descriptor =
            &descriptors->entries[i];
        uint32_t type = AI_RELOC_MPOOL_GET_TYPE(descriptor->flags);
        uint32_t dtype = AI_RELOC_MPOOL_GET_DTYPE(descriptor->flags);
        uint32_t attr = AI_RELOC_MPOOL_GET_ATTR(descriptor->flags);
        uint32_t id = AI_RELOC_MPOOL_GET_ID(descriptor->flags);
        size_t destination_span;

        if (type == AI_RELOC_MPOOL_TYPE_RELOC) {
            if (descriptor->dst != 0u || id > 1u ||
                seen_relative[id] != 0u) {
                return TIKU_NPU_ERR_ARGUMENT;
            }
            seen_relative[id] = 1u;
            if (id == 0u) {
                if (dtype != AI_RELOC_MPOOL_DTYPE_PARAM ||
                    (attr & AI_RELOC_MPOOL_DATTR_READ) == 0u ||
                    (attr & AI_RELOC_MPOOL_DATTR_WRITE) != 0u) {
                    return TIKU_NPU_ERR_ARGUMENT;
                }
            } else {
                if ((dtype != AI_RELOC_MPOOL_DTYPE_ACTIV &&
                     dtype != AI_RELOC_MPOOL_DTYPE_MIXED) ||
                    (attr & AI_RELOC_MPOOL_DATTR_WRITE) == 0u) {
                    return TIKU_NPU_ERR_ARGUMENT;
                }
                if (rt->ext_ram_sz == 0u ||
                    !round_up_size((size_t)descriptor->size, 32u,
                                   &destination_span) ||
                    destination_span > (size_t)rt->ext_ram_sz) {
                    return TIKU_NPU_ERR_CAPACITY;
                }
            }
            continue;
        }

        if (descriptor->dst == 0u || (descriptor->dst & 31u) != 0u ||
            !round_up_size((size_t)descriptor->size, 32u,
                           &destination_span)) {
            return TIKU_NPU_ERR_ARGUMENT;
        }
        if (type != AI_RELOC_MPOOL_TYPE_RESET &&
            type != AI_RELOC_MPOOL_TYPE_COPY) {
            return TIKU_NPU_ERR_HEADER;
        }
        if ((attr & AI_RELOC_MPOOL_DATTR_WRITE) == 0u ||
            (type == AI_RELOC_MPOOL_TYPE_RESET &&
             dtype != AI_RELOC_MPOOL_DTYPE_ACTIV &&
             dtype != AI_RELOC_MPOOL_DTYPE_MIXED) ||
            (uintptr_t)descriptor->dst < npu_begin ||
            (uintptr_t)descriptor->dst >= npu_end) {
            return TIKU_NPU_ERR_ARGUMENT;
        }
        if (destination_span >
            (size_t)(npu_end - (uintptr_t)descriptor->dst)) {
            return TIKU_NPU_ERR_CAPACITY;
        }
        if (layout != NULL) {
            uintptr_t start = (uintptr_t)descriptor->dst;
            uintptr_t end = start + (uintptr_t)destination_span;

            for (unsigned fixed_index = 0u; fixed_index < layout->fixed_count; fixed_index++) {
                if (intervals_overlap(start, end,
                                      layout->fixed[fixed_index].start,
                                      layout->fixed[fixed_index].end)) {
                    return TIKU_NPU_ERR_ARGUMENT;
                }
            }
            if (layout->fixed_count >= TIKU_LLATON_MAX_FIXED_REGIONS) {
                return TIKU_NPU_ERR_HEADER;
            }
            layout->fixed[layout->fixed_count].start = start;
            layout->fixed[layout->fixed_count].end = end;
            layout->fixed[layout->fixed_count].flags = descriptor->flags;
            layout->fixed_count++;
        }
    }
    if (layout != NULL) {
        sort_fixed_intervals(layout);
        return validate_dynamic_plan(layout, rt);
    }
    return TIKU_NPU_OK;
}

static int validate_pools(const tiku_n6_model_image_t *image,
                          const ll_aton_reloc_info *rt,
                          tiku_llaton_layout_t *layout)
{
    const uint8_t *source;
    tiku_llaton_pool_descriptors_t descriptors;
    uintptr_t params_ptr;
    size_t params_length;
    uintptr_t npu_begin;
    uintptr_t npu_end;
    size_t data_data;
    size_t rel_start;
    size_t params_start;
    int rc;

    if (layout != NULL) memset(layout, 0, sizeof(*layout));
    if (image == NULL || rt == NULL || image->file_ptr == 0u ||
        image->file_bytes < RELOC_IMAGE_DESC_BYTES ||
        (image->file_ptr & 3u) != 0u ||
        !root_interval(&npu_begin, &npu_end)) {
        return TIKU_NPU_ERR_HEADER;
    }
    source = (const uint8_t *)(uintptr_t)image->file_ptr;
    rc = resolve_parameter_span(image->file_ptr, image->file_bytes,
                                image->file_params_ptr, image->params_bytes,
                                (size_t)rt->params_off, &params_ptr,
                                &params_length);
    if (rc != TIKU_NPU_OK || params_ptr == 0u || params_length == 0u) {
        return rc == TIKU_NPU_OK ? TIKU_NPU_ERR_HEADER : rc;
    }

    if (!image_offset(rd32(source + 16u), RELOC_FLASH_CLASS,
                      image->file_bytes, &data_data) ||
        !image_offset(rd32(source + 36u), RELOC_FLASH_CLASS,
                      image->file_bytes, &rel_start) ||
        !image_offset(rd32(source + 44u), RELOC_RAM_CLASS,
                      image->file_bytes, &params_start)) {
        return TIKU_NPU_ERR_HEADER;
    }
    rc = validate_pool_descriptors(source, image->file_bytes, data_data,
                                   params_start, rel_start, params_length,
                                   &descriptors);
    if (rc != TIKU_NPU_OK) return rc;
    return validate_pool_intervals(&descriptors, rt, npu_begin, npu_end,
                                   layout);
}

#if defined(TIKU_NPU_POOL_TEST_ENABLE)
int tiku_npu_llaton_test_validate_pools(const void *image, size_t length,
                                        uintptr_t params_ptr,
                                        size_t params_length,
                                        uint32_t params_offset,
                                        uint32_t ext_ram_size)
{
    tiku_n6_model_image_t model_image;
    ll_aton_reloc_info info;
    tiku_llaton_layout_t layout;

    memset(&model_image, 0, sizeof(model_image));
    memset(&info, 0, sizeof(info));
    model_image.file_ptr = (uintptr_t)image;
    model_image.file_params_ptr = params_ptr;
    model_image.file_bytes = length;
    model_image.params_bytes = params_length;
    info.params_off = params_offset;
    info.ext_ram_sz = ext_ram_size;
    info.rt_ram_copy = 0x200u;
    return validate_pools(&model_image, &info, &layout);
}

int tiku_npu_llaton_test_materialize_pools(const void *image, size_t length,
                                           uintptr_t params_ptr,
                                           size_t params_length,
                                           uint32_t params_offset,
                                           uint32_t exec_ram_size,
                                           uint32_t ext_ram_size)
{
    tiku_n6_model_image_t model_image;
    ll_aton_reloc_info info;
    tiku_llaton_layout_t layout;
    uintptr_t resolved_params;
    size_t resolved_length;
    int rc;

    if (ll_model != NULL || ll_state.running ||
        ll_state.network_initialized || ll_state.file_ptr != 0u ||
        ll_state.exec_arena.active || ll_state.ext_arena.active ||
        ll_state.fixed_arena_count != 0u) {
        return TIKU_NPU_ERR_BUSY;
    }

    memset(&model_image, 0, sizeof(model_image));
    memset(&info, 0, sizeof(info));
    model_image.file_ptr = (uintptr_t)image;
    model_image.file_params_ptr = params_ptr;
    model_image.file_bytes = length;
    model_image.params_bytes = params_length;
    info.params_off = params_offset;
    info.ext_ram_sz = ext_ram_size;
    info.rt_ram_copy = exec_ram_size;

    rc = validate_pools(&model_image, &info, &layout);
    if (rc != TIKU_NPU_OK) return rc;
    rc = resolve_parameter_span(model_image.file_ptr, model_image.file_bytes,
                                model_image.file_params_ptr,
                                model_image.params_bytes,
                                (size_t)info.params_off, &resolved_params,
                                &resolved_length);
    if (rc != TIKU_NPU_OK || resolved_length == 0u) {
        return rc == TIKU_NPU_OK ? TIKU_NPU_ERR_HEADER : rc;
    }

    ll_state.info = info;
    ll_state.layout = layout;
    ll_state.file_params_ptr = resolved_params;
    return reserve_runtime_resources();
}
#endif

static int release_child_arena(tiku_arena_t *arena)
{
    tiku_mem_err_t rc;

    if (arena == NULL || !arena->active) {
        return TIKU_MEM_OK;
    }
    rc = tiku_arena_reset(arena);
    if (rc != TIKU_MEM_OK) {
        return (int)rc;
    }
    return (int)tiku_arena_destroy(arena);
}

static int release_runtime_resources(void)
{
    int rc;

    /* Fixed pools are claimed after the dynamic children, so release their
     * ownership records in the exact reverse order before rewinding root. */
    while (ll_state.fixed_arena_count != 0u) {
        unsigned index = (unsigned)ll_state.fixed_arena_count - 1u;

        rc = release_child_arena(&ll_state.fixed_arenas[index]);
        if (rc != TIKU_MEM_OK) {
            return rc;
        }
        ll_state.fixed_arena_count--;
    }

    rc = release_child_arena(&ll_state.ext_arena);
    if (rc != TIKU_MEM_OK) {
        return rc;
    }
    rc = release_child_arena(&ll_state.exec_arena);
    if (rc != TIKU_MEM_OK) {
        return rc;
    }

    rc = (int)tiku_stm32n6_npu_mem_reset();
    if (rc != TIKU_MEM_OK) {
        return rc;
    }

    memset(ll_state.fixed_arenas, 0, sizeof(ll_state.fixed_arenas));
    memset(&ll_state.ext_arena, 0, sizeof(ll_state.ext_arena));
    memset(&ll_state.exec_arena, 0, sizeof(ll_state.exec_arena));
    memset(&ll_state.config, 0, sizeof(ll_state.config));
    return TIKU_MEM_OK;
}

static int rollback_runtime_resources(int original_error)
{
    if (release_runtime_resources() != TIKU_MEM_OK) {
        return TIKU_NPU_ERR_STATE;
    }
    return original_error;
}

static int reserve_runtime_resources(void)
{
    int rc;
    uintptr_t ext_addr = 0u;

    if (ll_state.exec_arena.active || ll_state.ext_arena.active ||
        ll_state.fixed_arena_count != 0u) {
        return TIKU_NPU_ERR_STATE;
    }
    const tiku_arena_t *root = tiku_stm32n6_npu_mem_root();
    if (root == NULL) {
        return TIKU_NPU_ERR_STATE;
    }

    /* Bind normally performs this preflight.  Repeating it here makes the
     * allocation transaction safe for test seams and stale-state failures. */
    rc = validate_dynamic_plan(&ll_state.layout, &ll_state.info);
    if (rc != TIKU_NPU_OK) return rc;
    if (tiku_stm32n6_npu_mem_reset() != TIKU_MEM_OK) {
        return TIKU_NPU_ERR_STATE;
    }

    /* Child controls live outside the root so a parent reset can never erase
     * metadata required to tear them down. */
    memset(&ll_state.exec_arena, 0, sizeof(ll_state.exec_arena));
    memset(&ll_state.ext_arena, 0, sizeof(ll_state.ext_arena));
    memset(ll_state.fixed_arenas, 0, sizeof(ll_state.fixed_arenas));
    ll_state.fixed_arena_count = 0u;
    memset(&ll_state.config, 0, sizeof(ll_state.config));

    /* LL-ATON requires the executable base to be 8-byte aligned.  The
     * allocator rounds the carved capacity to its platform alignment, so the
     * requested COPY size itself does not need to be a multiple of eight. */
    if (ll_state.info.rt_ram_copy == 0u ||
        (uint32_t)(tiku_mem_arch_size_t)ll_state.info.rt_ram_copy !=
            ll_state.info.rt_ram_copy) {
        return TIKU_NPU_ERR_CAPACITY;
    }

    // Allocate resources for the executable arena from the root arena
    void *backing = tiku_stm32n6_npu_mem_alloc(
        (tiku_mem_arch_size_t)ll_state.info.rt_ram_copy, 32u);
    if (backing == NULL) {
        return TIKU_NPU_ERR_CAPACITY;
    }

    // Setup executable arena with allocated resources
    rc = (int)tiku_arena_create_raw(
        &ll_state.exec_arena, (uint8_t *)backing,
        (tiku_mem_arch_size_t)ll_state.info.rt_ram_copy);
    if (rc != TIKU_MEM_OK) {
        return rollback_runtime_resources(TIKU_NPU_ERR_CAPACITY);
    }

    // Check arena capacity and address alignment
    uintptr_t exec_addr = (uintptr_t)ll_state.exec_arena.buf;
    if ((exec_addr & 7u) != 0u ||
        ll_state.exec_arena.capacity < (size_t)ll_state.info.rt_ram_copy) {
        return rollback_runtime_resources(TIKU_NPU_ERR_CAPACITY);
    }
#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
    /* Exercise cleanup after the first child exists, rather than simulating
     * a failure before the allocation transaction has begun. */
    if (ll_test_load_failure == TIKU_NPU_LIFECYCLE_FAIL_RESERVE) {
        return TIKU_NPU_ERR_CAPACITY;
    }
#endif

    if (ll_state.info.ext_ram_sz != 0u) {
#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE) || \
    defined(TIKU_NPU_POOL_TEST_ENABLE) || \
    defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
        ll_test_ext_ram_reserve_attempts++;
#endif
        if ((uint32_t)(tiku_mem_arch_size_t)ll_state.info.ext_ram_sz !=
            ll_state.info.ext_ram_sz) {
            return rollback_runtime_resources(TIKU_NPU_ERR_CAPACITY);
        }
        backing = tiku_stm32n6_npu_mem_alloc(
            (tiku_mem_arch_size_t)ll_state.info.ext_ram_sz, 32u);
        if (backing == NULL ||
            tiku_arena_create_raw(
                &ll_state.ext_arena, (uint8_t *)backing,
                (tiku_mem_arch_size_t)ll_state.info.ext_ram_sz) !=
                TIKU_MEM_OK) {
            return rollback_runtime_resources(TIKU_NPU_ERR_CAPACITY);
        }
        ext_addr = (uintptr_t)ll_state.ext_arena.buf;
    }
#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
    if (ll_test_load_failure == TIKU_NPU_LIFECYCLE_FAIL_AFTER_EXT) {
        return TIKU_NPU_ERR_CAPACITY;
    }
#endif

    rc = validate_runtime_intervals(&ll_state.layout, exec_addr,
                                    (size_t)ll_state.info.rt_ram_copy,
                                    ext_addr,
                                    (size_t)ll_state.info.ext_ram_sz);
    if (rc != TIKU_NPU_OK) {
        return rollback_runtime_resources(rc);
    }

    for (unsigned i = 0u; i < ll_state.layout.fixed_count; i++) {
        const tiku_llaton_interval_t *fixed = &ll_state.layout.fixed[i];
        tiku_mem_arch_size_t span =
            (tiku_mem_arch_size_t)(fixed->end - fixed->start);

        backing = tiku_stm32n6_npu_mem_alloc_at(fixed->start, span, 32u);
        if (backing == NULL ||
            tiku_arena_create_raw(&ll_state.fixed_arenas[i],
                                  (uint8_t *)backing, span) != TIKU_MEM_OK) {
            return rollback_runtime_resources(TIKU_NPU_ERR_CAPACITY);
        }
        ll_state.fixed_arena_count++;
    }
#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
    if (ll_test_load_failure == TIKU_NPU_LIFECYCLE_FAIL_AFTER_FIXED) {
        return TIKU_NPU_ERR_CAPACITY;
    }
#endif

    /* Publish the relocation configuration only after every child region has
     * an ownership record and the full monotonic layout is established. */
    ll_state.config.exec_ram_addr = exec_addr;
    ll_state.config.exec_ram_size = ll_state.info.rt_ram_copy;
    ll_state.config.ext_ram_addr = ext_addr;
    ll_state.config.ext_ram_size = ll_state.info.ext_ram_sz;
    ll_state.config.ext_param_addr = ll_state.file_params_ptr;
    ll_state.config.mode = AI_RELOC_RT_LOAD_MODE_COPY;
    return TIKU_NPU_OK;
}

/* Complete the per-model transaction without touching the global runtime. */
static int handle_load_runtime_error(tiku_npu_model_t *model,
                                     int original_error)
{
    int cleanup_rc;
    uint8_t runtime_ready;

    if (model == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (ll_state.running) return TIKU_NPU_ERR_BUSY;

    runtime_ready = ll_state.runtime_ready;
    if (ll_state.network_initialized) {
        LL_ATON_RT_DeInit_Network(&ll_state.instance);
#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
        ll_test_network_deinit_calls++;
#endif
        ll_state.network_initialized = 0u;
    }

    memset(&ll_state.instance, 0, sizeof(ll_state.instance));
    memset(&model->io, 0, sizeof(model->io));
    ll_state.instance_installed = 0u;
    cleanup_rc = release_runtime_resources();
    if (cleanup_rc != TIKU_MEM_OK) {
        ll_state.last_error = TIKU_NPU_ERR_STATE;
        return TIKU_NPU_ERR_STATE;
    }

    model->container_loaded = 0u;
    model->container_bound = 0u;
    memset(model->container_path, 0, sizeof(model->container_path));
    model->backend_state = &ll_state;
    tiku_n6_model_store_set_model_bound(0);
    ll_model = NULL;
    ll_state.network_initialized = 0u;
    ll_state.running = 0u;
    ll_state.owner = NULL;
    ll_state.file_ptr = 0u;
    ll_state.file_params_ptr = 0u;
    ll_state.source_len = 0u;
    ll_state.params_len = 0u;
    memset(&ll_state.image, 0, sizeof(ll_state.image));
    memset(&ll_state.info, 0, sizeof(ll_state.info));
    memset(&ll_state.layout, 0, sizeof(ll_state.layout));
    ll_state.runtime_ready = runtime_ready;
    ll_state.last_error = original_error;

    return original_error;
}

int tiku_npu_model_bind(tiku_npu_model_t *model, const char *model_ref)
{
    tiku_n6_model_image_t image;
    uintptr_t params_ptr;
    size_t params_length;
    uint32_t capacity;
    int rc;

    if (model == NULL || model_ref == NULL) {
        TIKU_PRINTF("Model or path is NULL\n");
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (!ll_runtime_initialized) {
        TIKU_PRINTF("NPU runtime is not initialized\n");
        return TIKU_NPU_ERR_STATE;
    }
    const tiku_arena_t *root = tiku_stm32n6_npu_mem_root();
    if (root == NULL) {
        TIKU_PRINTF("NPU root memory is not reserved\n");
        return TIKU_NPU_ERR_STATE;
    }
    if (strlen(model_ref) >= TIKU_NPU_MODEL_PATH_MAX) {
        TIKU_PRINTF("Model path is too long\n");
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (model->container_bound || model->container_loaded || ll_model != NULL) {
        TIKU_PRINTF("Model is already loaded or ll_model is not NULL\n");
        return TIKU_NPU_ERR_BUSY;
    }

    rc = tiku_n6_model_store_resolve(model_ref, &image);
    if (rc != TIKU_NPU_OK) {
        TIKU_PRINTF("Failed to resolve model image with error: %d\n", rc);
        return rc;
    }
    if (image_preflight((const uint8_t *)(uintptr_t)image.file_ptr,
                        image.file_bytes, image.file_params_ptr,
                        image.params_bytes) != 1) {
        TIKU_PRINTF("Invalid bounded model image\n");
        return TIKU_NPU_ERR_HEADER;
    }

    memset(&ll_state, 0, sizeof(ll_state));
    ll_state.runtime_ready = ll_runtime_initialized;
    ll_state.image = image;
    ll_state.file_ptr = image.file_ptr;
    ll_state.file_params_ptr = image.file_params_ptr;
    ll_state.source_len = image.file_bytes;
    ll_state.params_len = image.params_bytes;
    capacity = (uint32_t)root->capacity;
    if (model->slot_capacity != 0u && model->slot_capacity < capacity) {
        capacity = model->slot_capacity;
    }
    rc = ll_aton_reloc_get_info(ll_state.file_ptr, &ll_state.info);
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
        ll_state.info.rt_ram_copy == 0u ||
        ll_state.info.rt_ram_copy > capacity) {
        int bind_error =
            rc == 0 && ll_state.info.rt_ram_copy > capacity
                ? TIKU_NPU_ERR_CAPACITY : TIKU_NPU_ERR_HEADER;

        TIKU_PRINTF("Invalid model info\n");
        clear_model_inspection();
        return bind_error;
    }
    rc = resolve_parameter_span(ll_state.file_ptr, ll_state.source_len,
                                image.file_params_ptr, image.params_bytes,
                                (size_t)ll_state.info.params_off,
                                &params_ptr, &params_length);
    if (rc != TIKU_NPU_OK) {
        clear_model_inspection();
        return rc;
    }
    ll_state.file_params_ptr = params_ptr;
    ll_state.params_len = params_length;
    rc = validate_pools(&ll_state.image, &ll_state.info, &ll_state.layout);
    if (rc != TIKU_NPU_OK) {
        TIKU_PRINTF("Failed to validate pools with error: %d\n", rc);
        clear_model_inspection();
        return rc;
    }

    strncpy(model->container_path, model_ref,
            sizeof(model->container_path) - 1u);
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

    if (model == NULL) {
        TIKU_PRINTF("Model is NULL\n");
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (!model->container_bound || model->backend_state != &ll_state) {
        TIKU_PRINTF("Model is not bound or backend state mismatch\n");
        return TIKU_NPU_ERR_STATE;
    }
    if (model->container_loaded || ll_model != NULL || ll_state.running) {
        TIKU_PRINTF("Model is already loaded or another model is loaded\n");
        return TIKU_NPU_ERR_BUSY;
    }
    if (ll_state.file_ptr == 0u || ll_state.source_len == 0u ||
        ll_state.info.rt_ram_copy == 0u) {
        TIKU_PRINTF("Bound model inspection state is incomplete\n");
        return TIKU_NPU_ERR_STATE;
    }
    if (!ll_state.runtime_ready) {
        TIKU_PRINTF("Runtime is not ready\n");
        return TIKU_NPU_ERR_STATE;
    }
    rc = reserve_runtime_resources();
    if (rc != TIKU_NPU_OK) {
        TIKU_PRINTF("Failed to reserve LL-ATON runtime resources with error: %d\n",
                    rc);
        return handle_load_runtime_error(model, rc);
    }
    memset(&ll_state.instance, 0, sizeof(ll_state.instance));

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE) || \
    defined(TIKU_NPU_POOL_TEST_ENABLE) || \
    defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
    ll_test_install_calls++;
#endif
#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
    if (ll_test_load_failure == TIKU_NPU_LIFECYCLE_FAIL_INSTALL) {
        return handle_load_runtime_error(model, TIKU_NPU_ERR_RUNTIME);
    }
#endif
    rc = ll_aton_reloc_install(ll_state.file_ptr, &ll_state.config,
                               &ll_state.instance);
    if (rc != 0 || ll_aton_reloc_is_valid(&ll_state.instance) != 1) {
        TIKU_PRINTF("Failed to install relocatable model with error: %d\n", rc);
        return handle_load_runtime_error(model, TIKU_NPU_ERR_RUNTIME);
    }

    ll_state.instance_installed = 1u;
    LL_ATON_RT_Init_Network(&ll_state.instance);
    ll_state.network_initialized = 1u;
#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
    if (ll_test_load_failure == TIKU_NPU_LIFECYCLE_FAIL_AFTER_INIT) {
        return handle_load_runtime_error(model, TIKU_NPU_ERR_RUNTIME);
    }
#endif
    rc = (ll_aton_reloc_is_valid(&ll_state.instance) == 1 &&
          cache_io(&ll_state.instance, &model->io) == TIKU_NPU_OK)
             ? TIKU_NPU_OK : TIKU_NPU_ERR_RUNTIME;
    if (rc != TIKU_NPU_OK) {
        TIKU_PRINTF("Failed to cache IO or validate instance\n");
        return handle_load_runtime_error(model, TIKU_NPU_ERR_RUNTIME);
    }

    model->container_loaded = 1u;
    ll_model = model;
    ll_state.last_error = TIKU_NPU_OK;
    ll_state.running = 0u;
    return TIKU_NPU_OK;
}

int tiku_npu_model_unload(tiku_npu_model_t *model)
{
    int rc;

    if (model == NULL) return TIKU_NPU_ERR_ARGUMENT;
    if (ll_state.running || (ll_model != NULL && ll_model != model)) {
        return TIKU_NPU_ERR_BUSY;
    }
    if (!model->container_bound && !model->container_loaded &&
        ll_model == NULL && ll_state.file_ptr == 0u &&
        !ll_state.instance_installed && !ll_state.network_initialized &&
        !ll_state.exec_arena.active && !ll_state.ext_arena.active &&
        ll_state.fixed_arena_count == 0u) {
        return TIKU_NPU_OK;
    }
    if (model->backend_state != &ll_state) {
        return TIKU_NPU_ERR_STATE;
    }

    rc = handle_load_runtime_error(model, TIKU_NPU_OK);
    return rc;
}

int tiku_npu_model_io(const tiku_npu_model_t *model, tiku_npu_model_io_t *out)
{
    if (model == NULL || out == NULL)
    {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    if (!model->container_loaded) {
        return TIKU_NPU_ERR_STATE;
    }
    *out = model->io;
    return TIKU_NPU_OK;
}

int tiku_npu_model_last_error(const tiku_npu_model_t *model)
{
    if (model == NULL || model->backend_state != &ll_state) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    return ll_state.last_error;
}

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
void tiku_npu_llaton_test_reset_run_trace(void)
{
    memset(&ll_run_trace, 0, sizeof(ll_run_trace));
}

int tiku_npu_llaton_test_get_run_trace(tiku_npu_llaton_run_trace_t *out)
{
    if (out == NULL) return TIKU_NPU_ERR_ARGUMENT;
    *out = ll_run_trace;
    return TIKU_NPU_OK;
}
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE) || \
    defined(TIKU_NPU_POOL_TEST_ENABLE) || \
    defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
unsigned tiku_npu_llaton_test_ext_ram_reserve_attempts(void)
{
    return ll_test_ext_ram_reserve_attempts;
}

unsigned tiku_npu_llaton_test_install_calls(void)
{
    return ll_test_install_calls;
}

int tiku_npu_llaton_test_release_resources(void)
{
    int rc;

    if (ll_model != NULL || ll_state.running ||
        ll_state.network_initialized) {
        return TIKU_NPU_ERR_BUSY;
    }
    rc = release_runtime_resources();
    if (rc != TIKU_MEM_OK) return TIKU_NPU_ERR_STATE;

    memset(&ll_state.info, 0, sizeof(ll_state.info));
    memset(&ll_state.layout, 0, sizeof(ll_state.layout));
    ll_state.file_params_ptr = 0u;
    return TIKU_NPU_OK;
}

int tiku_npu_llaton_test_get_resource_trace(
    tiku_npu_llaton_resource_trace_t *out)
{
    const tiku_arena_t *root;
    tiku_mem_stats_t stats;

    if (out == NULL) return TIKU_NPU_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->exec_addr = ll_state.config.exec_ram_addr;
    out->ext_addr = ll_state.config.ext_ram_addr;
    out->exec_active = ll_state.exec_arena.active;
    out->ext_active = ll_state.ext_arena.active;
    out->fixed_count = ll_state.fixed_arena_count;
    for (unsigned i = 0u; i < ll_state.fixed_arena_count; i++) {
        out->fixed_addr[i] = (uintptr_t)ll_state.fixed_arenas[i].buf;
        out->fixed_size[i] = (size_t)ll_state.fixed_arenas[i].capacity;
        if (ll_state.fixed_arenas[i].active) out->fixed_active_count++;
    }

    root = tiku_stm32n6_npu_mem_root();
    if (root == NULL || tiku_arena_stats(root, &stats) != TIKU_MEM_OK) {
        return TIKU_NPU_ERR_STATE;
    }
    out->root_used = (size_t)stats.used_bytes;
    return TIKU_NPU_OK;
}
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE)
int tiku_npu_llaton_test_reserve_resources(uint32_t exec_size,
                                           uint32_t ext_size,
                                           uintptr_t *exec_addr,
                                           uintptr_t *ext_addr)
{
    int rc;

    if (exec_addr == NULL || ext_addr == NULL || ll_model != NULL ||
        ll_state.running || ll_state.network_initialized) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    *exec_addr = 0u;
    *ext_addr = 0u;
    if (ll_state.exec_arena.active || ll_state.ext_arena.active ||
        ll_state.fixed_arena_count != 0u ||
        ll_state.config.exec_ram_addr != 0u) {
        rc = tiku_npu_llaton_test_release_resources();
        if (rc != TIKU_NPU_OK) return rc;
    }

    memset(&ll_state.info, 0, sizeof(ll_state.info));
    memset(&ll_state.layout, 0, sizeof(ll_state.layout));
    ll_state.info.rt_ram_copy = exec_size;
    ll_state.info.ext_ram_sz = ext_size;
    ll_state.file_params_ptr = 0u;
    rc = reserve_runtime_resources();
    if (rc != TIKU_NPU_OK) return rc;
    *exec_addr = ll_state.config.exec_ram_addr;
    *ext_addr = ll_state.config.ext_ram_addr;
    return TIKU_NPU_OK;
}

int tiku_npu_llaton_test_get_config(uintptr_t *exec_addr,
                                    uint32_t *exec_size,
                                    uintptr_t *ext_addr,
                                    size_t *ext_size,
                                    uintptr_t *param_addr,
                                    uint32_t *required_exec_size,
                                    uint32_t *requested_ext_size)
{
    if (exec_addr == NULL || exec_size == NULL || ext_addr == NULL ||
        ext_size == NULL || param_addr == NULL ||
        required_exec_size == NULL || requested_ext_size == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    *exec_addr = ll_state.config.exec_ram_addr;
    *exec_size = ll_state.config.exec_ram_size;
    *ext_addr = ll_state.config.ext_ram_addr;
    *ext_size = ll_state.config.ext_ram_size;
    *param_addr = ll_state.config.ext_param_addr;
    *required_exec_size = ll_state.info.rt_ram_copy;
    *requested_ext_size = ll_state.info.ext_ram_sz;
    return TIKU_NPU_OK;
}
#endif

#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
void tiku_npu_llaton_test_reset_lifecycle(void)
{
    ll_test_network_deinit_calls = 0u;
    ll_test_install_calls = 0u;
    ll_test_ext_ram_reserve_attempts = 0u;
    ll_test_load_failure = TIKU_NPU_LIFECYCLE_FAIL_NONE;
}

void tiku_npu_llaton_test_set_load_failure(int failure)
{
    ll_test_load_failure = failure;
}

int tiku_npu_llaton_test_get_lifecycle_trace(
    tiku_npu_llaton_lifecycle_trace_t *out)
{
    const tiku_arena_t *root;
    tiku_mem_stats_t stats;

    if (out == NULL) return TIKU_NPU_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->network_deinit_calls = ll_test_network_deinit_calls;
    out->install_calls = ll_test_install_calls;
    out->exec_addr = ll_state.config.exec_ram_addr;
    out->ext_addr = ll_state.config.ext_ram_addr;
    out->runtime_initialized = ll_runtime_initialized;
    out->exec_active = ll_state.exec_arena.active;
    out->ext_active = ll_state.ext_arena.active;
    out->fixed_count = ll_state.fixed_arena_count;
    for (unsigned i = 0u; i < ll_state.fixed_arena_count; i++) {
        out->fixed_addr[i] = (uintptr_t)ll_state.fixed_arenas[i].buf;
        out->fixed_size[i] = (size_t)ll_state.fixed_arenas[i].capacity;
        if (ll_state.fixed_arenas[i].active) out->fixed_active_count++;
    }
    out->instance_installed = ll_state.instance_installed;
    out->network_initialized = ll_state.network_initialized;
    root = tiku_stm32n6_npu_mem_root();
    if (root == NULL || tiku_arena_stats(root, &stats) != TIKU_MEM_OK) {
        return TIKU_NPU_ERR_STATE;
    }
    out->root_used = (size_t)stats.used_bytes;
    return TIKU_NPU_OK;
}
#endif

static uint8_t tiku_llaton_post_wake(const tiku_npu_model_t *model,
                                     uint8_t from_irq)
{
    if (model == NULL || model != ll_model || !ll_state.running) {
        return 0u;
    }

    uint8_t posted = tiku_process_post(&npu_llaton_worker, TIKU_EVENT_NPU_WAKE,
                               (tiku_event_data_t)model);
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    ll_run_trace.wake_posts++;
    if (from_irq) {
        ll_run_trace.irq_wake_posts++;
    } else {
        ll_run_trace.worker_wake_posts++;
    }
    if (!posted) {
        ll_run_trace.dropped_wake_posts++;
    }
#else
    (void)from_irq;
#endif

    return posted;
}

static void tiku_llaton_abort_submission(void)
{
    ll_state.running = 0u;
    ll_state.owner = NULL;
    ll_state.last_error = TIKU_NPU_ERR_RUNTIME;
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    ll_run_trace.runtime_failures++;
#endif
}

static void tiku_llaton_worker_step(void)
{
    LL_ATON_RT_RetValues_t ret;
    if (ll_model == NULL || !ll_state.running) {
        return;
    }
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    ll_run_trace.epoch_calls++;
#endif
    ret = LL_ATON_RT_RunEpochBlock(&ll_state.instance);
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    ll_run_trace.last_submit_result = (int)ret;
    if (ret == LL_ATON_RT_WFE) {
        ll_run_trace.wfe_returns++;
    } else if (ret == LL_ATON_RT_NO_WFE) {
        ll_run_trace.no_wfe_returns++;
    } else if (ret == LL_ATON_RT_DONE) {
        ll_run_trace.done_returns++;
    }
#endif
    if (ret == LL_ATON_RT_WFE) {
        return;
    }
    if (ret == LL_ATON_RT_NO_WFE) {
        if (!tiku_llaton_post_wake(ll_model, 0u)) {
            tiku_llaton_finish((LL_ATON_RT_RetValues_t)(-1));
        }
        return;
    }
    tiku_llaton_finish(ret);
}

static void tiku_llaton_finish(LL_ATON_RT_RetValues_t ret)
{
    if (ll_model == NULL || !ll_state.running) {
        return;
    }
    struct tiku_process *owner = ll_state.owner;
    tiku_npu_model_t *model = ll_model;
    ll_state.last_error = ret == LL_ATON_RT_DONE
                              ? TIKU_NPU_OK : TIKU_NPU_ERR_RUNTIME;
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    if (ret != LL_ATON_RT_DONE) {
        ll_run_trace.runtime_failures++;
    }
#endif
    ll_state.running = 0u;
    ll_state.owner = NULL;
    if (owner != NULL) {
        uint8_t posted = tiku_process_post(owner, TIKU_EVENT_NPU_DONE,
                                           (tiku_event_data_t)model);
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
        ll_run_trace.completion_posts++;
        if (!posted) {
            ll_run_trace.dropped_completion_posts++;
        }
#else
        (void)posted;
#endif
    }
}

int tiku_npu_run(const tiku_npu_model_t *model, const void *in[], void *out[])
{
    LL_ATON_RT_RetValues_t ret;
    struct tiku_process *owner = TIKU_PROCESS_CURRENT();

    if (model == NULL || in == NULL || out == NULL || model != ll_model ||
        !model->container_loaded || owner == NULL)
        return TIKU_NPU_ERR_STATE;
    if (ll_state.running) {
        return TIKU_NPU_ERR_BUSY;
    }
    if (!ll_worker_started) {
        tiku_process_start(&npu_llaton_worker, NULL);
        ll_worker_started = 1u;
    }
    for (uint16_t i = 0u; i < model->io.input_count; i++) {
        const LL_Buffer_InfoTypeDef *desc =
            ll_aton_reloc_get_input_buffers_info(&ll_state.instance, i);
        if (in[i] == NULL || desc == NULL ||
            ((uintptr_t)in[i] & 3u) != 0u || LL_ATON_Set_User_Input_Buffer(
                &ll_state.instance, i, (void *)in[i],
                LL_Buffer_len(desc)) !=
                LL_ATON_User_IO_NOERROR) return TIKU_NPU_ERR_ARGUMENT;
    }
    for (uint16_t i = 0u; i < model->io.output_count; i++) {
        const LL_Buffer_InfoTypeDef *desc =
            ll_aton_reloc_get_output_buffers_info(&ll_state.instance, i);
        if (out[i] == NULL || desc == NULL ||
            ((uintptr_t)out[i] & 3u) != 0u || LL_ATON_Set_User_Output_Buffer(
                &ll_state.instance, i, out[i],
                LL_Buffer_len(desc)) !=
            LL_ATON_User_IO_NOERROR) return TIKU_NPU_ERR_ARGUMENT;
    }
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    ll_run_trace.submissions++;
    ll_run_trace.reset_calls++;
#endif
    LL_ATON_RT_Reset_Network(&ll_state.instance);
    ll_state.owner = owner;
    ll_state.running = 1u;
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    ll_run_trace.epoch_calls++;
#endif
    ret = LL_ATON_RT_RunEpochBlock(&ll_state.instance);
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    ll_run_trace.last_submit_result = (int)ret;
    if (ret == LL_ATON_RT_WFE) ll_run_trace.wfe_returns++;
    else if (ret == LL_ATON_RT_NO_WFE) ll_run_trace.no_wfe_returns++;
    else if (ret == LL_ATON_RT_DONE) ll_run_trace.done_returns++;
#endif

    if (ret == LL_ATON_RT_DONE) {
        tiku_llaton_finish(ret);
    } else if (ret != LL_ATON_RT_WFE && ret != LL_ATON_RT_NO_WFE) {
        tiku_llaton_abort_submission();
        return TIKU_NPU_ERR_RUNTIME;
    } else if (ret == LL_ATON_RT_NO_WFE) {
        if (!tiku_llaton_post_wake(model, 0u)) {
            tiku_llaton_abort_submission();
            return TIKU_NPU_ERR_RUNTIME;
        }
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
void tiku_llaton_osal_deinit(void) {
    ll_irq_handler = NULL;
}

void tiku_llaton_osal_enter_cs(void) {
    tiku_llaton_osal_disable_irq(0u);
}

void tiku_llaton_osal_exit_cs(void) {
    tiku_llaton_osal_enable_irq(0u);
}

void tiku_llaton_osal_signal_event(void) { }

void tiku_npu_llaton_irq_bridge(void)
{
    if (ll_irq_handler != NULL) ll_irq_handler();
    if (ll_model != NULL && ll_state.running) {
        (void)tiku_llaton_post_wake(ll_model, 1u);
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

int tiku_npu_llaton_runtime_deinit(void)
{
    if (ll_model != NULL || ll_state.running || ll_state.file_ptr != 0u ||
        ll_state.exec_arena.active || ll_state.ext_arena.active ||
        ll_state.fixed_arena_count != 0u ||
        ll_state.config.exec_ram_addr != 0u) {
        return TIKU_NPU_ERR_BUSY;
    }
    if (!ll_runtime_initialized) {
        ll_state.runtime_ready = 0u;
        return TIKU_NPU_OK;
    }

    LL_ATON_RT_RuntimeDeInit();
    ll_runtime_initialized = 0u;
    ll_state.runtime_ready = 0u;
    ll_irq_handler = NULL;
    return TIKU_NPU_OK;
}
