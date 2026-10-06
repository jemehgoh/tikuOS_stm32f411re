/*
 * Tiku Operating System v0.06
 *
 * STM32N6 model storage: one raw combined LL-ATON image in external OSPI NOR.
 * The image occupies the model slot from offset zero; no storage envelope is
 * inserted before the ST relocatable header.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/fs/tiku_bigblob.h>
#include <kernel/memory/tiku_nvm_mirror.h>

#include "tiku_n6_model_store.h"
#include "tiku_n6_model_manifest.h"
#include "tiku_ospi_arch.h"

#include "ll_aton_reloc_network.h"

#define MODEL_SLOT_OFF  0U
#define MODEL_WRITE_STEP 4096U

typedef struct {
    tiku_nvm_backend_t *be;
    const uint8_t *src;
    uint32_t len;
    uint32_t done;
    uint8_t active;
} tiku_n6_model_wr_t;

static tiku_nvm_backend_t model_be;
static tiku_n6_model_wr_t model_wr;
static uint8_t model_wr_active;
static uint8_t model_bound;
static tiku_n6_model_store_state_t model_last = TIKU_N6_MODEL_STORE_IDLE;

static int model_ref_is_canonical(const char *model_ref)
{
    return model_ref != NULL &&
           (strcmp(model_ref, "/data/npu/network_rel.bin") == 0 ||
            strcmp(model_ref, "network_rel.bin") == 0);
}

int tiku_n6_model_store_resolve(const char *model_ref,
                                tiku_n6_model_image_t *out)
{
    uintptr_t file_ptr;
    size_t file_bytes;
    uint32_t magic;

    if (out == NULL) {
        return TIKU_NPU_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (!model_ref_is_canonical(model_ref)) {
        return TIKU_NPU_ERR_NOT_FOUND;
    }
    if (!tiku_ospi_ready() || tiku_ospi_mmap_enable() != TIKU_OSPI_OK) {
        return TIKU_NPU_ERR_IO;
    }
    file_bytes = (size_t)TIKU_NPU_MODEL_FILE_BYTES;
    if (file_bytes == 0U || file_bytes > (size_t)TIKU_OSPI_MODEL_BYTES) {
        return TIKU_NPU_ERR_CAPACITY;
    }

    if ((uintptr_t)TIKU_OSPI_MODEL_ADDR >
        UINTPTR_MAX - (uintptr_t)TIKU_OSPI_MMAP_BASE) {
        return TIKU_NPU_ERR_CAPACITY;
    }
    file_ptr = (uintptr_t)TIKU_OSPI_MMAP_BASE +
               (uintptr_t)TIKU_OSPI_MODEL_ADDR;
    if (file_bytes > (size_t)(UINTPTR_MAX - file_ptr)) {
        return TIKU_NPU_ERR_CAPACITY;
    }

    /* The manifest length is the only bound supplied to LL-ATON.  Verify the
     * same exact raw span here so a damaged or stale slot never reaches the
     * vendor routines, which accept a pointer but no length. */
    if (tiku_nvm_crc32((const void *)file_ptr, file_bytes) !=
        TIKU_NPU_MODEL_FILE_CRC32) {
        return TIKU_NPU_ERR_IO;
    }
    memcpy(&magic, (const void *)file_ptr, sizeof(magic));
    if (magic != AI_RELOC_MAGIC) {
        return TIKU_NPU_ERR_HEADER;
    }

    out->file_ptr = file_ptr;
    out->file_params_ptr = (uintptr_t)0U;
    out->file_bytes = file_bytes;
    out->params_bytes = 0U;
    return TIKU_NPU_OK;
}

static int model_range_ok(size_t off, size_t len)
{
    return off <= model_be.size && len <= model_be.size - off;
}

static int model_physical_range_ok(size_t off, size_t len)
{
    const size_t base = (size_t)TIKU_OSPI_MODEL_ADDR;

    return model_range_ok(off, len) &&
           base <= (size_t)UINT32_MAX &&
           off <= (size_t)UINT32_MAX - base &&
           len <= (size_t)UINT32_MAX - base - off;
}

