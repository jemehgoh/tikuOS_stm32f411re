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
 * The header is erased first and written last, its magic word after the rest.
 * Until the magic lands the slot reads as empty, so a power cut during the
 * payload write leaves no blob; the CRC beside the magic lets a reader check
 * the payload.
 */
<<<<<<< HEAD
=======
#define BIGBLOB_MAGIC   0x424C4232UL      /* "BLB2" */

/** @brief A slot's header as stored at the start of its first block. */
typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t reserved;
    char     name[TIKU_BIGBLOB_NAME_MAX + 1u];
} bigblob_hdr_t;

_Static_assert(sizeof(bigblob_hdr_t) <= TIKU_BIGBLOB_HDR_BYTES,
               "header must fit inside its own erase block");

>>>>>>> main
/** @brief The header as it sits in the mapped medium, or NULL if unusable. */
static const tiku_bigblob_disk_hdr_t *hdr_at(tiku_nvm_backend_t *be,
                                             uint32_t slot_off)
{
    const tiku_bigblob_disk_hdr_t *h;
    const char *terminator;

    if (be == NULL || be->base == NULL) {
        return NULL;
    }
<<<<<<< HEAD
    if ((uint64_t)slot_off > (uint64_t)be->size ||
        (uint64_t)TIKU_BIGBLOB_HDR_BYTES >
            (uint64_t)be->size - (uint64_t)slot_off) {
=======
    if (slot_off % TIKU_BIGBLOB_HDR_BYTES != 0 || be->size < slot_off ||
        (be->size - slot_off) < TIKU_BIGBLOB_HDR_BYTES) {
>>>>>>> main
        return NULL;
    }
    h = (const tiku_bigblob_disk_hdr_t *)(const void *)(be->base + slot_off);
    if (h->magic != TIKU_BIGBLOB_MAGIC) {
        return NULL;
    }
<<<<<<< HEAD
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
=======
    /* A zero length, a length that runs off the end of the medium or a name
     * without a terminator returns NULL, which keeps every caller's pointer
     * arithmetic inside the medium. */
    if (h->len == 0 ||
        h->len > (be->size - slot_off - TIKU_BIGBLOB_HDR_BYTES) ||
        memchr(h->name, '\0', sizeof(h->name)) == NULL) {
>>>>>>> main
        return NULL;
    }
    return h;
}

<<<<<<< HEAD
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
=======
/** @brief Payload bytes erased and programmed per step: one erase sector. */
>>>>>>> main
#define BIGBLOB_STEP  4096u

/** @brief Check a write's arguments and range without 32-bit overflow;
 *  TIKU_BIGBLOB_OK, TIKU_BIGBLOB_ERR_PARAM or TIKU_BIGBLOB_ERR_SPACE. */
static int writable(tiku_nvm_backend_t *be, uint32_t off,
                    const char *name, const void *src, uint32_t len)
{
    if (be == NULL || be->base == NULL || be->write == NULL ||
        be->erase == NULL || src == NULL || name == NULL || len == 0 ||
        off % TIKU_BIGBLOB_HDR_BYTES != 0) {
        return TIKU_BIGBLOB_ERR_PARAM;
    }
    if (strlen(name) > TIKU_BIGBLOB_NAME_MAX) return TIKU_BIGBLOB_ERR_PARAM;
    if (off > UINT32_MAX - TIKU_BIGBLOB_HDR_BYTES ||
        len > UINT32_MAX - off - TIKU_BIGBLOB_HDR_BYTES ||
        off > be->size || TIKU_BIGBLOB_HDR_BYTES > be->size - off ||
        len > be->size - off - TIKU_BIGBLOB_HDR_BYTES) {
        return TIKU_BIGBLOB_ERR_SPACE;
    }
    return TIKU_BIGBLOB_OK;
}

/** @brief Verify the medium against the source's CRC, then publish the
 *  metadata followed by its magic. */
static int publish(tiku_nvm_backend_t *be, uint32_t off, const char *name,
                   uint32_t src_crc, uint32_t len)
{
    bigblob_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = BIGBLOB_MAGIC;
    h.len = len;
    h.crc = src_crc;
    if (h.crc != tiku_nvm_crc32(be->base + off + TIKU_BIGBLOB_HDR_BYTES, len)) {
        return TIKU_BIGBLOB_ERR_CRC;
    }
    memcpy(h.name, name, strlen(name));
    if (be->write(be, off + sizeof(h.magic),
                  (const uint8_t *)&h + sizeof(h.magic),
                  sizeof(h) - sizeof(h.magic)) != 0 ||
        memcmp(be->base + off + sizeof(h.magic),
               (const uint8_t *)&h + sizeof(h.magic),
               sizeof(h) - sizeof(h.magic)) != 0 ||
        be->write(be, off, &h.magic, sizeof(h.magic)) != 0 ||
        memcmp(be->base + off, &h, sizeof(h)) != 0) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    return TIKU_BIGBLOB_OK;
}

int tiku_bigblob_open(tiku_nvm_backend_t *be, uint32_t slot_off,
                      const char *name, const void *src, uint32_t len,
                      tiku_bigblob_wr_t *w)
{
<<<<<<< HEAD
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

=======
    int rc;
    if (w == NULL) return TIKU_BIGBLOB_ERR_PARAM;
>>>>>>> main
    memset(w, 0, sizeof(*w));
    rc = writable(be, slot_off, name, src, len);
    if (rc != TIKU_BIGBLOB_OK) return rc;
    w->be       = be;
    w->src      = (const uint8_t *)src;
    w->slot_off = slot_off;
    w->len      = len;
    w->crc      = 0xFFFFFFFFU;
    memcpy(w->name, name, strlen(name));

<<<<<<< HEAD
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
=======
    /* Unpublish first: from here the slot reads as empty, and a power cut
     * before the header is rewritten leaves no blob. */
>>>>>>> main
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
        /* Erase and program the same range in one step, so the medium is
         * never left erased-but-unwritten across a return to the caller. */
        if (w->be->erase(w->be, payload + w->done, n) != 0 ||
            w->be->write(w->be, payload + w->done, &w->src[w->done], n) != 0) {
            w->active = 0U;
            return TIKU_BIGBLOB_ERR_IO;
        }
        /* The source's CRC is accumulated per chunk, so the publish step
         * reads only the medium.  One pass over a model-sized payload is the
         * most a step can take within the hang detector's limit. */
        w->crc = tiku_nvm_crc32_update(w->crc, &w->src[w->done], n);
        w->done += n;
        if (done != NULL) {
            *done = w->done;
        }
        return 1;
    }

<<<<<<< HEAD
    /* Publish only after the body and medium CRC have been checked. */
    {
        int rc = publish_blob(w);
        if (rc != TIKU_BIGBLOB_OK) {
            w->active = 0U;
            return rc;
        }
=======
    /* publish() compares the medium with the source's CRC: a misprogrammed
     * payload returns TIKU_BIGBLOB_ERR_CRC and stays unpublished. */
    {
        int rc = publish(w->be, w->slot_off, w->name, w->crc ^ 0xFFFFFFFFU,
                         w->len);
        w->active = 0U;
        if (rc != TIKU_BIGBLOB_OK) return rc;
>>>>>>> main
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
<<<<<<< HEAD
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
=======
    uint32_t payload;
    int rc = writable(be, slot_off, name, src, len);
    if (rc != TIKU_BIGBLOB_OK) return rc;
    payload = slot_off + TIKU_BIGBLOB_HDR_BYTES;

    /* 1. Unpublish.  From here until publish() the slot reads as empty. */
    if (be->erase(be, slot_off, TIKU_BIGBLOB_HDR_BYTES) != 0) {
        return TIKU_BIGBLOB_ERR_IO;
    }

    /* 2. Erase and write the payload. */
    if (be->erase(be, payload, len) != 0) {
        return TIKU_BIGBLOB_ERR_IO;
    }
    if (be->write(be, payload, src, len) != 0) {
        return TIKU_BIGBLOB_ERR_IO;
    }

    return publish(be, slot_off, name, tiku_nvm_crc32(src, len), len);
>>>>>>> main
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
