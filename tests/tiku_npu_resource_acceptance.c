/* STM32N6 LL-ATON runtime-resource reservation acceptance process. */
#include <stdint.h>

#include <interfaces/npu/tiku_npu.h>
#include <kernel/memory/tiku_mem.h>
#include <kernel/process/tiku_process.h>

#include <arch/stm32n6/tiku_npu_llaton.h>
#include <arch/stm32n6/tiku_npu_arch.h>
#include <arch/stm32n6/tiku_npu_mem.h>

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_RESOURCE_TEST_ENABLE

TIKU_PROCESS(npu_resource_test, "NPU resource test");

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        TIKU_PRINTF("NPU resource FAIL: %s\n", message);
    }
}

PT_THREAD(tiku_process_thread_npu_resource_test(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    uintptr_t exec_addr;
    uintptr_t ext_addr;
    uintptr_t config_exec_addr;
    uintptr_t config_ext_addr;
    uintptr_t config_param_addr;
    uint32_t config_exec_size;
    uint32_t required_exec_size;
    uint32_t requested_ext_size;
    size_t config_ext_size;
    unsigned attempts;
    unsigned installs;
    tiku_npu_llaton_resource_trace_t trace;
    const tiku_arena_t *root;
    uintptr_t root_end;
    int rc;

    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    rc = tiku_npu_init();
    check(rc == TIKU_NPU_INIT_OK || rc == TIKU_NPU_INIT_ALREADY,
          "application NPU initialization failed");
    if (rc != TIKU_NPU_INIT_OK && rc != TIKU_NPU_INIT_ALREADY) {
        goto npu_resource_report;
    }
    root = tiku_stm32n6_npu_mem_root();
    check(root != NULL, "boot-owned NPU root is unavailable");
    if (root == NULL) goto npu_resource_report;
    root_end = (uintptr_t)root->buf + (uintptr_t)root->capacity;

    exec_addr = 0u;
    ext_addr = 0u;
    /* A non-multiple-of-eight request is valid: the LL-ATON requirement is
     * on the executable base address, while the tier allocator rounds the
     * backing capacity. */
    rc = tiku_npu_llaton_test_reserve_resources(66u, 0u,
                                                &exec_addr, &ext_addr);
    check(rc == TIKU_NPU_OK && exec_addr != 0u &&
              (exec_addr & 7u) == 0u && ext_addr == 0u,
          "zero-external request did not reserve aligned executable RAM");
    rc = tiku_npu_llaton_test_get_config(
        &config_exec_addr, &config_exec_size, &config_ext_addr,
        &config_ext_size, &config_param_addr, &required_exec_size,
        &requested_ext_size);
    check(rc == TIKU_NPU_OK && config_exec_addr == exec_addr &&
              config_exec_size == 66u && config_ext_addr == 0u &&
              config_ext_size == 0u && config_param_addr == 0u &&
              required_exec_size == 66u && requested_ext_size == 0u,
          "COPY relocation config did not preserve inspected resource sizes");

    attempts = tiku_npu_llaton_test_ext_ram_reserve_attempts();
    installs = tiku_npu_llaton_test_install_calls();
    rc = tiku_npu_llaton_test_reserve_resources(64u, 1024u,
                                                &exec_addr, &ext_addr);
    check(rc == TIKU_NPU_OK && exec_addr != 0u && ext_addr != 0u &&
              (exec_addr & 31u) == 0u && (ext_addr & 31u) == 0u &&
              exec_addr >= (uintptr_t)root->buf &&
              ext_addr + 1024u <= root_end &&
              exec_addr + 64u <= ext_addr &&
              tiku_npu_llaton_test_ext_ram_reserve_attempts() == attempts + 1u &&
              tiku_npu_llaton_test_install_calls() == installs,
          "relative activation RAM was not allocated from the NPU root");
    rc = tiku_npu_llaton_test_get_config(
        &config_exec_addr, &config_exec_size, &config_ext_addr,
        &config_ext_size, &config_param_addr, &required_exec_size,
        &requested_ext_size);
    check(rc == TIKU_NPU_OK && config_exec_addr == exec_addr &&
              config_exec_size == 64u && config_ext_addr == ext_addr &&
              config_ext_size == 1024u && requested_ext_size == 1024u,
          "relative activation configuration did not match root backing");
    check(tiku_npu_llaton_test_get_resource_trace(&trace) == TIKU_NPU_OK &&
              trace.exec_active != 0u && trace.ext_active != 0u &&
              trace.fixed_count == 0u && trace.fixed_active_count == 0u &&
              trace.root_used ==
                  (size_t)(ext_addr + 1024u - (uintptr_t)root->buf),
          "dynamic resource trace did not match the root transaction");

    rc = tiku_npu_llaton_test_reserve_resources(
        (uint32_t)TIKU_NPU_MODEL_SLOT_BYTES + 8u, 0u, &exec_addr, &ext_addr);
    check(rc == TIKU_NPU_ERR_CAPACITY,
          "oversized executable request was accepted");

    /* Failed oversized requests must not strand the root bump pointer. */
    rc = tiku_npu_llaton_test_reserve_resources(66u, 0u,
                                                &exec_addr, &ext_addr);
    check(rc == TIKU_NPU_OK && (exec_addr & 7u) == 0u,
          "resource rollback leaked executable capacity");

 npu_resource_report:
    (void)tiku_npu_llaton_test_release_resources();
    TIKU_PRINTF("NPU resource acceptance: result=%s\n",
                failures == 0u ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

#if !(TIKU_NPU_EMBEDDED_TEST_ENABLE + 0)
TIKU_AUTOSTART_PROCESSES(&npu_resource_test);
#endif

#endif /* TIKU_HAS_NPU && TIKU_NPU_RESOURCE_TEST_ENABLE */
