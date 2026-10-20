/* STM32N6 LL-ATON memory-pool policy acceptance process. */
#include <stdint.h>
#include <string.h>

#include <kernel/process/tiku_process.h>

#include <interfaces/npu/tiku_npu.h>
#include <arch/stm32n6/tiku_npu_mem.h>
#include <arch/stm32n6/tiku_npu_llaton.h>
#include <arch/stm32n6/tiku_npu_arch.h>
#include <arch/stm32n6/tiku_ospi_arch.h>
#include "npu/fixtures/stm32n6_identity_corrupt.network_rel.h"

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_POOL_TEST_ENABLE

#define IMAGE_WORD(_n) ((size_t)(_n) * 4u)
#define IMAGE_MASK UINT32_C(0x0fffffff)
#define DESC_BYTES 20u
#define DESC_FLAGS 4u
#define DESC_FOFF 8u
#define DESC_DST 12u
#define DESC_SIZE 16u

TIKU_PROCESS(npu_pool_test, "NPU pool test");

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        TIKU_PRINTF("NPU pool FAIL: %s\n", message);
    }
}

static uint32_t get32(const uint8_t *image, size_t offset)
{
    return (uint32_t)image[offset] |
           ((uint32_t)image[offset + 1u] << 8) |
           ((uint32_t)image[offset + 2u] << 16) |
           ((uint32_t)image[offset + 3u] << 24);
}

static void put32(uint8_t *image, size_t offset, uint32_t value)
{
    image[offset] = (uint8_t)value;
    image[offset + 1u] = (uint8_t)(value >> 8);
    image[offset + 2u] = (uint8_t)(value >> 16);
    image[offset + 3u] = (uint8_t)(value >> 24);
}

static size_t raw_offset(const uint8_t *image, unsigned word)
{
    return (size_t)(get32(image, IMAGE_WORD(word)) & IMAGE_MASK);
}

static size_t descriptor_at(const uint8_t *image, unsigned index)
{
    return raw_offset(image, 4u) + raw_offset(image, 11u) +
           (size_t)index * DESC_BYTES;
}

static void make_base(uint8_t *image)
{
    size_t desc0;
    size_t desc1;

    memcpy(image, tiku_npu_stm32n6_identity_corrupt,
           tiku_npu_stm32n6_identity_corrupt_len);
    desc0 = descriptor_at(image, 0u);
    desc1 = descriptor_at(image, 1u);

    /* ID 0: combined-image read-only parameter pool. */
    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x01010100));
    put32(image, desc0 + DESC_FOFF, UINT32_C(0));
    put32(image, desc0 + DESC_DST, UINT32_C(0));
    put32(image, desc0 + DESC_SIZE, UINT32_C(4));

    /* RESET activation pool in the linker-owned NPU tier. */
    put32(image, desc1 + DESC_FLAGS, UINT32_C(0x03020200));
    put32(image, desc1 + DESC_DST, UINT32_C(0x342e0000));
    put32(image, desc1 + DESC_SIZE, UINT32_C(0x20));
}

static int validate(const uint8_t *image, uintptr_t params_ptr,
                    size_t params_length, uint32_t params_offset,
                    uint32_t ext_ram_size)
{
    return tiku_npu_llaton_test_validate_pools(
        image, tiku_npu_stm32n6_identity_corrupt_len, params_ptr,
        params_length, params_offset, ext_ram_size);
}

