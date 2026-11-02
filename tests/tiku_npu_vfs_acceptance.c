/* STM32N6 LL-ATON raw-slot model acceptance process. */
#include <stdint.h>
#include <string.h>

#include "ll_aton_reloc_network.h"

#if (TIKU_HAS_NPU + 0) && TIKU_NPU_VFS_TEST_ENABLE
#include <interfaces/npu/tiku_npu.h>
#include <kernel/memory/tiku_nvm_mirror.h>
#include <kernel/process/tiku_process.h>
#include <drivers/stm32n6/npu/models/stm32n6_network_model.h>
#include <arch/stm32n6/tiku_npu_arch.h>
#include <arch/stm32n6/tiku_n6_model_manifest.h>
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
    tiku_n6_model_image_t image;
    tiku_n6_model_image_t bare_image;
    tiku_n6_model_image_t invalid_image;
    const void *mapped;
    tiku_npu_model_io_t io;
    uint32_t mapped_len;
    int source_before_len;
    int source_after_len;
    int rc;
    (void)ev;
    (void)data;
    TIKU_PROCESS_BEGIN();

    rc = tiku_npu_init();
    check(rc == TIKU_NPU_INIT_OK || rc == TIKU_NPU_INIT_ALREADY,
          "application NPU initialization failed");
    if (rc != TIKU_NPU_INIT_OK && rc != TIKU_NPU_INIT_ALREADY) {
        goto npu_storage_report;
    }

    memset(&invalid_image, 0xA5, sizeof invalid_image);
    check(tiku_n6_model_store_resolve(NULL, &invalid_image) ==
              TIKU_NPU_ERR_NOT_FOUND &&
              invalid_image.file_ptr == 0U &&
              invalid_image.file_params_ptr == 0U &&
              invalid_image.file_bytes == 0U &&
              invalid_image.params_bytes == 0U,
          "null model reference was accepted or descriptor was not cleared");
    check(tiku_n6_model_store_resolve(TIKU_NPU_VFS_TEST_MODEL_PATH, NULL) ==
              TIKU_NPU_ERR_ARGUMENT,
          "null model descriptor was accepted");
    check(tiku_npu_model_bind(&npu_storage_model, TIKU_NPU_MISSING_PATH) ==
              TIKU_NPU_ERR_NOT_FOUND, "missing model reference was accepted");
    check(tiku_n6_model_store_resolve(TIKU_NPU_VFS_TEST_MODEL_PATH, &image) ==
              TIKU_NPU_OK &&
              image.file_ptr == (uintptr_t)0x70880000UL &&
              image.file_params_ptr == 0U && image.params_bytes == 0U &&
              image.file_bytes == TIKU_NPU_MODEL_FILE_BYTES,
          "raw model descriptor is incorrect");
    check(tiku_nvm_crc32((const void *)(uintptr_t)image.file_ptr,
                         image.file_bytes) == TIKU_NPU_MODEL_FILE_CRC32,
          "raw model CRC does not match the generated manifest");
    check(tiku_n6_model_store_resolve("network_rel.bin", &bare_image) ==
              TIKU_NPU_OK && bare_image.file_ptr == image.file_ptr &&
              bare_image.file_bytes == image.file_bytes,
          "bare model reference did not resolve to the raw slot");
    mapped_len = 0U;
    mapped = tiku_n6_model_store_map(&mapped_len);
    check((uintptr_t)mapped == (uintptr_t)0x70880000UL &&
              mapped_len == TIKU_NPU_MODEL_FILE_BYTES,
          "raw model map did not return slot offset zero");
    check(tiku_n6_model_store_resolve("/data/npu/../network_rel.bin",
                                      &invalid_image) ==
              TIKU_NPU_ERR_NOT_FOUND,
          "traversal model reference was accepted");
    check(tiku_n6_model_store_resolve("other.network_rel.bin", &invalid_image) ==
              TIKU_NPU_ERR_NOT_FOUND,
          "alternate model reference was accepted");
    check(*(const uint32_t *)(uintptr_t)image.file_ptr == AI_RELOC_MAGIC,
          "raw model does not begin with AI_RELOC_MAGIC");
    rc = tiku_npu_model_bind(&npu_storage_model, TIKU_NPU_VFS_TEST_MODEL_PATH);
    check(rc == TIKU_NPU_OK, "deployed raw model bind failed");
    if (rc == TIKU_NPU_OK) {
        check(tiku_n6_model_store_begin("network_rel.bin", mapped,
                                        mapped_len) == TIKU_BIGBLOB_ERR_BUSY,
              "model-bound raw-slot update interlock was not enforced");
        source_before_len = (int)(image.file_bytes < sizeof source_before ?
                                  image.file_bytes : sizeof source_before);
        memcpy(source_before, (const void *)(uintptr_t)image.file_ptr,
               (size_t)source_before_len);
        rc = tiku_npu_model_load(&npu_storage_model);
        check(rc == TIKU_NPU_OK, "deployed raw model installation failed");
        if (rc == TIKU_NPU_OK) {
            check(tiku_npu_model_io(&npu_storage_model, &io) == TIKU_NPU_OK &&
                      io.input_count == 1u &&
                      io.output_count == TIKU_NPU_FIXTURE_OUTPUT_COUNT &&
                      io.inputs[0].type == TIKU_NPU_TENSOR_INT8 &&
                      io.inputs[0].shape[0] == 1u &&
                      io.inputs[0].shape[1] == TIKU_NPU_FIXTURE_INPUT_CHANNELS &&
                      io.inputs[0].shape[2] == TIKU_NPU_FIXTURE_INPUT_HEIGHT &&
                      io.inputs[0].shape[3] == TIKU_NPU_FIXTURE_INPUT_WIDTH &&
                      io.outputs[0].type == TIKU_NPU_TENSOR_INT8 &&
                      io.outputs[0].shape[0] == 1u &&
                      io.outputs[0].shape[1] == 6u &&
                      io.outputs[0].shape[2] == 24u &&
                      io.outputs[0].shape[3] == 24u,
                  "deployed network I/O descriptors mismatch");
            check(tiku_npu_model_unload(&npu_storage_model) == TIKU_NPU_OK,
                  "deployed raw model unload failed");
            rc = tiku_npu_model_bind(&npu_storage_model,
                                     "network_rel.bin");
            check(rc == TIKU_NPU_OK, "bare raw model name bind failed");
            if (rc == TIKU_NPU_OK) {
                check(tiku_npu_model_load(&npu_storage_model) == TIKU_NPU_OK,
                      "second raw model installation failed");
                check(tiku_npu_model_unload(&npu_storage_model) == TIKU_NPU_OK,
                      "second raw model unload failed");
            }
            source_after_len = (int)(bare_image.file_bytes < sizeof source_after ?
                                     bare_image.file_bytes : sizeof source_after);
            memcpy(source_after, (const void *)(uintptr_t)bare_image.file_ptr,
                   (size_t)source_after_len);
            check(source_before_len == source_after_len &&
                      source_before_len >= 0 &&
                      memcmp(source_before, source_after,
                             (size_t)source_before_len) == 0,
                  "raw model source changed across reloads");
        }
    }
 npu_storage_report:
    TIKU_PRINTF("NPU raw-slot acceptance: result=%s\n",
                failures == 0u ? "PASS" : "FAIL");
    while (1) TIKU_PROCESS_WAIT_EVENT();

    TIKU_PROCESS_END();
}

TIKU_AUTOSTART_PROCESSES(&npu_storage_test);

#endif
