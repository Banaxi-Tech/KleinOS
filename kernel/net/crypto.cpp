#include "net/crypto.h"
#include "lib/string.h"
#include "arch/cpu.h"

namespace crypto {

// ================================================================ SHA-256
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void Sha256::init() {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, iv, sizeof(h));
    len = 0;
    blen = 0;
}

void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    len += n;
    while (n) {
        size_t c = 64 - blen < n ? 64 - blen : n;
        memcpy(buf + blen, p, c);
        blen += c; p += c; n -= c;
        if (blen == 64) { sha256_block(h, buf); blen = 0; }
    }
}

void Sha256::final(uint8_t out[32]) {
    Sha256 c = *this;
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80;
    c.update(&pad, 1);
    uint8_t zero = 0;
    while (c.blen != 56) c.update(&zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = bits >> (56 - i * 8);
    c.update(lb, 8);
    for (int i = 0; i < 8; i++) { out[i * 4] = c.h[i] >> 24; out[i * 4 + 1] = c.h[i] >> 16; out[i * 4 + 2] = c.h[i] >> 8; out[i * 4 + 3] = c.h[i]; }
}

void sha256(const void* data, size_t n, uint8_t out[32]) {
    Sha256 s;
    s.init();
    s.update(data, n);
    s.final(out);
}

void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (klen > 64) sha256(key, klen, k);
    else memcpy(k, key, klen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[32];
    Sha256 s;
    s.init(); s.update(ipad, 64); s.update(msg, mlen); s.final(inner);
    s.init(); s.update(opad, 64); s.update(inner, 32); s.final(out);
}

void hkdf_extract(const uint8_t* salt, size_t slen, const uint8_t* ikm, size_t ilen, uint8_t out[32]) {
    uint8_t zero[32] = {0};
    if (!salt || !slen) { salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, out);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t ilen, uint8_t* out, size_t olen) {
    uint8_t t[32];
    size_t tlen = 0, done = 0;
    uint8_t buf[32 + 300 + 1];
    for (uint8_t ctr = 1; done < olen; ctr++) {
        size_t n = 0;
        memcpy(buf, t, tlen); n += tlen;
        memcpy(buf + n, info, ilen); n += ilen;
        buf[n++] = ctr;
        hmac_sha256(prk, 32, buf, n, t);
        tlen = 32;
        size_t c = olen - done < 32 ? olen - done : 32;
        memcpy(out + done, t, c);
        done += c;
    }
}

void hkdf_expand_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, size_t clen, uint8_t* out, size_t olen) {
    uint8_t info[300];
    size_t ll = strlen(label);
    size_t n = 0;
    info[n++] = olen >> 8;
    info[n++] = olen;
    info[n++] = 6 + ll;
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = clen;
    if (clen) { memcpy(info + n, ctx, clen); n += clen; }
    hkdf_expand(secret, info, n, out, olen);
}

