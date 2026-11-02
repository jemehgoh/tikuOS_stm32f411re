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
#include <arch/stm32n6/tiku_cache_arch.h>
#include <arch/stm32n6/tiku_n6_model_store.h>
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

/* The network was exported with its I/O inside the AXISRAM5 activation pool,
 * so the buffers are owned by the model: fetch them after load and pass NULL
 * to tiku_npu_run(). */
static int8_t *npu_embedded_in;
static int8_t *npu_embedded_out[TIKU_NPU_FIXTURE_OUTPUT_COUNT];
static size_t npu_embedded_out_bytes[TIKU_NPU_FIXTURE_OUTPUT_COUNT];
static unsigned npu_embedded_out_count;

static void npu_embedded_fill(size_t input_bytes)
{
    /* Use a deterministic signed image-like INT8 input. */
    for (size_t i = 0u; i < input_bytes; i++) {
        npu_embedded_in[i] = (int8_t)((i & 63U) - 32);
    }
    tiku_stm32n6_dcache_clean(npu_embedded_in, input_bytes);
}

/* Poison every output so an untouched buffer is detectable after the run. */
static void npu_embedded_prime_outputs(void)
{
    for (unsigned o = 0u; o < npu_embedded_out_count; o++) {
        for (size_t i = 0u; i < npu_embedded_out_bytes[o]; i++) {
            npu_embedded_out[o][i] = (int8_t)0x5a;
        }
        tiku_stm32n6_dcache_clean(npu_embedded_out[o],
                                  npu_embedded_out_bytes[o]);
    }
}

static void npu_embedded_check_outputs(void)
{
    for (unsigned o = 0u; o < npu_embedded_out_count; o++) {
        int unchanged = 1;

        tiku_stm32n6_dcache_invalidate(npu_embedded_out[o],
                                       npu_embedded_out_bytes[o]);
        for (size_t i = 0u; i < npu_embedded_out_bytes[o]; i++) {
            if (npu_embedded_out[o][i] != (int8_t)0x5a) {
                unchanged = 0;
                break;
            }
        }
        if (unchanged) {
            TIKU_PRINTF("NPU output %u was not written\n", o);
        }
        npu_embedded_check(!unchanged, "network output was not written");
    }
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
    /* Protothread locals do not survive a WAIT_EVENT: anything used after
     * one is static. */
    static const void *inputs[1];
    static void *outputs[TIKU_NPU_FIXTURE_OUTPUT_COUNT];
    tiku_npu_model_io_t io;
    int rc;
    static int model_loaded;
    static int npu_initialized;
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
    tiku_n6_model_image_t model_image;
    uintptr_t expected_param;
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

    rc = tiku_npu_model_io(&tiku_npu_fixture_model, &io);
    io_ok = rc == TIKU_NPU_OK && io.input_count == 1u &&
            io.output_count == TIKU_NPU_FIXTURE_OUTPUT_COUNT &&
            io.inputs[0].type == TIKU_NPU_TENSOR_INT8 &&
            io.inputs[0].rank == 4u &&
            io.inputs[0].shape[0] == 1u &&
            io.inputs[0].shape[1] == TIKU_NPU_FIXTURE_INPUT_CHANNELS &&
            io.inputs[0].shape[2] == TIKU_NPU_FIXTURE_INPUT_HEIGHT &&
            io.inputs[0].shape[3] == TIKU_NPU_FIXTURE_INPUT_WIDTH &&
            npu_embedded_tensor_bytes(&io.inputs[0], &input_bytes) &&
            input_bytes == TIKU_NPU_FIXTURE_INPUT_BYTES;
    npu_embedded_out_count = 0u;
    for (unsigned o = 0u; io_ok && o < io.output_count; o++) {
        const tiku_npu_tensor_t *tn = &io.outputs[o];

        io_ok = tn->type == TIKU_NPU_TENSOR_INT8 &&
                npu_embedded_tensor_bytes(tn, &output_bytes) &&
                output_bytes <= TIKU_NPU_FIXTURE_OUTPUT_MAX_BYTES;
        if (io_ok) {
            npu_embedded_out_bytes[o] = output_bytes;
            npu_embedded_out_count++;
            npu_embedded_out[o] = tiku_npu_model_io_buffer(
                &tiku_npu_fixture_model, 1, (uint16_t)o);
            io_ok = npu_embedded_out[o] != NULL;
            outputs[o] = NULL;
        }
        TIKU_PRINTF("NPU io out%u: rank=%u shape=[%u,%u,%u,%u] bytes=%u zp=%d\n",
                    o, (unsigned)tn->rank, (unsigned)tn->shape[0],
                    (unsigned)tn->shape[1], (unsigned)tn->shape[2],
                    (unsigned)tn->shape[3], (unsigned)output_bytes,
                    (int)tn->zero_point);
    }
    npu_embedded_check(io_ok,
                       "deployed network I/O metadata mismatch");
    if (!io_ok) goto npu_embedded_report;
    npu_embedded_in = tiku_npu_model_io_buffer(&tiku_npu_fixture_model, 0, 0u);
    npu_embedded_check(npu_embedded_in != NULL,
                       "model-owned input buffer is unavailable");
    if (npu_embedded_in == NULL) goto npu_embedded_report;
    inputs[0] = NULL;

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
    tiku_npu_llaton_test_reset_run_trace();
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE)
    rc = tiku_npu_llaton_test_get_config(&exec_addr, &exec_size,
                                         &ext_addr, &ext_size, &param_addr,
                                         &required_exec_size,
                                         &requested_ext_size);
    /* A combined image keeps its parameters inside the mapped model slot:
     * the runtime is handed file_ptr + the header's params_offset (word 12). */
    expected_param = 0u;
    if (tiku_n6_model_store_resolve(TIKU_NPU_FIXTURE_MODEL_PATH,
                                    &model_image) == TIKU_NPU_OK) {
        expected_param = model_image.file_ptr +
            *(const uint32_t *)(uintptr_t)(model_image.file_ptr + 48u);
    }
    npu_embedded_check(rc == TIKU_NPU_OK && exec_addr != 0u &&
                           (exec_addr & 7u) == 0u &&
                           exec_size == required_exec_size &&
                           requested_ext_size == 0u && ext_addr == 0u &&
                           ext_size == 0u && expected_param != 0u &&
                           param_addr == expected_param,
                       "combined model resource configuration mismatch");
#endif

    npu_embedded_fill(input_bytes);
    npu_embedded_prime_outputs();
    npu_embedded_start = npu_embedded_cycles();
    rc = tiku_npu_run(&tiku_npu_fixture_model, inputs, outputs);
    npu_embedded_submit_cycles = npu_embedded_cycles() - npu_embedded_start;
    if (rc != TIKU_NPU_OK) {
        TIKU_PRINTF("NPU run rc=%d\n", rc);
    }
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
    npu_embedded_check_outputs();

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
    npu_embedded_prime_outputs();
    npu_embedded_start = npu_embedded_cycles();
    tiku_npu_run_sync(&tiku_npu_fixture_model, inputs, outputs);
    npu_embedded_sync_cycles = npu_embedded_cycles() - npu_embedded_start;
    npu_embedded_done_events++;
    npu_embedded_check_outputs();

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
