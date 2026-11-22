/* The board's token: what `top` prints after keetsta.me/mk1/top#, and what the server checks (SPEC.md).
 *
 *   token = base64url without padding of  1 | chipid[8] | entries | mac[16]
 *   entry = task number | length | the task's program, vm_encode'd
 *   mac   = the first 16 bytes of HMAC-SHA256(card key, everything before it)
 *
 * The card key is sha256("mk1/card" | RANDID), RANDID being the 128 random bits the factory leaves in OTP
 * rows 0x004..0x00b, each row's low byte first. chipid is the chip id in the order the USB serial number
 * spells it. The server keeps chipid -> card key for the cards it lets on the board and runs the programs
 * again itself, so a token says what the card solved, never how many points that is worth.
 */
#ifndef MK1_TOKEN_H
#define MK1_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#define TOKEN_VERSION 1
#define TOKEN_MAC 16
#define TOKEN_SOLS_MAX 1500   /* the entries: the card's best programs, kept in its state and sent */
#define TOKEN_BIN_MAX (1 + 8 + TOKEN_SOLS_MAX + TOKEN_MAC)
#define TOKEN_TEXT_MAX ((TOKEN_BIN_MAX + 2) / 3 * 4 + 1)
#define TOKEN_NICK_NONCE 16
#define TOKEN_NICK_MAX 96

void token_card_key(const uint8_t randid[16], uint8_t key[32]);
int token_nickname_proof(const uint8_t chipid[8], const uint8_t key[32], const uint8_t nonce[TOKEN_NICK_NONCE],
                         const uint8_t *nickname, size_t len, uint8_t proof[32]);

/* the entries lie end to end and fill exactly sols_len bytes */
int token_entries_ok(const uint8_t *sols, int sols_len);
/* the program kept for task t, or NULL; *len gets its length */
const uint8_t *token_find(const uint8_t *sols, int sols_len, int task, int *len);
/* keeps code as task t's program in place of the one before, entries in task order; 0 ok, -1 no room */
int token_put(uint8_t sols[TOKEN_SOLS_MAX], uint16_t *sols_len, int task, const uint8_t *code, int len);

/* the token as text into out[TOKEN_TEXT_MAX]; returns its length */
int token_make(const uint8_t chipid[8], const uint8_t key[32], const uint8_t *sols, int sols_len, char *out);

/* the server's side, and the tests' */
typedef struct {
    const uint8_t *bin, *chipid, *sols, *mac;
    int sols_len, signed_len;   /* the mac covers bin[0..signed_len) */
} token_parts;
/* decodes text into bin[TOKEN_BIN_MAX] and finds its parts; 0 ok, -1 not a token */
int token_read(const char *text, uint8_t *bin, token_parts *t);
/* 0 if the mac was made with this card key */
int token_check(const token_parts *t, const uint8_t key[32]);

#endif