// ================================================================ X25519 (after TweetNaCl, public domain)
typedef int64_t gf[16];
static const gf k121665 = {0xDB41, 1};

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
static void sel25519(gf p, gf q, int b) {
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = t[i] & 0xff; o[2 * i + 1] = t[i] >> 8; }
}
static void unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fA(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fZ(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fM(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fS(gf o, const gf a) { fM(o, a, a); }
static void inv25519(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { fS(c, c); if (a != 2 && a != 4) fM(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]) {
    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fA(e, a, c); fZ(a, a, c); fA(c, b, d); fZ(b, b, d); fS(d, e); fS(f, a); fM(a, c, a); fM(c, b, e);
        fA(e, a, c); fZ(a, a, c); fS(b, a); fZ(c, d, f); fM(a, c, k121665); fA(a, a, d); fM(c, c, a); fM(a, d, f);
        fM(d, b, x); fS(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519(x + 32, x + 32);
    fM(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}

void x25519_public(uint8_t pub[32], const uint8_t priv[32]) {
    uint8_t base[32] = {9};
    x25519(pub, priv, base);
}

// ================================================================ AES-128
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59,
    0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1,
    0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83,
    0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
    0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee,
    0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6,
    0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1,
    0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static inline uint8_t xtime(uint8_t x) { return (x << 1) ^ ((x >> 7) * 0x1b); }

static void aes_expand(uint32_t rk[44], const uint8_t key[16]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    for (int i = 0; i < 4; i++) rk[i] = ((uint32_t)key[4 * i] << 24) | (key[4 * i + 1] << 16) | (key[4 * i + 2] << 8) | key[4 * i + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);
            t = ((uint32_t)SBOX[t >> 24] << 24) | (SBOX[(t >> 16) & 0xff] << 16) | (SBOX[(t >> 8) & 0xff] << 8) | SBOX[t & 0xff];
            t ^= (uint32_t)rcon[i / 4 - 1] << 24;
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

static void aes_encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ (rk[i / 4] >> (24 - 8 * (i % 4)));
    for (int round = 1; round <= 10; round++) {
        uint8_t t[16];
        // SubBytes + ShiftRows
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[c * 4 + r] = SBOX[s[((c + r) % 4) * 4 + r]];
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t* col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ xtime(a0 ^ a1);
                col[1] ^= all ^ xtime(a1 ^ a2);
                col[2] ^= all ^ xtime(a2 ^ a3);
                col[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ (rk[round * 4 + i / 4] >> (24 - 8 * (i % 4)));
    }
    memcpy(out, s, 16);
}

// GHASH multiply in GF(2^128): x = x * H
static void gf_mul(uint8_t x[16], const uint8_t h[16]) {
    uint64_t zh = 0, zl = 0;
    uint64_t vh = 0, vl = 0;
    for (int i = 0; i < 8; i++) { vh = (vh << 8) | h[i]; vl = (vl << 8) | h[8 + i]; }
    uint64_t xh = 0, xl = 0;
    for (int i = 0; i < 8; i++) { xh = (xh << 8) | x[i]; xl = (xl << 8) | x[8 + i]; }
    for (int i = 0; i < 128; i++) {
        uint64_t bit = i < 64 ? (xh >> (63 - i)) & 1 : (xl >> (127 - i)) & 1;
        uint64_t mask = 0 - bit;
        zh ^= vh & mask;
        zl ^= vl & mask;
        uint64_t lsb = vl & 1;
        vl = (vl >> 1) | (vh << 63);
        vh >>= 1;
        vh ^= 0xE100000000000000ull & (0 - lsb);
    }
    for (int i = 0; i < 8; i++) { x[i] = zh >> (56 - 8 * i); x[8 + i] = zl >> (56 - 8 * i); }
}

static void ghash(const uint8_t H[16], const uint8_t* aad, size_t alen, const uint8_t* c, size_t clen, uint8_t out[16]) {
    uint8_t x[16] = {0};
    for (size_t i = 0; i < alen; i += 16) {
        size_t n = alen - i < 16 ? alen - i : 16;
        for (size_t k = 0; k < n; k++) x[k] ^= aad[i + k];
        gf_mul(x, H);
    }
    for (size_t i = 0; i < clen; i += 16) {
        size_t n = clen - i < 16 ? clen - i : 16;
        for (size_t k = 0; k < n; k++) x[k] ^= c[i + k];
        gf_mul(x, H);
    }
    uint8_t lens[16];
    uint64_t ab = (uint64_t)alen * 8, cb = (uint64_t)clen * 8;
    for (int i = 0; i < 8; i++) { lens[i] = ab >> (56 - 8 * i); lens[8 + i] = cb >> (56 - 8 * i); }
    for (int k = 0; k < 16; k++) x[k] ^= lens[k];
    gf_mul(x, H);
    memcpy(out, x, 16);
}

void AesGcm::init(const uint8_t key[16]) {
    aes_expand(rk, key);
    uint8_t zero[16] = {0};
    aes_encrypt_block(rk, zero, H);
}

static void gcm_ctr(const uint32_t rk[44], const uint8_t nonce[12], const uint8_t* in, size_t n, uint8_t* out) {
    uint8_t ctr[16], ks[16];
    memcpy(ctr, nonce, 12);
    uint32_t cnt = 2;
    for (size_t i = 0; i < n; i += 16) {
        ctr[12] = cnt >> 24; ctr[13] = cnt >> 16; ctr[14] = cnt >> 8; ctr[15] = cnt;
        aes_encrypt_block(rk, ctr, ks);
        size_t m = n - i < 16 ? n - i : 16;
        for (size_t k = 0; k < m; k++) out[i + k] = in[i + k] ^ ks[k];
        cnt++;
    }
}

static void gcm_tag(const uint32_t rk[44], const uint8_t H[16], const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                    const uint8_t* c, size_t clen, uint8_t tag[16]) {
    uint8_t s[16], j0[16], ek[16];
    ghash(H, aad, alen, c, clen, s);
    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes_encrypt_block(rk, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ ek[i];
}

void AesGcm::encrypt(const uint8_t nonce[12], const uint8_t* aad, size_t alen, const uint8_t* in, size_t n, uint8_t* out, uint8_t tag[16]) {
    gcm_ctr(rk, nonce, in, n, out);
    gcm_tag(rk, H, nonce, aad, alen, out, n, tag);
}

bool AesGcm::decrypt(const uint8_t nonce[12], const uint8_t* aad, size_t alen, const uint8_t* in, size_t n, uint8_t* out, const uint8_t tag[16]) {
    uint8_t t[16];
    gcm_tag(rk, H, nonce, aad, alen, in, n, t);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= t[i] ^ tag[i];
    if (diff) return false;
    gcm_ctr(rk, nonce, in, n, out);
    return true;
}

void random_bytes(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        uint64_t r = cpu_random64();
        for (size_t k = 0; k < 8 && i + k < n; k++) out[i + k] = r >> (k * 8);
    }
}

}  // namespace crypto
