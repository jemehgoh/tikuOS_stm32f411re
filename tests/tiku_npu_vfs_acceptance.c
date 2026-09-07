/* STM32N6 LL-ATON VFS acceptance process. */
#include <stdint.h>
#include <string.h>

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_VFS_TEST_ENABLE
#include <interfaces/npu/tiku_npu.h>
#include <kernel/process/tiku_process.h>
#include <kernel/vfs/tiku_vfs.h>
#include <drivers/stm32n6/npu/models/stm32n6_network_model.h>
#include <tests/npu/fixtures/stm32n6_identity_corrupt.network_rel.h>

#ifndef TIKU_NPU_VFS_TEST_MODEL_PATH
#define TIKU_NPU_VFS_TEST_MODEL_PATH TIKU_NPU_FIXTURE_MODEL_PATH
#endif

#define TIKU_NPU_BAD_PATH "/data/npu/bad.bin"
#define TIKU_NPU_CORRUPT_PATH "/data/npu/corrupt.bin"

TIKU_PROCESS(npu_vfs_test, "NPU VFS test");
TIKU_NPU_MODEL(npu_vfs_model);

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        TIKU_PRINTF("NPU VFS FAIL: %s\n", message);
    }
}

PT_THREAD(tiku_process_thread_npu_vfs_test(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    uint8_t bad[128] = {0};
    uint8_t source_before[2048];
    uint8_t source_after[2048];
    tiku_npu_model_io_t io;
    int source_before_len;
    int source_after_len;
    int rc;
    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    /* A TN6P-shaped input must be rejected now that LL-ATON owns the ABI. */
    bad[0] = 'T'; bad[1] = 'N'; bad[2] = '6'; bad[3] = 'P';
    check(tiku_vfs_write(TIKU_NPU_BAD_PATH, (const char *)bad, sizeof bad) ==
              TIKU_VFS_OK, "could not write malformed model");
    check(tiku_npu_model_bind(&npu_vfs_model, TIKU_NPU_BAD_PATH) ==
              TIKU_NPU_ERR_HEADER, "TN6P input was accepted");

    /* The 3.2 MiB combined model is deployed in external NOR before boot. */
    check(tiku_vfs_write(TIKU_NPU_CORRUPT_PATH,
                         (const char *)tiku_npu_stm32n6_identity_corrupt,
                         tiku_npu_stm32n6_identity_corrupt_len) == TIKU_VFS_OK,
          "could not provision corrupted relocation fixture");
    check(tiku_npu_model_bind(&npu_vfs_model, TIKU_NPU_CORRUPT_PATH) ==
              TIKU_NPU_ERR_RELOCATION,
          "corrupted relocation site was accepted");
    rc = tiku_npu_model_bind(&npu_vfs_model, TIKU_NPU_VFS_TEST_MODEL_PATH);
    check(rc == TIKU_NPU_OK, "deployed network_rel.bin bind failed");
    if (rc == TIKU_NPU_OK) {
        source_before_len = tiku_vfs_read(TIKU_NPU_VFS_TEST_MODEL_PATH,
                                          (char *)source_before,
                                          sizeof source_before);
        rc = tiku_npu_model_load(&npu_vfs_model);
        check(rc == TIKU_NPU_OK, "deployed network_rel.bin installation failed");
        if (rc == TIKU_NPU_OK) {
            check(tiku_npu_model_io(&npu_vfs_model, &io) == TIKU_NPU_OK &&
                      io.input_count == 1u && io.output_count == 1u &&
                      io.inputs[0].type == TIKU_NPU_TENSOR_INT8 &&
                      io.inputs[0].shape[0] == 1u &&
                      io.inputs[0].shape[1] == 3u &&
                      io.inputs[0].shape[2] == 256u &&
                      io.inputs[0].shape[3] == 256u &&
                      io.outputs[0].type == TIKU_NPU_TENSOR_INT8 &&
                      io.outputs[0].shape[0] == 1u &&
                      io.outputs[0].shape[1] == 5u &&
                      io.outputs[0].shape[2] == 1344u,
                  "deployed network I/O descriptors mismatch");
            check(tiku_npu_model_unload(&npu_vfs_model) == TIKU_NPU_OK,
                  "deployed network_rel.bin unload failed");
            rc = tiku_npu_model_bind(&npu_vfs_model,
                                     TIKU_NPU_VFS_TEST_MODEL_PATH);
            check(rc == TIKU_NPU_OK, "second deployed network_rel.bin bind failed");
            if (rc == TIKU_NPU_OK) {
                check(tiku_npu_model_load(&npu_vfs_model) == TIKU_NPU_OK,
                      "second deployed network_rel.bin installation failed");
                check(tiku_npu_model_unload(&npu_vfs_model) == TIKU_NPU_OK,
                      "second network_rel.bin unload failed");
            }
            source_after_len = tiku_vfs_read(TIKU_NPU_VFS_TEST_MODEL_PATH,
                                             (char *)source_after,
                                             sizeof source_after);
            check(source_before_len == source_after_len &&
                      source_before_len >= 0 &&
                      memcmp(source_before, source_after,
                             (size_t)source_before_len) == 0,
                  "deployed network_rel.bin source changed across reloads");
        }
    }
    TIKU_PRINTF("NPU VFS acceptance: result=%s\n",
                failures == 0u ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&npu_vfs_test);

#endif
