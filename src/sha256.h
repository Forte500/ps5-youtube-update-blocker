#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* Write 64 lowercase hex characters plus a NUL terminator. */
void sha256_hex(const uint8_t digest[32], char out[65]);

#endif /* SHA256_H */