PT_THREAD(tiku_process_thread_npu_pool_test(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    static uint8_t image[1856] __attribute__((aligned(8)));
    static uint8_t saved_desc[DESC_BYTES];
    size_t desc0;
    size_t desc1;
    size_t desc2;
    uint32_t params_offset;
    unsigned reserve_before;
    unsigned install_before;
    tiku_npu_llaton_resource_trace_t first_layout;
    tiku_npu_llaton_resource_trace_t second_layout;
    const tiku_arena_t *root;
    int rc;

    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    rc = tiku_npu_init();
    check(rc == TIKU_NPU_INIT_OK || rc == TIKU_NPU_INIT_ALREADY,
          "application NPU initialization failed");

    make_base(image);
    desc0 = descriptor_at(image, 0u);
    desc1 = descriptor_at(image, 1u);
    desc2 = descriptor_at(image, 2u);
    params_offset = get32(image, IMAGE_WORD(12u));
    reserve_before = tiku_npu_llaton_test_ext_ram_reserve_attempts();
    install_before = tiku_npu_llaton_test_install_calls();

    rc = validate(image, 0u, 0u, params_offset, 0u);
    check(rc == TIKU_NPU_OK,
          "valid combined parameter and RESET pools were rejected");
    check(tiku_npu_llaton_test_ext_ram_reserve_attempts() == reserve_before &&
              tiku_npu_llaton_test_install_calls() == install_before,
          "pool validation allocated external RAM or installed a model");

    root = tiku_stm32n6_npu_mem_root();
    rc = tiku_npu_llaton_test_materialize_pools(
        image, tiku_npu_stm32n6_identity_corrupt_len, 0u, 0u,
        params_offset, 0x200u, 0u);
    check(rc == TIKU_NPU_OK &&
              tiku_npu_llaton_test_get_resource_trace(&first_layout) ==
                  TIKU_NPU_OK &&
              first_layout.fixed_count == 1u &&
              first_layout.fixed_active_count == 1u &&
              first_layout.fixed_addr[0] == UINT32_C(0x342e0000) &&
              first_layout.fixed_size[0] == 0x20u &&
              root != NULL &&
              first_layout.root_used ==
                  (size_t)(UINT32_C(0x342e0020) -
                           (uintptr_t)root->buf),
          "fixed RESET pool was not materialized from the root");
    check(tiku_npu_llaton_test_release_resources() == TIKU_NPU_OK &&
              tiku_npu_llaton_test_get_resource_trace(&first_layout) ==
                  TIKU_NPU_OK &&
              first_layout.fixed_count == 0u &&
              first_layout.root_used == 0u,
          "fixed RESET pool teardown did not rewind the root");

    /* COPY and RESET descriptors may appear in either order.  Their ownership
     * records and root high-water layout must still be address sorted. */
    make_base(image);
    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x02010200));
    put32(image, desc0 + DESC_FOFF, UINT32_C(0));
    put32(image, desc0 + DESC_DST, UINT32_C(0x342d0000));
    put32(image, desc0 + DESC_SIZE, UINT32_C(0x20));
    rc = tiku_npu_llaton_test_materialize_pools(
        image, tiku_npu_stm32n6_identity_corrupt_len, 0u, 0u,
        params_offset, 0x200u, 0u);
    check(rc == TIKU_NPU_OK &&
              tiku_npu_llaton_test_get_resource_trace(&first_layout) ==
                  TIKU_NPU_OK &&
              first_layout.fixed_count == 2u &&
              first_layout.fixed_active_count == 2u &&
              first_layout.fixed_addr[0] == UINT32_C(0x342d0000) &&
              first_layout.fixed_addr[1] == UINT32_C(0x342e0000),
          "fixed pools were not materialized in destination order");
    check(tiku_npu_llaton_test_release_resources() == TIKU_NPU_OK,
          "first fixed-pool layout did not release");

    memcpy(saved_desc, image + desc0, DESC_BYTES);
    memcpy(image + desc0, image + desc1, DESC_BYTES);
    memcpy(image + desc1, saved_desc, DESC_BYTES);
    rc = tiku_npu_llaton_test_materialize_pools(
        image, tiku_npu_stm32n6_identity_corrupt_len, 0u, 0u,
        params_offset, 0x200u, 0u);
    check(rc == TIKU_NPU_OK &&
              tiku_npu_llaton_test_get_resource_trace(&second_layout) ==
                  TIKU_NPU_OK &&
              second_layout.fixed_count == first_layout.fixed_count &&
              second_layout.fixed_addr[0] == first_layout.fixed_addr[0] &&
              second_layout.fixed_addr[1] == first_layout.fixed_addr[1] &&
              second_layout.root_used == first_layout.root_used,
          "descriptor order changed the materialized pool layout");
    check(tiku_npu_llaton_test_release_resources() == TIKU_NPU_OK,
          "reordered fixed-pool layout did not release");
    make_base(image);

    put32(image, desc0 + DESC_FOFF, UINT32_C(0x300));
    put32(image, desc0 + DESC_SIZE, UINT32_C(4));
    check(validate(image, 0u, 0u, params_offset, 0u) == TIKU_NPU_ERR_HEADER,
          "combined foff was checked against the raw file start");
    make_base(image);

    check(validate(image, 0u, 0u, 0u, 0u) == TIKU_NPU_ERR_ARGUMENT,
          "unbounded split parameters were accepted");
    check(validate(image, (uintptr_t)image, 8u, 0u, 0u) == TIKU_NPU_OK,
          "bounded split parameters were rejected");
    make_base(image);

    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x01020601));
    put32(image, desc0 + DESC_SIZE, UINT32_C(0x20));
    check(validate(image, 0u, 0u, params_offset, 0x20u) == TIKU_NPU_OK,
          "root-backed relative activation pool was rejected");
    check(tiku_npu_llaton_test_ext_ram_reserve_attempts() == reserve_before &&
              tiku_npu_llaton_test_install_calls() == install_before,
          "external-RAM validation performed a reservation or install");
    make_base(image);

    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x01010102));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "unsupported relative pool ID was accepted");
    make_base(image);

    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x01020100));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "relative parameter pool with activation dtype was accepted");
    make_base(image);

    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x01018000));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_HEADER,
          "pool with unsupported attribute bits was accepted");
    make_base(image);

    put32(image, desc1 + DESC_FLAGS, UINT32_C(0x01010100));
    put32(image, desc1 + DESC_DST, UINT32_C(0));
    put32(image, desc1 + DESC_SIZE, UINT32_C(4));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "duplicate relative pool ID was accepted");
    make_base(image);

    put32(image, desc0 + DESC_DST, UINT32_C(0x34000000));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "relative pool with an absolute destination was accepted");
    make_base(image);

    put32(image, desc1 + DESC_DST, UINT32_C(0x34000000));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "RESET pool outside the NPU tier was accepted");
    make_base(image);

    put32(image, desc1 + DESC_DST, UINT32_C(0x34200000));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "fixed pool overlapping the executable slice was accepted");
    make_base(image);

    put32(image, desc1 + DESC_DST, UINT32_C(0x342e0004));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "misaligned fixed writable pool was accepted");
    make_base(image);

    put32(image, desc2, get32(image, desc1));
    put32(image, desc2 + DESC_FLAGS, UINT32_C(0x03020200));
    put32(image, desc2 + DESC_DST, UINT32_C(0x342e0000));
    put32(image, desc2 + DESC_SIZE, UINT32_C(0x20));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "overlapping fixed writable pools were accepted");
    make_base(image);

    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x02010200));
    put32(image, desc0 + DESC_DST,
          (uint32_t)(TIKU_OSPI_MMAP_BASE + TIKU_OSPI_MODEL_ADDR));
    put32(image, desc0 + DESC_SIZE, UINT32_C(4));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "COPY pool targeting XSPI was accepted");
    make_base(image);

    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x02010200));
    put32(image, desc0 + DESC_DST, UINT32_C(0x10000000));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "COPY pool outside runtime RAM was accepted");
    make_base(image);

    put32(image, desc0 + DESC_FLAGS, UINT32_C(0x02010200));
    put32(image, desc0 + DESC_DST, UINT32_C(0x343bfff0));
    put32(image, desc0 + DESC_SIZE, UINT32_C(0xffffffe0));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_ARGUMENT,
          "COPY destination span overflow was accepted");
    make_base(image);

    put32(image, desc0, UINT32_C(0x2000073f));
    memset(image + 0x73fu, 0xa5, sizeof image - 0x73fu);
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_HEADER,
          "unterminated pool name was accepted");
    make_base(image);

    put32(image, desc2, UINT32_C(0x20000163));
    put32(image, desc2 + DESC_FLAGS, UINT32_C(0));
    check(validate(image, 0u, 0u, params_offset, 0u) ==
              TIKU_NPU_ERR_HEADER,
          "malformed pool terminator was accepted");

    TIKU_PRINTF("NPU pool acceptance: result=%s\n",
                failures == 0u ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&npu_pool_test);

#endif /* TIKU_HAS_NPU && TIKU_NPU_POOL_TEST_ENABLE */