static int model_write(tiku_nvm_backend_t *be, size_t off,
                       const void *src, size_t len)
{
    if (be == NULL || src == NULL || len == 0U ||
        !model_physical_range_ok(off, len)) {
        return -1;
    }
    if (tiku_ospi_mmap_disable() != TIKU_OSPI_OK ||
        tiku_ospi_program((uint32_t)(TIKU_OSPI_MODEL_ADDR + off), src,
                          (uint32_t)len) != TIKU_OSPI_OK) {
        (void)tiku_ospi_mmap_enable();
        return -1;
    }
    return tiku_ospi_mmap_enable() == TIKU_OSPI_OK ? 0 : -1;
}

static int model_erase(tiku_nvm_backend_t *be, size_t off, size_t len)
{
    size_t first;
    size_t last;

    if (be == NULL || len == 0U || !model_physical_range_ok(off, len) ||
        off > SIZE_MAX - len ||
        off + len > SIZE_MAX - ((size_t)TIKU_OSPI_SECTOR_SIZE - 1U)) {
        return -1;
    }
    first = off & ~((size_t)TIKU_OSPI_SECTOR_SIZE - 1U);
    last = (off + len + TIKU_OSPI_SECTOR_SIZE - 1U) &
           ~((size_t)TIKU_OSPI_SECTOR_SIZE - 1U);
    if (last > be->size || tiku_ospi_mmap_disable() != TIKU_OSPI_OK) {
        (void)tiku_ospi_mmap_enable();
        return -1;
    }
    while (first < last) {
        if (tiku_ospi_erase_sector((uint32_t)(TIKU_OSPI_MODEL_ADDR + first)) !=
            TIKU_OSPI_OK) {
            (void)tiku_ospi_mmap_enable();
            return -1;
        }
        first += TIKU_OSPI_SECTOR_SIZE;
    }
    return tiku_ospi_mmap_enable() == TIKU_OSPI_OK ? 0 : -1;
}

static tiku_nvm_backend_t *model_backend(void)
{
    if (!tiku_ospi_ready() ||
        tiku_ospi_mmap_enable() != TIKU_OSPI_OK) {
        return NULL;
    }
    model_be.base = (uint8_t *)(uintptr_t)(TIKU_OSPI_MMAP_BASE +
                                           TIKU_OSPI_MODEL_ADDR);
    model_be.size = (size_t)TIKU_OSPI_MODEL_BYTES;
    model_be.write = model_write;
    model_be.erase = model_erase;
    model_be.ctx = NULL;
    return &model_be;
}

int tiku_n6_model_store_info(tiku_bigblob_info_t *out)
{
    tiku_nvm_backend_t *be;
    int rc;

    if (out == NULL) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));
    be = model_backend();
    if (be == NULL) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    rc = tiku_n6_model_store_verify();
    if (rc != TIKU_BIGBLOB_OK) {
        return rc;
    }
    out->len = TIKU_NPU_MODEL_FILE_BYTES;
    out->crc = TIKU_NPU_MODEL_FILE_CRC32;
    memcpy(out->name, "network_rel.bin", sizeof("network_rel.bin"));
    return TIKU_BIGBLOB_OK;
}

int tiku_n6_model_store_verify(void)
{
    tiku_nvm_backend_t *be = model_backend();
    uint32_t magic;

    if (be == NULL) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    if (TIKU_NPU_MODEL_FILE_BYTES == 0U ||
        TIKU_NPU_MODEL_FILE_BYTES > be->size) {
        return TIKU_BIGBLOB_ERR_SPACE;
    }
    memcpy(&magic, be->base + MODEL_SLOT_OFF, sizeof(magic));
    if (magic != AI_RELOC_MAGIC ||
        tiku_nvm_crc32(be->base + MODEL_SLOT_OFF,
                       TIKU_NPU_MODEL_FILE_BYTES) !=
            TIKU_NPU_MODEL_FILE_CRC32) {
        return TIKU_BIGBLOB_ERR_CRC;
    }
    return TIKU_BIGBLOB_OK;
}

