/* STM32N6 LL-ATON bigblob-backed model acceptance process. */
#include <stdint.h>
#include <string.h>

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_VFS_TEST_ENABLE
#include <interfaces/npu/tiku_npu.h>
#include <kernel/process/tiku_process.h>
#include <drivers/stm32n6/npu/models/stm32n6_network_model.h>
#include <arch/stm32n6/tiku_n6_model_store.h>

#ifndef TIKU_NPU_VFS_TEST_MODEL_PATH
#define TIKU_NPU_VFS_TEST_MODEL_PATH TIKU_NPU_FIXTURE_MODEL_PATH
#endif

#define TIKU_NPU_MISSING_PATH "/data/npu/missing.bin"

TIKU_PROCESS(npu_storage_test, "NPU storage test");
TIKU_NPU_MODEL(npu_storage_model);

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        TIKU_PRINTF("NPU storage FAIL: %s\n", message);
    }
}

PT_THREAD(tiku_process_thread_npu_storage_test(
    struct pt *process_pt, tiku_event_t ev, tiku_event_data_t data))
{
    uint8_t source_before[2048];
    uint8_t source_after[2048];
    uint32_t mapped_len;
    const uint8_t *mapped;
    tiku_bigblob_info_t info;
    tiku_npu_model_io_t io;
    int source_before_len;
    int source_after_len;
    int rc;
    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    memset(&info, 0, sizeof info);
    check(tiku_npu_model_bind(&npu_storage_model, TIKU_NPU_MISSING_PATH) ==
              TIKU_NPU_ERR_NOT_FOUND, "missing bigblob was accepted");
    check(tiku_n6_model_store_info(&info) == TIKU_BIGBLOB_OK,
          "published bigblob is unavailable");
    check(tiku_n6_model_store_verify() == TIKU_BIGBLOB_OK,
          "published bigblob CRC failed");
    mapped = (const uint8_t *)tiku_n6_model_store_map(&mapped_len);
    check(mapped != NULL && mapped_len == info.len,
          "published bigblob could not be mapped");
    rc = tiku_npu_model_bind(&npu_storage_model, TIKU_NPU_VFS_TEST_MODEL_PATH);
    check(rc == TIKU_NPU_OK, "deployed bigblob model bind failed");
    if (rc == TIKU_NPU_OK) {
        check(tiku_n6_model_store_begin(info.name, mapped, mapped_len) ==
                  TIKU_BIGBLOB_ERR_BUSY,
              "model-bound provisioning interlock was not enforced");
        source_before_len = (int)(mapped_len < sizeof source_before ?
                                  mapped_len : sizeof source_before);
        memcpy(source_before, mapped, (size_t)source_before_len);
        rc = tiku_npu_model_load(&npu_storage_model);
        check(rc == TIKU_NPU_OK, "deployed bigblob installation failed");
        if (rc == TIKU_NPU_OK) {
            check(tiku_npu_model_io(&npu_storage_model, &io) == TIKU_NPU_OK &&
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
            check(tiku_npu_model_unload(&npu_storage_model) == TIKU_NPU_OK,
                  "deployed bigblob unload failed");
            rc = tiku_npu_model_bind(&npu_storage_model,
                                     "network_rel.bin");
            check(rc == TIKU_NPU_OK, "bare bigblob name bind failed");
            if (rc == TIKU_NPU_OK) {
                check(tiku_npu_model_load(&npu_storage_model) == TIKU_NPU_OK,
                      "second bigblob installation failed");
                check(tiku_npu_model_unload(&npu_storage_model) == TIKU_NPU_OK,
                      "second bigblob unload failed");
            }
            mapped = (const uint8_t *)tiku_n6_model_store_map(&mapped_len);
            source_after_len = (int)(mapped_len < sizeof source_after ?
                                     mapped_len : sizeof source_after);
            if (mapped != NULL) {
                memcpy(source_after, mapped, (size_t)source_after_len);
            }
            check(source_before_len == source_after_len &&
                      source_before_len >= 0 &&
                      memcmp(source_before, source_after,
                             (size_t)source_before_len) == 0,
                  "bigblob model source changed across reloads");
        }
    }
    TIKU_PRINTF("NPU bigblob acceptance: result=%s\n",
                failures == 0u ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&npu_storage_test);

#endif
