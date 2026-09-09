/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tiku_bigblob.c - one very large object per slot, on erase-block media.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <kernel/fs/tiku_bigblob.h>
#include <kernel/memory/tiku_nvm_mirror.h>

/*
 * HEADER LAST, AND THAT IS THE WHOLE DURABILITY STORY.
 *
 * Erasing the header first makes the slot read as empty for the entire
 * minutes-long payload write, so a power cut anywhere in the middle leaves
 * "no blob" rather than "a blob that is partly the old one and partly the
 * new".  The magic word going down last is what publishes it, and the CRC
 * beside it is what makes the publication checkable rather than merely
 * present -- the same gate-last discipline the persist cells use, at a
 * different scale.
 */
/** @brief The header as it sits in the mapped medium, or NULL if unusable. */
static const tiku_bigblob_disk_hdr_t *hdr_at(tiku_nvm_backend_t *be,
                                             uint32_t slot_off)
{
    const tiku_bigblob_disk_hdr_t *h;
    const char *terminator;

    if (be == NULL || be->base == NULL) {
        return NULL;
    }
    if ((uint64_t)slot_off > (uint64_t)be->size ||
        (uint64_t)TIKU_BIGBLOB_HDR_BYTES >
            (uint64_t)be->size - (uint64_t)slot_off) {
        return NULL;
    }
    h = (const tiku_bigblob_disk_hdr_t *)(const void *)(be->base + slot_off);
    if (h->magic != TIKU_BIGBLOB_MAGIC) {
        return NULL;
    }
    /* A length that runs off the end means a header from a different layout,
     * not a blob; refusing here keeps every caller's pointer arithmetic
     * inside the medium. */
    if (h->len == 0U ||
        (uint64_t)h->len > (uint64_t)be->size - (uint64_t)slot_off -
                               (uint64_t)TIKU_BIGBLOB_HDR_BYTES) {
        return NULL;
    }
    if (h->reserved != 0U) {
        return NULL;
    }
    terminator = (const char *)memchr(h->name, '\0', sizeof(h->name));
    if (terminator == NULL) {
        return NULL;
    }
    /* The name is fixed-width on media.  Reject non-erased bytes after the
     * terminator so a torn/corrupt header cannot resolve under a different
     * spelling on another boot. */
    for (const char *p = terminator + 1; p < h->name + sizeof(h->name); p++) {
        if (*p != '\0') {
            return NULL;
        }
    }
    if (terminator == h->name) {
        return NULL;
    }
    return h;
}

static int blob_range_ok(const tiku_nvm_backend_t *be, uint32_t off,
                         uint32_t len)
{
    return be != NULL && (uint64_t)off <= (uint64_t)be->size &&
           (uint64_t)len <= (uint64_t)be->size - (uint64_t)off;
}

static int publish_blob(tiku_bigblob_wr_t *w)
{
    tiku_bigblob_disk_hdr_t h;
    const tiku_bigblob_disk_hdr_t *published;
    uint32_t payload = w->slot_off + TIKU_BIGBLOB_HDR_BYTES;
    uint32_t expected_crc;
    uint32_t medium_crc;

    memset(&h, 0, sizeof(h));
    h.magic = TIKU_BIGBLOB_MAGIC;
    h.len = w->len;
    expected_crc = tiku_nvm_crc32(w->src, w->len);
    medium_crc = tiku_nvm_crc32(w->be->base + payload, w->len);
    if (medium_crc != expected_crc) {
        return TIKU_BIGBLOB_ERR_CRC;
    }
    h.crc = expected_crc;
    memcpy(h.name, w->name, sizeof(h.name));

    /* Program every field except magic first.  The erased magic remains the
     * publication gate while the body is being written and checked. */
    if (w->be->write(w->be, w->slot_off + sizeof(h.magic),
                     (const uint8_t *)&h + sizeof(h.magic),
                     sizeof(h) - sizeof(h.magic)) != 0) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    if (w->be->write(w->be, w->slot_off, &h.magic, sizeof(h.magic)) != 0) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    published = hdr_at(w->be, w->slot_off);
    if (published == NULL || published->len != h.len ||
        published->crc != h.crc ||
        memcmp(published->name, h.name, sizeof(h.name)) != 0 ||
        tiku_bigblob_verify(w->be, w->slot_off) != TIKU_BIGBLOB_OK) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    return TIKU_BIGBLOB_OK;
}

/** @brief Payload ground covered per step; see the header for the sizing. */
#define BIGBLOB_STEP  4096u

