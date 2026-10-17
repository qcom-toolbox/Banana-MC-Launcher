/* hash - MD5 (RFC 1321) and SHA-1 (FIPS 180-4), one-shot. */
#include <stdint.h>
#include <string.h>
#include "hash.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* the message padded to a multiple of 64 bytes, its bit length at the end
 * (little-endian for MD5, big-endian for SHA-1); calls block() per 64 bytes */
static void blocks(const unsigned char* d, unsigned long len, int big_endian, void (*block)(void*, const unsigned char*), void* st) {
    unsigned long i = 0;
    for (; i + 64 <= len; i += 64) block(st, d + i);
    unsigned char tail[128];
    unsigned long rest = len - i;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, d + i, rest);
    tail[rest] = 0x80;
    int n = rest + 9 > 64 ? 128 : 64;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++)
        tail[big_endian ? n - 1 - k : n - 8 + k] = (unsigned char)(bits >> (8 * k));
    block(st, tail);
    if (n == 128) block(st, tail + 64);
}

/* ── MD5 ─────────────────────────────────────────────────────────── */
static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const int MD5_R[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(void* st, const unsigned char* b) {
    uint32_t* h = (uint32_t*)st;
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)b[i * 4] | (uint32_t)b[i * 4 + 1] << 8 | (uint32_t)b[i * 4 + 2] << 16 | (uint32_t)b[i * 4 + 3] << 24;
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (bb & c) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = bb ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (bb | ~d);        g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = bb;
        uint32_t x = a + f + MD5_K[i] + m[g];
        int r = MD5_R[(i >> 4) * 4 + (i & 3)];
        bb = bb + ROL(x, r);
        a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
}

void md5(const void* data, unsigned long len, unsigned char out[16]) {
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    blocks((const unsigned char*)data, len, 0, md5_block, h);
    for (int i = 0; i < 16; i++) out[i] = (unsigned char)(h[i / 4] >> (8 * (i % 4)));
}

/* ── SHA-1 ───────────────────────────────────────────────────────── */
static void sha1_block(void* st, const unsigned char* b) {
    uint32_t* h = (uint32_t*)st;
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | (uint32_t)b[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (bb & c) | (~bb & d);           k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = bb ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(bb, 30);
        bb = a;
        a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(const void* data, unsigned long len, unsigned char out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    blocks((const unsigned char*)data, len, 1, sha1_block, h);
    for (int i = 0; i < 20; i++) out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

void hex(const unsigned char* b, int n, char* out) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = 0;
}
