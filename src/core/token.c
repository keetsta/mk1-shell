#include "token.h"

#include <string.h>

#include "sha256.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

void token_card_key(const uint8_t randid[16], uint8_t key[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, "mk1/card", 8);
    sha256_update(&c, randid, 16);
    sha256_final(&c, key);
}

int token_entries_ok(const uint8_t *sols, int sols_len)
{
    int k = 0;
    while (k < sols_len) {
        if (k + 2 > sols_len || k + 2 + sols[k + 1] > sols_len)
            return 0;
        k += 2 + sols[k + 1];
    }
    return 1;
}

const uint8_t *token_find(const uint8_t *sols, int sols_len, int task, int *len)
{
    for (int k = 0; k + 2 <= sols_len && k + 2 + sols[k + 1] <= sols_len; k += 2 + sols[k + 1])
        if (sols[k] == task) {
            *len = sols[k + 1];
            return sols + k + 2;
        }
    return NULL;
}

int token_put(uint8_t sols[TOKEN_SOLS_MAX], uint16_t *sols_len, int task, const uint8_t *code, int len)
{
    int n = *sols_len, at = 0, old;
    if (!token_entries_ok(sols, n))
        return -1;
    const uint8_t *was = token_find(sols, n, task, &old);
    if (len > 255 || n - (was ? old + 2 : 0) + len + 2 > TOKEN_SOLS_MAX)
        return -1;
    if (was) {
        int from = (int)(was - sols) - 2;
        memmove(sols + from, sols + from + old + 2, (size_t)(n - from - old - 2));
        n -= old + 2;
    }
    while (at < n && sols[at] < task)
        at += 2 + sols[at + 1];
    memmove(sols + at + len + 2, sols + at, (size_t)(n - at));
    sols[at] = (uint8_t)task;
    sols[at + 1] = (uint8_t)len;
    memcpy(sols + at + 2, code, (size_t)len);
    *sols_len = (uint16_t)(n + len + 2);
    return 0;
}

int token_make(const uint8_t chipid[8], const uint8_t key[32], const uint8_t *sols, int sols_len, char *out)
{
    static uint8_t bin[TOKEN_BIN_MAX];   /* static: the card's stack is 4 KB */
    uint8_t mac[32];
    int n = 0, k = 0;
    bin[n++] = TOKEN_VERSION;
    memcpy(bin + n, chipid, 8);
    n += 8;
    memcpy(bin + n, sols, (size_t)sols_len);
    n += sols_len;
    hmac_sha256(key, 32, bin, (size_t)n, mac);
    memcpy(bin + n, mac, TOKEN_MAC);
    n += TOKEN_MAC;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)bin[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)bin[i + 1] << 8;
        if (i + 2 < n)
            v |= bin[i + 2];
        int chars = n - i >= 3 ? 4 : n - i + 1;
        for (int c = 0; c < chars; c++)
            out[k++] = B64[v >> (18 - 6 * c) & 63];
    }
    out[k] = 0;
    return k;
}

int token_read(const char *text, uint8_t *bin, token_parts *t)
{
    uint32_t v = 0;
    int bits = 0, n = 0;
    for (const char *p = text; *p; p++) {
        const char *at = memchr(B64, *p, 64);
        if (!at || n == TOKEN_BIN_MAX)
            return -1;
        v = v << 6 | (uint32_t)(at - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bin[n++] = (uint8_t)(v >> bits);
        }
    }
    /* a lone character, or leftover bits that are not zero, is not what token_make writes */
    if (bits >= 6 || (v & ((1u << bits) - 1)))
        return -1;
    if (n < 1 + 8 + TOKEN_MAC || bin[0] != TOKEN_VERSION)
        return -1;
    t->bin = bin;
    t->chipid = bin + 1;
    t->sols = bin + 9;
    t->signed_len = n - TOKEN_MAC;
    t->sols_len = t->signed_len - 9;
    t->mac = bin + t->signed_len;
    return token_entries_ok(t->sols, t->sols_len) ? 0 : -1;
}

int token_check(const token_parts *t, const uint8_t key[32])
{
    uint8_t mac[32], diff = 0;
    hmac_sha256(key, 32, t->bin, (size_t)t->signed_len, mac);
    for (int i = 0; i < TOKEN_MAC; i++)
        diff |= (uint8_t)(mac[i] ^ t->mac[i]);
    return diff ? -1 : 0;
}
