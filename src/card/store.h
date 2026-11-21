/* The shell's state in a ring of flash sectors. Every save goes to the next sector with a higher sequence
 * number and a CRC, so power lost halfway through a save leaves the previous record to load, and the
 * wear spreads over the ring. No hardware here: the platform hands in the flash. */
#ifndef MK1_STORE_H
#define MK1_STORE_H

#include <stdint.h>

#define STORE_SECTOR 4096u
#define STORE_PAGE 256u
#define STORE_MAX (STORE_SECTOR - 16u)

typedef struct {
    uint32_t base, sectors;            /* flash offset of the ring, its length in sectors */
    const uint8_t *(*read)(uint32_t off);
    /* 0 on success; program writes whole pages into an erased sector */
    int (*erase)(uint32_t off);
    int (*program)(uint32_t off, const uint8_t *data, uint32_t len);
    int32_t cur;                       /* sector of the newest record, -1 none; found by the first load */
    uint32_t seq;
    uint8_t scanned;
} store_t;

/* 1 and buf filled if the newest record is n bytes long, else 0 */
int store_load(store_t *s, void *buf, uint32_t n);
/* 0 saved or nothing to do (same bytes as the newest record), -1 the flash failed */
int store_save(store_t *s, const void *buf, uint32_t n);

#endif
