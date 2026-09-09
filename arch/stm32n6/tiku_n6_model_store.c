/*
 * Tiku Operating System v0.06
 *
 * STM32N6 model storage: one verified tiku_bigblob in external XSPI NOR.
 * Reads use the memory-mapped window; provisioning uses indirect commands.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/fs/tiku_bigblob.h>

#include "tiku_n6_model_store.h"
#include "tiku_xspi_arch.h"

#define MODEL_SLOT_OFF  0U

static tiku_nvm_backend_t model_be;
static tiku_bigblob_wr_t model_wr;
static uint8_t model_wr_active;
static uint8_t model_bound;
static tiku_n6_model_store_state_t model_last = TIKU_N6_MODEL_STORE_IDLE;

static int model_range_ok(size_t off, size_t len)
{
    return off <= model_be.size && len <= model_be.size - off;
}

static int model_physical_range_ok(size_t off, size_t len)
{
    const size_t base = (size_t)TIKU_XSPI_MODEL_ADDR;

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
    if (tiku_xspi_mmap_disable() != TIKU_XSPI_OK ||
        tiku_xspi_program((uint32_t)(TIKU_XSPI_MODEL_ADDR + off), src,
                          (uint32_t)len) != TIKU_XSPI_OK) {
        (void)tiku_xspi_mmap_enable();
        return -1;
    }
    return tiku_xspi_mmap_enable() == TIKU_XSPI_OK ? 0 : -1;
}

static int model_erase(tiku_nvm_backend_t *be, size_t off, size_t len)
{
    size_t first;
    size_t last;

    if (be == NULL || len == 0U || !model_physical_range_ok(off, len) ||
        off > SIZE_MAX - len ||
        off + len > SIZE_MAX - ((size_t)TIKU_XSPI_SECTOR_SIZE - 1U)) {
        return -1;
    }
    first = off & ~((size_t)TIKU_XSPI_SECTOR_SIZE - 1U);
    last = (off + len + TIKU_XSPI_SECTOR_SIZE - 1U) &
           ~((size_t)TIKU_XSPI_SECTOR_SIZE - 1U);
    if (last > be->size || tiku_xspi_mmap_disable() != TIKU_XSPI_OK) {
        (void)tiku_xspi_mmap_enable();
        return -1;
    }
    while (first < last) {
        if (tiku_xspi_erase_sector((uint32_t)(TIKU_XSPI_MODEL_ADDR + first)) !=
            TIKU_XSPI_OK) {
            (void)tiku_xspi_mmap_enable();
            return -1;
        }
        first += TIKU_XSPI_SECTOR_SIZE;
    }
    return tiku_xspi_mmap_enable() == TIKU_XSPI_OK ? 0 : -1;
}

static tiku_nvm_backend_t *model_backend(void)
{
    if (!tiku_xspi_ready() ||
        tiku_xspi_mmap_enable() != TIKU_XSPI_OK) {
        return NULL;
    }
    model_be.base = (uint8_t *)(uintptr_t)(TIKU_XSPI_MMAP_BASE +
                                           TIKU_XSPI_MODEL_ADDR);
    model_be.size = (size_t)TIKU_XSPI_MODEL_BYTES;
    model_be.write = model_write;
    model_be.erase = model_erase;
    model_be.ctx = NULL;
    return &model_be;
}

int tiku_n6_model_store_info(tiku_bigblob_info_t *out)
{
    tiku_nvm_backend_t *be;

    if (out == NULL) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    be = model_backend();
    if (be == NULL) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    return tiku_bigblob_info(be, MODEL_SLOT_OFF, out);
}

int tiku_n6_model_store_verify(void)
{
    tiku_nvm_backend_t *be = model_backend();

    return be == NULL ? TIKU_BIGBLOB_ERR_IO :
        tiku_bigblob_verify(be, MODEL_SLOT_OFF);
}

const void *tiku_n6_model_store_map(uint32_t *length)
{
    tiku_nvm_backend_t *be = model_backend();

    if (be == NULL || tiku_bigblob_verify(be, MODEL_SLOT_OFF) !=
        TIKU_BIGBLOB_OK) {
        if (length != NULL) {
            *length = 0U;
        }
        return NULL;
    }
    return tiku_bigblob_map(be, MODEL_SLOT_OFF, length);
}

int tiku_n6_model_store_begin(const char *name, const void *source,
                              uint32_t length)
{
    tiku_nvm_backend_t *be;
    int rc;

    if (model_wr_active || model_bound) {
        return TIKU_BIGBLOB_ERR_BUSY;
    }
    if (name == NULL || name[0] == '\0' ||
        strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        model_last = TIKU_N6_MODEL_STORE_ERR_WRITE;
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    be = model_backend();
    if (be == NULL) {
        model_last = TIKU_N6_MODEL_STORE_ERR_WRITE;
        return TIKU_BIGBLOB_ERR_IO;
    }
    rc = tiku_bigblob_open(be, MODEL_SLOT_OFF, name, source, length,
                           &model_wr);
    if (rc != TIKU_BIGBLOB_OK) {
        model_last = TIKU_N6_MODEL_STORE_ERR_WRITE;
        return rc;
    }
    model_wr_active = 1U;
    model_last = TIKU_N6_MODEL_STORE_WRITING;
    return TIKU_BIGBLOB_OK;
}

int tiku_n6_model_store_step(uint32_t *completed)
{
    int rc;

    if (!model_wr_active) {
        return 0;
    }
    rc = tiku_bigblob_step(&model_wr, completed);
    if (rc > 0) {
        return 1;
    }
    model_wr_active = 0U;
    if (rc < 0) {
        model_last = (rc == TIKU_BIGBLOB_ERR_CRC) ?
                     TIKU_N6_MODEL_STORE_ERR_VERIFY :
                     TIKU_N6_MODEL_STORE_ERR_WRITE;
        return 0;
    }
    model_last = (tiku_n6_model_store_verify() == TIKU_BIGBLOB_OK) ?
                 TIKU_N6_MODEL_STORE_DONE : TIKU_N6_MODEL_STORE_ERR_VERIFY;
    if (model_last != TIKU_N6_MODEL_STORE_DONE) {
        return 0;
    }
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
