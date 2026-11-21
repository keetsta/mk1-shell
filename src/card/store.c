#include "store.h"

#include <string.h>

#define MAGIC 0x31534b4du /* "MKS1" */

typedef struct {
    uint32_t magic, seq, len, crc;
} head;

static uint8_t page[STORE_SECTOR];

static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n) {
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static uint32_t rec_crc(uint32_t seq, uint32_t len, const uint8_t *data) {
    uint32_t h[2] = {seq, len};
    return crc32(crc32(0, (const uint8_t *)h, sizeof h), data, len);
}

/* the record in sector i, or NULL if it is erased, torn or foreign */
static const head *record(const store_t *s, uint32_t i) {
    const uint8_t *p = s->read(s->base + i * STORE_SECTOR);
    head h;
    memcpy(&h, p, sizeof h);
    if (h.magic != MAGIC || h.len > STORE_MAX || rec_crc(h.seq, h.len, p + sizeof h) != h.crc) return NULL;
    return (const head *)p;
}

static void scan(store_t *s) {
    s->cur = -1;
    s->seq = 0;
    for (uint32_t i = 0; i < s->sectors; i++) {
        const head *h = record(s, i);
        if (h && (s->cur < 0 || (int32_t)(h->seq - s->seq) > 0)) {
            s->cur = (int32_t)i;
            s->seq = h->seq;
        }
    }
    s->scanned = 1;
}

int store_load(store_t *s, void *buf, uint32_t n) {
    scan(s);
    if (s->cur < 0) return 0;
    const head *h = record(s, (uint32_t)s->cur);
    if (!h || h->len != n) return 0;
    memcpy(buf, (const uint8_t *)h + sizeof *h, n);
    return 1;
}

int store_save(store_t *s, const void *buf, uint32_t n) {
    if (n > STORE_MAX) return -1;
    if (!s->scanned) scan(s);
    if (s->cur >= 0) {
        const head *h = record(s, (uint32_t)s->cur);
        if (h && h->len == n && !memcmp((const uint8_t *)h + sizeof *h, buf, n)) return 0;
    }
    uint32_t next = s->cur < 0 ? 0 : ((uint32_t)s->cur + 1) % s->sectors;
    head h = {MAGIC, s->seq + 1, n, rec_crc(s->seq + 1, n, buf)};
    uint32_t len = (uint32_t)sizeof h + n;
    len = (len + STORE_PAGE - 1) / STORE_PAGE * STORE_PAGE;
    memset(page, 0xFF, len);
    memcpy(page, &h, sizeof h);
    memcpy(page + sizeof h, buf, n);
    uint32_t off = s->base + next * STORE_SECTOR;
    /* the first page, with the magic, goes last: until it lands, the sector reads as erased */
    if (s->erase(off) || (len > STORE_PAGE && s->program(off + STORE_PAGE, page + STORE_PAGE, len - STORE_PAGE)) ||
        s->program(off, page, STORE_PAGE))
        return -1;
    if (memcmp(s->read(off), page, len)) return -1;
    s->cur = (int32_t)next;
    s->seq = h.seq;
    return 0;
}
