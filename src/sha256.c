/* SHA-256 (FIPS 180-4), self contained. */

#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void compress(sha256_ctx *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++)
        w[i] = be32(block + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    ctx->h[0] = 0x6a09e667;
    ctx->h[1] = 0xbb67ae85;
    ctx->h[2] = 0x3c6ef372;
    ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f;
    ctx->h[5] = 0x9b05688c;
    ctx->h[6] = 0x1f83d9ab;
    ctx->h[7] = 0x5be0cd19;
    ctx->total = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;

    ctx->total += len;
    while (len > 0) {
        size_t n = 64 - ctx->buflen;

        if (n > len)
            n = len;
        memcpy(ctx->buf + ctx->buflen, p, n);
        ctx->buflen += n;
        p += n;
        len -= n;
        if (ctx->buflen == 64) {
            compress(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->total * 8;
    size_t i = ctx->buflen;

    ctx->buf[i++] = 0x80;
    if (i > 56) {
        memset(ctx->buf + i, 0, 64 - i);
        compress(ctx, ctx->buf);
        i = 0;
    }
    memset(ctx->buf + i, 0, 56 - i);
    for (int j = 0; j < 8; j++)
        ctx->buf[56 + j] = (uint8_t)(bits >> (56 - 8 * j));
    compress(ctx, ctx->buf);

    for (int j = 0; j < 8; j++) {
        out[j * 4]     = (uint8_t)(ctx->h[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(ctx->h[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(ctx->h[j] >> 8);
        out[j * 4 + 3] = (uint8_t)ctx->h[j];
    }
    memset(ctx, 0, sizeof(*ctx));
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void sha256_hex(const uint8_t digest[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    out[64] = '\0';
}
