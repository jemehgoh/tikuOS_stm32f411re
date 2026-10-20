/* STM32N6 boot-owned NPU root arena acceptance process. */
#include <stdint.h>

#include <kernel/memory/tiku_mem.h>
#include <kernel/process/tiku_process.h>

#include <arch/stm32n6/tiku_npu_mem.h>

extern uint8_t __npu_reserved_start;
extern uint8_t __npu_reserved_end;

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_MEM_TEST_ENABLE

TIKU_PROCESS(npu_mem_acceptance, "NPU memory acceptance");

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        TIKU_PRINTF("NPU memory FAIL: %s\n", message);
    }
}

static unsigned root_reservation_count(const tiku_arena_t *root,
                                       tiku_mem_arch_size_t *length)
{
    uint16_t cursor = 0;
    unsigned count = 0;
    tiku_mem_reservation_info_t info;

    if (length != NULL) *length = 0;
    while (tiku_mem_reservation_next(&cursor, &info) == TIKU_MEM_OK) {
        if (root != NULL && info.handle.generation == root->backing.generation &&
            info.handle.slot_plus_one == root->backing.slot_plus_one) {
            count++;
            if (length != NULL) *length = info.length;
        }
    }
    return count;
}

#if defined(TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE) && \
    TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE
static void run_negative_placement_test(void)
{
    tiku_arena_t consumer = {0};
    tiku_mem_err_t err;

    /* This is an isolated destructive test image: tier_reset() invalidates
     * the boot root, then a prior high-end SRAM consumer forces the next root
     * reservation away from the linker contract. */
    err = tiku_tier_reset();
    check(err == TIKU_MEM_OK, "tier reset failed for negative test");
    if (err != TIKU_MEM_OK) return;

    err = tiku_tier_arena_create(&consumer, TIKU_MEM_SRAM, 64u, 0x43u);
    check(err == TIKU_MEM_OK, "pre-existing SRAM consumer was not created");
    if (err != TIKU_MEM_OK) return;

    err = tiku_stm32n6_npu_mem_reserve();
    check(err == TIKU_MEM_ERR_INVALID,
          "misplaced root reservation was accepted");
    check(tiku_stm32n6_npu_mem_root() == NULL,
          "failed root reservation remained published");

    (void)tiku_arena_destroy(&consumer);
}
#endif

