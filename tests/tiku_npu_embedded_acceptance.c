/*
 * STM32N6 embedded-NPU acceptance process.
 *
 * This is opt-in because it owns the fixture buffers at the addresses emitted
 * by ST EdgeAI. The combined model is preloaded in the external-NOR-backed
 * /data/npu store; no multi-megabyte model table is linked into the image.
 */

#include <stdio.h>
#include <stdint.h>

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_EMBEDDED_TEST_ENABLE
#include <interfaces/npu/tiku_npu.h>
#include <kernel/process/tiku_process.h>
#include <kernel/vfs/tiku_vfs.h>

#include <arch/stm32n6/tiku_stm32n6_regs.h>
#include <drivers/stm32n6/npu/models/stm32n6_network_model.h>

TIKU_PROCESS(npu_embedded_owner, "NPU owner");
TIKU_PROCESS(npu_embedded_observer, "NPU observer");
TIKU_NPU_MODEL(tiku_npu_fixture_model);

static unsigned npu_embedded_failures;
static uint32_t npu_embedded_start;
static uint32_t npu_embedded_submit_cycles;
static uint32_t npu_embedded_sync_cycles;

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

static void npu_embedded_fill(void)
{
    volatile int8_t *input =
        (volatile int8_t *)TIKU_NPU_FIXTURE_INPUT_ADDRESS;

    /* Use a deterministic signed image-like INT8 input. */
    for (unsigned i = 0U; i < TIKU_NPU_FIXTURE_INPUT_BYTES; i++) {
        input[i] = (int8_t)((i & 63U) - 32);
    }
}

static void npu_embedded_check_output(void)
{
    volatile const int8_t *output =
        (volatile const int8_t *)TIKU_NPU_FIXTURE_OUTPUT_ADDRESS;
    int unchanged = 1;

    for (unsigned i = 0U; i < TIKU_NPU_FIXTURE_OUTPUT_BYTES; i++) {
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
    int io_ok;

    (void)data;
    TIKU_PROCESS_BEGIN();

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
                           io.output_count == 1u &&
                           io.inputs[0].type == TIKU_NPU_TENSOR_INT8 &&
                           io.inputs[0].shape[0] == 1u &&
                           io.inputs[0].shape[1] == 3u &&
                           io.inputs[0].shape[2] == 256u &&
                           io.inputs[0].shape[3] == 256u &&
                           io.outputs[0].type == TIKU_NPU_TENSOR_INT8 &&
                           io.outputs[0].shape[0] == 1u &&
                           io.outputs[0].shape[1] == 5u &&
                           io.outputs[0].shape[2] == 1344u;
    npu_embedded_check(io_ok,
                       "deployed network I/O metadata mismatch");
    if (!io_ok) goto npu_embedded_report;

    npu_embedded_fill();
    for (unsigned i = 0U; i < TIKU_NPU_FIXTURE_OUTPUT_BYTES; i++) {
        ((volatile int8_t *)TIKU_NPU_FIXTURE_OUTPUT_ADDRESS)[i] = (int8_t)0x5a;
    }
    npu_embedded_start = npu_embedded_cycles();
    rc = tiku_npu_run(&tiku_npu_fixture_model, inputs, outputs);
    npu_embedded_submit_cycles = npu_embedded_cycles() - npu_embedded_start;
    npu_embedded_check(rc == TIKU_NPU_OK, "async submit failed");
    if (rc != TIKU_NPU_OK) goto npu_embedded_report;

    TIKU_PROCESS_WAIT_EVENT_UNTIL(ev == TIKU_EVENT_NPU_DONE);
    npu_embedded_check(tiku_event_npu_model(ev, data) ==
                           &tiku_npu_fixture_model,
                       "completion carried the wrong model");
    npu_embedded_check_output();

    npu_embedded_fill();
    for (unsigned i = 0U; i < TIKU_NPU_FIXTURE_OUTPUT_BYTES; i++) {
        ((volatile int8_t *)TIKU_NPU_FIXTURE_OUTPUT_ADDRESS)[i] = (int8_t)0x5a;
    }
    npu_embedded_start = npu_embedded_cycles();
    tiku_npu_run_sync(&tiku_npu_fixture_model, inputs, outputs);
    npu_embedded_sync_cycles = npu_embedded_cycles() - npu_embedded_start;
    npu_embedded_check_output();
npu_embedded_report:
    TIKU_PRINTF("NPU acceptance: submit=%lu sync=%lu result=%s\n",
           (unsigned long)npu_embedded_submit_cycles,
           (unsigned long)npu_embedded_sync_cycles,
           npu_embedded_failures == 0U ? "PASS" : "FAIL");
    if (model_loaded && tiku_npu_model_unload(&tiku_npu_fixture_model) != TIKU_NPU_OK) {
        npu_embedded_check(0, "deployed network unload failed");
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