int tiku_bigblob_open(tiku_nvm_backend_t *be, uint32_t slot_off,
                      const char *name, const void *src, uint32_t len,
                      tiku_bigblob_wr_t *w)
{
    size_t n;

    if (be == NULL || be->write == NULL || be->erase == NULL || w == NULL ||
        src == NULL || name == NULL || len == 0U) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    n = strlen(name);
    if (n == 0U || n > TIKU_BIGBLOB_NAME_MAX) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    if (slot_off > UINT32_MAX - TIKU_BIGBLOB_HDR_BYTES ||
        !blob_range_ok(be, slot_off, TIKU_BIGBLOB_HDR_BYTES) ||
        !blob_range_ok(be, slot_off + TIKU_BIGBLOB_HDR_BYTES, len)) {
        return TIKU_BIGBLOB_ERR_SPACE;
    }

    memset(w, 0, sizeof(*w));
    w->be       = be;
    w->src      = (const uint8_t *)src;
    w->slot_off = slot_off;
    w->len      = len;
    memcpy(w->name, name, n);

    /* Invalidate the publication gate before erasing.  Programming zero is
     * safe on NOR even when the old header is still present, and closes the
     * small failure window in which an erase refusal could leave the old
     * magic published. */
    {
        const uint32_t unpublished = 0U;
        if (be->write(be, slot_off, &unpublished, sizeof(unpublished)) != 0) {
            return TIKU_BIGBLOB_ERR_IO;
        }
    }
    /* From here the slot reads as empty, so a power cut during the minutes
     * that follow leaves no blob rather than a splice. */
    if (be->erase(be, slot_off, TIKU_BIGBLOB_HDR_BYTES) != 0) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    w->active = 1U;
    return TIKU_BIGBLOB_OK;
}

int tiku_bigblob_step(tiku_bigblob_wr_t *w, uint32_t *done)
{
    uint32_t payload, n;

    if (w == NULL || !w->active) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    payload = w->slot_off + TIKU_BIGBLOB_HDR_BYTES;

    if (w->done < w->len) {
        n = w->len - w->done;
        if (n > BIGBLOB_STEP) {
            n = BIGBLOB_STEP;
        }
        /* Erase and program the same ground in one step, so the medium is
         * never left erased-but-unwritten across a return to the caller. */
        if (w->be->erase(w->be, payload + w->done, n) != 0 ||
            w->be->write(w->be, payload + w->done, &w->src[w->done], n) != 0) {
            w->active = 0U;
            return TIKU_BIGBLOB_ERR_IO;
        }
        w->done += n;
        if (done != NULL) {
            *done = w->done;
        }
        return 1;
    }

    /* Publish only after the body and medium CRC have been checked. */
    {
        int rc = publish_blob(w);
        if (rc != TIKU_BIGBLOB_OK) {
            w->active = 0U;
            return rc;
        }
    }
    w->active = 0U;
    if (done != NULL) {
        *done = w->len;
    }
    return 0;
}

int tiku_bigblob_write(tiku_nvm_backend_t *be, uint32_t slot_off,
                       const char *name, const void *src, uint32_t len)
{
    tiku_bigblob_wr_t w;
    int rc;

    if (be == NULL || be->write == NULL || be->erase == NULL ||
        src == NULL || name == NULL || len == 0U) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    rc = tiku_bigblob_open(be, slot_off, name, src, len, &w);
    if (rc != TIKU_BIGBLOB_OK) {
        return rc;
    }
    do {
        rc = tiku_bigblob_step(&w, NULL);
    } while (rc > 0);
    if (rc < 0) {
        return rc;
    }
    return TIKU_BIGBLOB_OK;
}

int tiku_bigblob_info(tiku_nvm_backend_t *be, uint32_t slot_off,
                      tiku_bigblob_info_t *out)
{
    const tiku_bigblob_disk_hdr_t *h = hdr_at(be, slot_off);

    if (out == NULL) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    if (h == NULL) {
        return TIKU_BIGBLOB_ERR_NOENT;
    }
    out->len = h->len;
    out->crc = h->crc;
    memcpy(out->name, h->name, sizeof(out->name));
    out->name[TIKU_BIGBLOB_NAME_MAX] = '\0';
    return TIKU_BIGBLOB_OK;
}

const void *tiku_bigblob_map(tiku_nvm_backend_t *be, uint32_t slot_off,
                             uint32_t *len)
{
    const tiku_bigblob_disk_hdr_t *h = hdr_at(be, slot_off);

    if (h == NULL) {
        return NULL;
    }
    if (len != NULL) {
        *len = h->len;
    }
    return (const void *)(be->base + slot_off + TIKU_BIGBLOB_HDR_BYTES);
}

int tiku_bigblob_verify(tiku_nvm_backend_t *be, uint32_t slot_off)
{
    const tiku_bigblob_disk_hdr_t *h = hdr_at(be, slot_off);
    uint32_t got;

    if (h == NULL) {
        return TIKU_BIGBLOB_ERR_NOENT;
    }
    got = tiku_nvm_crc32(be->base + slot_off + TIKU_BIGBLOB_HDR_BYTES,
                         h->len);
    return (got == h->crc) ? TIKU_BIGBLOB_OK : TIKU_BIGBLOB_ERR_CRC;
}
