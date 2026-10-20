/* STM32N6 bounded LL-ATON relocatable-image preflight acceptance process. */
#include <stdint.h>
#include <string.h>

#include <kernel/process/tiku_process.h>

#include <arch/stm32n6/tiku_npu_llaton.h>
#include "npu/fixtures/stm32n6_identity_corrupt.network_rel.h"

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_PREFLIGHT_TEST_ENABLE

#define IMAGE_WORD(_n) ((size_t)(_n) * 4u)
#define IMAGE_MASK UINT32_C(0x0fffffff)

TIKU_PROCESS(npu_preflight_test, "NPU preflight test");

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        TIKU_PRINTF("NPU preflight FAIL: %s\n", message);
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

static void make_valid(uint8_t *image)
{
    memcpy(image, tiku_npu_stm32n6_identity_corrupt,
           tiku_npu_stm32n6_identity_corrupt_len);
    /* The checked-in fixture differs from a valid image only at this site. */
    put32(image, raw_offset(image, 9u), UINT32_C(0x40000000));
}

static int preflight(const uint8_t *image, size_t length)
{
    return tiku_npu_llaton_test_image_preflight(image, length, 0u, 0u);
}

PT_THREAD(tiku_process_thread_npu_preflight_test(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    static uint8_t image[1856] __attribute__((aligned(8)));
    size_t data_data;
    size_t params_at;
    size_t rel_start;
    size_t params_offset;
    size_t i;

    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    make_valid(image);
    data_data = raw_offset(image, 4u);
    params_at = data_data + raw_offset(image, 11u);
    rel_start = raw_offset(image, 9u);
    params_offset = (size_t)get32(image, IMAGE_WORD(12u));

    check(preflight(image, sizeof image) == 1,
          "valid combined image was rejected");

    put32(image, IMAGE_WORD(12u), UINT32_C(0));
    check(preflight(image, sizeof image) == 0,
          "split image without parameter bounds was accepted");
    check(tiku_npu_llaton_test_image_preflight(
              image, sizeof image, (uintptr_t)4u,
              sizeof image - params_offset) == 1,
          "bounded split parameter image was rejected by preflight");
    make_valid(image);

    check(preflight(image, rel_start - 1u) == 0,
          "truncated relocation table was accepted");

    put32(image, IMAGE_WORD(0u), UINT32_C(0));
    check(preflight(image, sizeof image) == 0,
          "bad magic was accepted");
    make_valid(image);

    put32(image, IMAGE_WORD(3u), UINT32_C(0x40000744));
    check(preflight(image, sizeof image) == 0,
          "out-of-range section endpoint was accepted");
    make_valid(image);

    put32(image, IMAGE_WORD(5u), UINT32_C(0x40000100));
    check(preflight(image, sizeof image) == 0,
          "invalid section ordering was accepted");
    make_valid(image);

    put32(image, IMAGE_WORD(25u), UINT32_C(0x40000700));
    check(preflight(image, sizeof image) == 0,
          "out-of-range context was accepted");
    make_valid(image);

    put32(image, data_data + (raw_offset(image, 25u)) + 20u,
          UINT32_C(0x2000073f));
    memset(image + 0x73fu, 0xa5, sizeof image - 0x73fu);
    check(preflight(image, sizeof image) == 0,
          "unterminated context string was accepted");
    make_valid(image);

    put32(image, params_at + 4u, UINT32_C(0));
    check(preflight(image, sizeof image) == 0,
          "half-zero pool terminator was accepted");
    make_valid(image);

    put32(image, IMAGE_WORD(11u), UINT32_C(0x40000070));
    params_at = data_data + 0x70u;
    for (i = 0u; i < 10u; i++) {
        put32(image, params_at + i * 20u, UINT32_C(0x20000163));
        put32(image, params_at + i * 20u + 4u, UINT32_C(0x01010100));
    }
    check(preflight(image, sizeof image) == 0,
          "unterminated pool descriptor table was accepted");
    make_valid(image);
    params_at = data_data + raw_offset(image, 11u);

    put32(image, params_at, UINT32_C(0x2000073f));
    memset(image + 0x73fu, 0xa5, sizeof image - 0x73fu);
    check(preflight(image, sizeof image) == 0,
          "out-of-range pool name was accepted");
    make_valid(image);

    put32(image, params_at + 8u,
          (uint32_t)(sizeof image - params_offset - 4u));
    put32(image, params_at + 16u, UINT32_C(8));
    check(preflight(image, sizeof image) == 0,
          "initializer span past parameter image was accepted");
    make_valid(image);

    put32(image, params_at + 16u, UINT32_MAX);
    check(preflight(image, sizeof image) == 0,
          "initializer rounding overflow was accepted");
    make_valid(image);

    put32(image, IMAGE_WORD(13u), UINT32_C(0x40000080));
    check(preflight(image, sizeof image) == 0,
          "invalid entry-point address class was accepted");
    make_valid(image);

    put32(image, IMAGE_WORD(13u), UINT32_C(0x20000083));
    check(preflight(image, sizeof image) == 0,
          "misaligned Thumb entry point was accepted");
    make_valid(image);

    put32(image, IMAGE_WORD(13u), UINT32_C(0x20000fff));
    check(preflight(image, sizeof image) == 0,
          "out-of-range entry point was accepted");
    make_valid(image);

    put32(image, rel_start, UINT32_C(0x10000000));
    check(preflight(image, sizeof image) == -1,
          "invalid relocation address class was not reported");
    make_valid(image);

    put32(image, rel_start, UINT32_C(0x40000002));
    check(preflight(image, sizeof image) == -1,
          "misaligned relocation site was not reported");
    make_valid(image);

    put32(image, rel_start, UINT32_C(0x4000073f));
    check(preflight(image, sizeof image) == -1,
          "out-of-range relocation site was not reported");

    check(tiku_npu_llaton_test_image_preflight(
              tiku_npu_stm32n6_identity_corrupt,
              tiku_npu_stm32n6_identity_corrupt_len, 0u, 0u) == -1,
          "corrupt relocation fixture was accepted");

    TIKU_PRINTF("NPU preflight acceptance: result=%s\n",
                failures == 0u ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&npu_preflight_test);

#endif /* TIKU_HAS_NPU && TIKU_NPU_PREFLIGHT_TEST_ENABLE */