const void *tiku_n6_model_store_map(uint32_t *length)
{
    tiku_nvm_backend_t *be = model_backend();

    if (length != NULL) {
        *length = 0U;
    }
    if (be == NULL || tiku_n6_model_store_verify() != TIKU_BIGBLOB_OK) {
        return NULL;
    }
    if (length != NULL) {
        *length = TIKU_NPU_MODEL_FILE_BYTES;
    }
    return (const void *)(be->base + MODEL_SLOT_OFF);
}

int tiku_n6_model_store_begin(const char *name, const void *source,
                              uint32_t length)
{
    tiku_nvm_backend_t *be;

    if (model_wr_active || model_bound) {
        return TIKU_BIGBLOB_ERR_BUSY;
    }
    if (!model_ref_is_canonical(name) || source == NULL || length == 0U) {
        model_last = TIKU_N6_MODEL_STORE_ERR_WRITE;
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    if (length != TIKU_NPU_MODEL_FILE_BYTES ||
        length > (uint32_t)TIKU_OSPI_MODEL_BYTES) {
        model_last = TIKU_N6_MODEL_STORE_ERR_WRITE;
        return TIKU_BIGBLOB_ERR_SPACE;
    }
    if (tiku_nvm_crc32(source, length) != TIKU_NPU_MODEL_FILE_CRC32) {
        model_last = TIKU_N6_MODEL_STORE_ERR_VERIFY;
        return TIKU_BIGBLOB_ERR_CRC;
    }
    be = model_backend();
    if (be == NULL) {
        model_last = TIKU_N6_MODEL_STORE_ERR_WRITE;
        return TIKU_BIGBLOB_ERR_IO;
    }

    memset(&model_wr, 0, sizeof(model_wr));
    model_wr.be = be;
    model_wr.src = (const uint8_t *)source;
    model_wr.len = length;
    model_wr.active = 1U;
    model_wr_active = 1U;
    model_last = TIKU_N6_MODEL_STORE_WRITING;
    return TIKU_BIGBLOB_OK;
}

int tiku_n6_model_store_step(uint32_t *completed)
{
    uint32_t n;

    if (!model_wr_active) {
        return 0;
    }
    if (model_wr.done < model_wr.len) {
        n = model_wr.len - model_wr.done;
        if (n > MODEL_WRITE_STEP) {
            n = MODEL_WRITE_STEP;
        }
        if (model_wr.be->erase(model_wr.be, model_wr.done, n) != 0 ||
            model_wr.be->write(model_wr.be, model_wr.done,
                               &model_wr.src[model_wr.done], n) != 0) {
            model_wr.active = 0U;
            model_wr_active = 0U;
            model_last = TIKU_N6_MODEL_STORE_ERR_WRITE;
            return 0;
        }
        model_wr.done += n;
        if (completed != NULL) {
            *completed = model_wr.done;
        }
        return 1;
    }

    model_wr.active = 0U;
    model_wr_active = 0U;
    if (tiku_n6_model_store_verify() != TIKU_BIGBLOB_OK) {
        model_last = TIKU_N6_MODEL_STORE_ERR_VERIFY;
        return 0;
    }
    if (completed != NULL) {
        *completed = model_wr.len;
    }
    model_last = TIKU_N6_MODEL_STORE_DONE;
    return 0;
}

int tiku_n6_model_store_busy(void)
{
    return (int)model_wr_active;
}

void tiku_n6_model_store_set_model_bound(int bound)
{
    model_bound = bound ? 1U : 0U;
}

tiku_n6_model_store_state_t tiku_n6_model_store_state(void)
{
    return model_last;
}