PT_THREAD(tiku_process_thread_npu_mem_acceptance(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    const tiku_arena_t *root;
    const tiku_arena_t *same_root;
    tiku_mem_stats_t root_stats;
    tiku_mem_stats_t tier_stats;
    tiku_mem_arch_size_t reservation_length = 0;
    uintptr_t expected_start;
    uintptr_t expected_end;
    uintptr_t root_start;
    uintptr_t root_end;
    void *first;
    void *second;
    void *fixed;
    tiku_mem_arch_size_t used_before;
    tiku_mem_err_t err;

    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    root = tiku_stm32n6_npu_mem_root();
    expected_start = (uintptr_t)&__npu_reserved_start;
    expected_end = (uintptr_t)&__npu_reserved_end;
    check(root != NULL, "boot did not publish the NPU root");
    if (root == NULL) goto report;

    root_start = (uintptr_t)root->buf;
    root_end = root_start + (uintptr_t)root->capacity;
    check(root_start == expected_start && root_end == expected_end,
          "root arena does not match linker reservation");
    check(root->capacity == (tiku_mem_arch_size_t)
              TIKU_NPU_SRAM_RESERVE_SIZE,
          "root capacity does not match configured reservation");
    check(((root_start | root_end) & 31U) == 0U,
          "root boundaries are not cache-line aligned");

    check(root_reservation_count(root, &reservation_length) == 1u &&
              reservation_length == root->capacity,
          "root reservation record is missing or has the wrong size");
    check(tiku_arena_stats(root, &root_stats) == TIKU_MEM_OK &&
              root_stats.used_bytes == 0u,
          "root arena was not empty at acceptance start");
    check(tiku_tier_stats(TIKU_MEM_SRAM, &tier_stats) == TIKU_MEM_OK &&
              tier_stats.used_bytes >= root->capacity,
          "SRAM tier does not charge the root reservation");

    first = tiku_stm32n6_npu_mem_alloc(17u, 64u);
    second = tiku_stm32n6_npu_mem_alloc(33u, 32u);
    check(first != NULL && ((uintptr_t)first & 63U) == 0U,
          "64-byte aligned root allocation failed");
    check(second != NULL && ((uintptr_t)second & 31U) == 0U,
          "32-byte aligned root allocation failed");
    check(tiku_stm32n6_npu_mem_alloc(root->capacity, 32u) == NULL,
          "oversized root allocation was accepted");

    err = tiku_stm32n6_npu_mem_reset();
    check(err == TIKU_MEM_OK, "root reset failed");
    check(tiku_stm32n6_npu_mem_root() == root,
          "root control block changed during reset");
    check(tiku_arena_stats(root, &root_stats) == TIKU_MEM_OK &&
              root_stats.used_bytes == 0u,
          "root reset did not rewind child allocations");
    check(tiku_tier_stats(TIKU_MEM_SRAM, &tier_stats) == TIKU_MEM_OK &&
              tier_stats.used_bytes >= root->capacity,
          "root reset released the boot reservation");

    first = tiku_stm32n6_npu_mem_alloc(32u, 32u);
    fixed = tiku_stm32n6_npu_mem_alloc_at(root_start + 0x100u, 33u, 32u);
    check(first == (void *)root_start &&
              fixed == (void *)(root_start + 0x100u),
          "exact allocation did not preserve the requested address");
    check(tiku_arena_stats(root, &root_stats) == TIKU_MEM_OK &&
              root_stats.used_bytes == 0x140u,
          "exact allocation did not charge padding and rounded size");
    used_before = root_stats.used_bytes;
    check(tiku_stm32n6_npu_mem_alloc_at(root_start + 0x80u,
                                        32u, 32u) == NULL,
          "backward exact allocation was accepted");
    check(tiku_stm32n6_npu_mem_alloc_at(root_start + 0x141u,
                                        32u, 32u) == NULL,
          "misaligned exact allocation was accepted");
    check(tiku_stm32n6_npu_mem_alloc_at(root_end, 32u, 32u) == NULL,
          "out-of-range exact allocation was accepted");
    check(tiku_stm32n6_npu_mem_alloc_at(root_start + 0x160u,
                                        32u, 16u) == NULL,
          "sub-cache-line exact alignment was accepted");
    check(tiku_arena_stats(root, &root_stats) == TIKU_MEM_OK &&
              root_stats.used_bytes == used_before,
          "failed exact allocation changed the root cursor");

    err = tiku_stm32n6_npu_mem_reset();
    check(err == TIKU_MEM_OK &&
              tiku_stm32n6_npu_mem_alloc(32u, 32u) == first &&
              tiku_stm32n6_npu_mem_alloc_at(root_start + 0x100u,
                                             33u, 32u) == fixed,
          "reset did not reproduce the exact layout");
    check(tiku_stm32n6_npu_mem_reset() == TIKU_MEM_OK,
          "final exact-layout reset failed");

    same_root = tiku_stm32n6_npu_mem_root();
    err = tiku_stm32n6_npu_mem_reserve();
    check(err == TIKU_MEM_OK && same_root == tiku_stm32n6_npu_mem_root(),
          "repeated reserve was not idempotent");

#if defined(TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE) && \
    TIKU_NPU_MEM_NEGATIVE_TEST_ENABLE
    run_negative_placement_test();
#endif

report:
    TIKU_PRINTF("NPU memory acceptance: result=%s\n",
                failures == 0u ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&npu_mem_acceptance);

#endif /* TIKU_HAS_NPU && TIKU_NPU_MEM_TEST_ENABLE */
