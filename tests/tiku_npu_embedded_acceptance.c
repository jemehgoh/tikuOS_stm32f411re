/*
 * STM32N6 embedded-NPU acceptance process.
 *
 * This is opt-in because it owns the fixture buffers at the addresses emitted
 * by ST EdgeAI. The combined model is preloaded in the external-NOR-backed
 * /data/npu store; no multi-megabyte model table is linked into the image.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_EMBEDDED_TEST_ENABLE
#include <interfaces/npu/tiku_npu.h>
#include <kernel/process/tiku_process.h>
#include <kernel/vfs/tiku_vfs.h>

#include <arch/stm32n6/tiku_stm32n6_regs.h>
#include <arch/stm32n6/tiku_npu_arch.h>
#include <arch/stm32n6/tiku_npu_llaton.h>
#include <drivers/stm32n6/npu/models/stm32n6_network_model.h>

#include "ll_aton_rt_user_api.h"

TIKU_PROCESS(npu_embedded_owner, "NPU owner");
TIKU_PROCESS(npu_embedded_observer, "NPU observer");
TIKU_NPU_MODEL(tiku_npu_fixture_model);

static unsigned npu_embedded_failures;
static uint32_t npu_embedded_start;
static uint32_t npu_embedded_submit_cycles;
static uint32_t npu_embedded_sync_cycles;
static unsigned npu_embedded_done_events;

static void npu_embedded_check(int condition, const char *what)
{
    if (!condition) {
        npu_embedded_failures++;
        TIKU_PRINTF("NPU FAIL: %s\n", what);
    }
}

static uint32_t npu_embedded_cycles(void)
{
    return TIKU_REG32(STM32N6_DWT_CYCCNT);
}

static int npu_embedded_tensor_bytes(const tiku_npu_tensor_t *tensor,
                                     size_t *bytes)
{
    size_t total = 1u;

    if (tensor == NULL || bytes == NULL || tensor->rank == 0u) {
        return 0;
    }
    if (tensor->type != TIKU_NPU_TENSOR_INT8 &&
        tensor->type != TIKU_NPU_TENSOR_UINT8) {
        return 0;
    }
    for (uint8_t i = 0u; i < tensor->rank; i++) {
        if (tensor->shape[i] == 0u ||
            total > SIZE_MAX / (size_t)tensor->shape[i]) {
            return 0;
        }
        total *= (size_t)tensor->shape[i];
    }
    *bytes = total;
    return 1;
}

static void npu_embedded_fill(size_t input_bytes)
{
    volatile int8_t *input =
        (volatile int8_t *)TIKU_NPU_FIXTURE_INPUT_ADDRESS;

    /* Use a deterministic signed image-like INT8 input. */
    for (size_t i = 0u; i < input_bytes; i++) {
        input[i] = (int8_t)((i & 63U) - 32);
    }
}

static void npu_embedded_check_output(size_t output_bytes)
{
    volatile const int8_t *output =
        (volatile const int8_t *)TIKU_NPU_FIXTURE_OUTPUT_ADDRESS;
    int unchanged = 1;

    for (size_t i = 0u; i < output_bytes; i++) {
        if (output[i] != (int8_t)0x5a) {
            unchanged = 0;
            break;
        }
    }
    npu_embedded_check(!unchanged, "network output was not written");
}

PT_THREAD(tiku_process_thread_npu_embedded_observer(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    (void)data;
    TIKU_PROCESS_BEGIN();

    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
        /* A broadcast completion would enter this process. It must not. */
        npu_embedded_check(ev != TIKU_EVENT_NPU_DONE,
                           "NPU completion was broadcast");
    }

    TIKU_PROCESS_END();
}

