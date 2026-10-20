/* STM32N6 checked-region boundary acceptance process. */
#include <stdint.h>

#include <kernel/memory/tiku_mem.h>
#include <kernel/process/tiku_process.h>

#if defined(TIKU_STM32N6_REGION_TEST_ENABLE) && \
    TIKU_STM32N6_REGION_TEST_ENABLE

extern uint8_t __axisram_start;
extern uint8_t __tier_sram_end;
extern uint8_t __npu_reserved_start;
extern uint8_t __npu_reserved_end;

#if (TIKU_HAS_NPU + 0)
#include <arch/stm32n6/tiku_npu_mem.h>
#endif

TIKU_PROCESS(stm32n6_region_acceptance, "STM32N6 region acceptance");

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        TIKU_PRINTF("STM32N6 region FAIL: %s\n", message);
    }
}

PT_THREAD(tiku_process_thread_stm32n6_region_acceptance(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    uintptr_t axi_start;
    uintptr_t tier_end;
    uintptr_t npu_start;
    uintptr_t npu_end;
    tiku_mem_tier_t tier;
    tiku_mem_region_type_t type;
    tiku_arena_t arena = {0};
    tiku_mem_arch_size_t count = 0;
    const tiku_mem_region_t *table;

    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    axi_start = (uintptr_t)&__axisram_start;
    tier_end = (uintptr_t)&__tier_sram_end;
    npu_start = (uintptr_t)&__npu_reserved_start;
    npu_end = (uintptr_t)&__npu_reserved_end;
    table = tiku_region_arch_get_table(&count);

    check(table != NULL && count != 0U, "region table is unavailable");
    check(tiku_region_contains((const uint8_t *)axi_start, 32U,
                               TIKU_MEM_REGION_SRAM) != 0,
          "AXI SRAM start is not published as SRAM");

#if (TIKU_HAS_NPU + 0)
    check(npu_end > npu_start, "NPU reservation has no extent");
    check(npu_start >= axi_start && npu_end <= tier_end,
          "NPU reservation is outside the AXI tier");
    check(tiku_region_contains((const uint8_t *)npu_start, 32U,
                               TIKU_MEM_REGION_SRAM) == 0,
          "reserved NPU memory is visible as generic SRAM");
    check(tiku_region_get_type((const uint8_t *)npu_start, &type) !=
              TIKU_MEM_OK,
          "reserved NPU memory has a generic region type");
    check(tiku_region_claim((const uint8_t *)npu_start, 32U, 0xe1u) ==
              TIKU_MEM_ERR_INVALID,
          "reserved NPU memory can be claimed generically");
    check(tiku_arena_create(&arena, (uint8_t *)npu_start, 32U, 0xe2u) ==
              TIKU_MEM_ERR_INVALID,
          "checked arena creation accepted reserved NPU memory");

    if (npu_start >= axi_start + 32U) {
        check(tiku_region_contains((const uint8_t *)(npu_start - 32U), 32U,
                                   TIKU_MEM_REGION_SRAM) != 0,
              "generic SRAM does not reach the NPU boundary");
    }
    if (npu_start >= axi_start + 16U) {
        check(tiku_region_contains((const uint8_t *)(npu_start - 16U), 32U,
                                   TIKU_MEM_REGION_SRAM) == 0,
              "a range crossing into NPU memory was accepted");
    }
    check(tiku_stm32n6_npu_mem_root() != NULL,
          "boot-owned NPU root is unavailable");
    check(tiku_tier_get((const uint8_t *)npu_start, &tier) == TIKU_MEM_OK &&
              tier == TIKU_MEM_SRAM,
          "NPU root is not classified as SRAM by the tier allocator");
#else
    check(npu_start == npu_end, "NPU-disabled build has a nonzero carve");
    check(tier_end >= axi_start + 32U &&
              tiku_region_contains((const uint8_t *)(tier_end - 32U), 32U,
                                   TIKU_MEM_REGION_SRAM) != 0,
          "NPU-disabled build does not publish the full AXI SRAM span");
#endif

    TIKU_PRINTF("STM32N6 region acceptance: result=%s\n",
                failures == 0U ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&stm32n6_region_acceptance);

#endif /* TIKU_STM32N6_REGION_TEST_ENABLE */