PT_THREAD(tiku_process_thread_npu_embedded_owner(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    const void *inputs[] = {
        (const void *)TIKU_NPU_FIXTURE_INPUT_ADDRESS
    };
    void *outputs[] = {
        (void *)TIKU_NPU_FIXTURE_OUTPUT_ADDRESS
    };
    tiku_npu_model_io_t io;
    int rc;
    int model_loaded = 0;
    int npu_initialized = 0;
    int io_ok;
    size_t input_bytes = 0u;
    size_t output_bytes = 0u;
#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    tiku_npu_llaton_run_trace_t trace;
    int busy_rc;
#endif
#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE)
    uintptr_t exec_addr;
    uintptr_t ext_addr;
    uintptr_t param_addr;
    uint32_t exec_size;
    uint32_t required_exec_size;
    uint32_t requested_ext_size;
    size_t ext_size;
#endif

    (void)data;
    TIKU_PROCESS_BEGIN();

    rc = tiku_npu_init();
    npu_embedded_check(rc == TIKU_NPU_INIT_OK ||
                       rc == TIKU_NPU_INIT_ALREADY,
                       "application NPU initialization failed");
    if (rc != TIKU_NPU_INIT_OK && rc != TIKU_NPU_INIT_ALREADY) {
        goto npu_embedded_report;
    }
    npu_initialized = 1;

    rc = tiku_npu_model_bind(&tiku_npu_fixture_model,
                             TIKU_NPU_FIXTURE_MODEL_PATH);
    npu_embedded_check(rc == TIKU_NPU_OK,
                       "deployed network_rel.bin bind failed");
    if (rc != TIKU_NPU_OK) goto npu_embedded_report;

    rc = tiku_npu_model_load(&tiku_npu_fixture_model);
    npu_embedded_check(rc == TIKU_NPU_OK,
                       "deployed network_rel.bin load failed");
    if (rc != TIKU_NPU_OK) goto npu_embedded_report;
    model_loaded = 1;

    // rc = tiku_npu_model_io(&tiku_npu_fixture_model, &io);
    // io_ok = rc == TIKU_NPU_OK && io.input_count == 1u &&
    //         io.output_count == 1u &&
    //         io.inputs[0].type == TIKU_NPU_TENSOR_INT8 &&
    //         io.inputs[0].rank == 4u &&
    //         io.inputs[0].shape[0] == 1u &&
    //         io.inputs[0].shape[1] == 3u &&
    //         io.inputs[0].shape[2] == 192u &&
    //         io.inputs[0].shape[3] == 192u &&
    //         io.outputs[0].type == TIKU_NPU_TENSOR_INT8 &&
    //         io.outputs[0].rank == 3u &&
    //         io.outputs[0].shape[0] == 1u &&
    //         io.outputs[0].shape[1] == 5u &&
    //         io.outputs[0].shape[2] == 756u &&
    //         npu_embedded_tensor_bytes(&io.inputs[0], &input_bytes) &&
    //         npu_embedded_tensor_bytes(&io.outputs[0], &output_bytes);
    // npu_embedded_check(io_ok,
    //                    "deployed network I/O metadata mismatch");
    // if (!io_ok) goto npu_embedded_report;
    npu_embedded_check(input_bytes <= TIKU_NPU_FIXTURE_INPUT_BYTES,
                       "network input exceeds the fixture buffer");
    npu_embedded_check(output_bytes <= TIKU_NPU_FIXTURE_OUTPUT_BYTES,
                       "network output exceeds the fixture buffer");
    if (input_bytes > TIKU_NPU_FIXTURE_INPUT_BYTES ||
        output_bytes > TIKU_NPU_FIXTURE_OUTPUT_BYTES) {
        goto npu_embedded_report;
    }

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    tiku_npu_llaton_test_reset_run_trace();
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE)
    rc = tiku_npu_llaton_test_get_config(&exec_addr, &exec_size,
                                         &ext_addr, &ext_size, &param_addr,
                                         &required_exec_size,
                                         &requested_ext_size);
    npu_embedded_check(rc == TIKU_NPU_OK && exec_addr != 0u &&
                           (exec_addr & 7u) == 0u &&
                           exec_size == required_exec_size &&
                           requested_ext_size == 0u && ext_addr == 0u &&
                           ext_size == 0u && param_addr == 0u,
                       "combined model resource configuration mismatch");
#endif

    npu_embedded_fill(input_bytes);
    for (size_t i = 0u; i < output_bytes; i++) {
        ((volatile int8_t *)TIKU_NPU_FIXTURE_OUTPUT_ADDRESS)[i] = (int8_t)0x5a;
    }
    npu_embedded_start = npu_embedded_cycles();
    rc = tiku_npu_run(&tiku_npu_fixture_model, inputs, outputs);
    npu_embedded_submit_cycles = npu_embedded_cycles() - npu_embedded_start;
    npu_embedded_check(rc == TIKU_NPU_OK, "async submit failed");
    if (rc != TIKU_NPU_OK) goto npu_embedded_report;

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    rc = tiku_npu_llaton_test_get_run_trace(&trace);
    npu_embedded_check(rc == TIKU_NPU_OK && trace.submissions == 1u &&
                           trace.reset_calls == 1u &&
                           trace.reset_order_failures == 0u &&
                           trace.epoch_calls >= 1u,
                       "first inference did not reset before its first epoch");
    if (trace.last_submit_result != LL_ATON_RT_DONE) {
        busy_rc = tiku_npu_run(&tiku_npu_fixture_model, inputs, outputs);
        npu_embedded_check(busy_rc == TIKU_NPU_ERR_BUSY,
                           "second inference was accepted while running");
        busy_rc = tiku_npu_model_unload(&tiku_npu_fixture_model);
        npu_embedded_check(busy_rc == TIKU_NPU_ERR_BUSY,
                           "model unload was accepted while inference was running");
    }
#endif

    TIKU_PROCESS_WAIT_EVENT_UNTIL(ev == TIKU_EVENT_NPU_DONE);
    npu_embedded_done_events++;
    npu_embedded_check(tiku_event_npu_model(ev, data) ==
                           &tiku_npu_fixture_model,
                       "completion carried the wrong model");
    npu_embedded_check_output(output_bytes);

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    rc = tiku_npu_llaton_test_get_run_trace(&trace);
    npu_embedded_check(rc == TIKU_NPU_OK &&
                           trace.runtime_failures == 0u &&
                           trace.reset_order_failures == 0u,
                       "first inference reported a runtime or reset-order failure");
    if (trace.wfe_returns != 0u) {
        npu_embedded_check(trace.irq_wake_posts != 0u,
                           "WFE inference was not resumed by the IRQ bridge");
    }
    if (trace.no_wfe_returns != 0u) {
        npu_embedded_check(trace.worker_wake_posts != 0u,
                           "NO_WFE inference was not rescheduled by the worker");
    }
    {
        (void)tiku_process_post(TIKU_PROCESS_BROADCAST, TIKU_EVENT_NPU_WAKE,
                                NULL);
    }
    {
        uint32_t irq_wakes = trace.irq_wake_posts;
        tiku_npu_llaton_irq_bridge();
        rc = tiku_npu_llaton_test_get_run_trace(&trace);
        npu_embedded_check(rc == TIKU_NPU_OK &&
                               trace.irq_wake_posts == irq_wakes,
                           "idle IRQ bridge posted a stale worker wake");
    }
#endif

    npu_embedded_fill(input_bytes);
    for (size_t i = 0u; i < output_bytes; i++) {
        ((volatile int8_t *)TIKU_NPU_FIXTURE_OUTPUT_ADDRESS)[i] = (int8_t)0x5a;
    }
    npu_embedded_start = npu_embedded_cycles();
    tiku_npu_run_sync(&tiku_npu_fixture_model, inputs, outputs);
    npu_embedded_sync_cycles = npu_embedded_cycles() - npu_embedded_start;
    npu_embedded_done_events++;
    npu_embedded_check_output(output_bytes);

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    rc = tiku_npu_llaton_test_get_run_trace(&trace);
    npu_embedded_check(rc == TIKU_NPU_OK && trace.submissions == 2u &&
                           trace.reset_calls == 2u &&
                           trace.reset_order_failures == 0u &&
                           trace.runtime_failures == 0u &&
                           trace.completion_posts == 2u &&
                           trace.ignored_wake_events > 0u &&
                           tiku_npu_model_last_error(&tiku_npu_fixture_model) ==
                               TIKU_NPU_OK,
                       "second inference did not complete cleanly");
#endif
    npu_embedded_check(npu_embedded_done_events == 2u,
                       "inference completion was not delivered exactly twice");
npu_embedded_report:
    TIKU_PRINTF("NPU acceptance: submit=%lu sync=%lu result=%s\n",
           (unsigned long)npu_embedded_submit_cycles,
           (unsigned long)npu_embedded_sync_cycles,
           npu_embedded_failures == 0U ? "PASS" : "FAIL");
    if (model_loaded) {
        if (tiku_npu_model_unload(&tiku_npu_fixture_model) != TIKU_NPU_OK) {
            npu_embedded_check(0, "deployed network unload failed");
        } else {
            model_loaded = 0;
        }
    }
    if (npu_initialized) {
        rc = tiku_npu_shutdown();
        npu_embedded_check(rc == TIKU_NPU_OK,
                           "application NPU shutdown failed");
        if (rc == TIKU_NPU_OK) {
            rc = tiku_npu_model_bind(&tiku_npu_fixture_model,
                                     TIKU_NPU_FIXTURE_MODEL_PATH);
            npu_embedded_check(rc == TIKU_NPU_ERR_STATE,
                               "model bind succeeded before NPU restart");
            rc = tiku_npu_init();
            npu_embedded_check(rc == TIKU_NPU_INIT_OK,
                               "application NPU restart failed");
        }
    }
    while (1) {
        TIKU_PROCESS_WAIT_EVENT();
    }

    TIKU_PROCESS_END();
}

#if !(TIKU_NPU_VFS_TEST_ENABLE + 0)
TIKU_AUTOSTART_PROCESSES(&npu_embedded_owner, &npu_embedded_observer);
#endif

#endif /* TIKU_NPU_EMBEDDED_TEST_ENABLE */
