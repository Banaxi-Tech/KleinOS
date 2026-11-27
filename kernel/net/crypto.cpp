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

// ================================================================ SHA-512 + Ed25519 verify (after TweetNaCl)
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};
static inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

namespace {
struct Sha512 {
    uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                     0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    uint8_t buf[128];
    size_t blen = 0;
    uint64_t total = 0;
    void block(const uint8_t* p) {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            uint64_t v = 0;
            for (int j = 0; j < 8; j++) v = (v << 8) | p[i * 8 + j];
            w[i] = v;
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t v[8];
        for (int i = 0; i < 8; i++) v[i] = h[i];
        for (int i = 0; i < 80; i++) {
            uint64_t s1 = ror64(v[4], 14) ^ ror64(v[4], 18) ^ ror64(v[4], 41);
            uint64_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K512[i] + w[i];
            uint64_t s0 = ror64(v[0], 28) ^ ror64(v[0], 34) ^ ror64(v[0], 39);
            uint64_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += v[i];
    }
    void update(const uint8_t* d, size_t n) {
        total += n;
        while (n) {
            size_t c = 128 - blen < n ? 128 - blen : n;
            for (size_t i = 0; i < c; i++) buf[blen + i] = d[i];
            blen += c; d += c; n -= c;
            if (blen == 128) { block(buf); blen = 0; }
        }
    }
    void final(uint8_t out[64]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (blen != 120) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
        update(len, 8);
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++) out[i * 8 + j] = (uint8_t)(h[i] >> (56 - j * 8));
    }
};
}  // namespace

static const gf kGf0 = {0}, kGf1 = {1};
static const gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                      0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                       0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf kX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                      0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf kY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                      0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                      0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
static const uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

static void gf_copy(gf o, const gf a) { for (int i = 0; i < 16; i++) o[i] = a[i]; }
static int gf_parity(const gf a) { uint8_t d[32]; pack25519(d, a); return d[0] & 1; }
static bool gf_neq(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a); pack25519(d, b);
    uint8_t r = 0;
    for (int i = 0; i < 32; i++) r |= c[i] ^ d[i];
    return r != 0;
}
static void pow2523(gf o, const gf in) {
    gf c;
    gf_copy(c, in);
    for (int a = 250; a >= 0; a--) { fS(c, c); if (a != 1) fM(c, c, in); }
    gf_copy(o, c);
}
static void ed_add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    fZ(a, p[1], p[0]); fZ(t, q[1], q[0]); fM(a, a, t);
    fA(b, p[0], p[1]); fA(t, q[0], q[1]); fM(b, b, t);
    fM(c, p[3], q[3]); fM(c, c, kD2);
    fM(d, p[2], q[2]); fA(d, d, d);
    fZ(e, b, a); fZ(f, d, c); fA(g, d, c); fA(h, b, a);
    fM(p[0], e, f); fM(p[1], h, g); fM(p[2], g, f); fM(p[3], e, h);
}
static void ed_scalarmult(gf p[4], gf q[4], const uint8_t s[32]) {
    gf_copy(p[0], kGf0); gf_copy(p[1], kGf1); gf_copy(p[2], kGf1); gf_copy(p[3], kGf0);
    for (int i = 255; i >= 0; i--) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        for (int k = 0; k < 4; k++) sel25519(p[k], q[k], b);
        ed_add(q, p);
        ed_add(p, p);
        for (int k = 0; k < 4; k++) sel25519(p[k], q[k], b);
    }
}
static bool ed_unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    gf_copy(r[2], kGf1);
    unpack25519(r[1], p);
    fS(num, r[1]); fM(den, num, kD); fZ(num, num, r[2]); fA(den, r[2], den);
    fS(den2, den); fS(den4, den2); fM(den6, den4, den2);
    fM(t, den6, num); fM(t, t, den);
    pow2523(t, t);
    fM(t, t, num); fM(t, t, den); fM(t, t, den); fM(r[0], t, den);
    fS(chk, r[0]); fM(chk, chk, den);
    if (gf_neq(chk, num)) fM(r[0], r[0], kI);
    fS(chk, r[0]); fM(chk, chk, den);
    if (gf_neq(chk, num)) return false;
    if (gf_parity(r[0]) == (p[31] >> 7)) fZ(r[0], kGf0, r[0]);
    fM(r[3], r[0], r[1]);
    return true;
}
static void mod_l(uint8_t r[32], int64_t x[64]) {
    int64_t carry;
    int j;
    for (int i = 63; i >= 32; i--) {
        carry = 0;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * kL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * kL[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = x[i] & 255;
    }
}

bool ed25519_verify(const uint8_t sig[64], const void* msg, size_t n, const uint8_t pub[32]) {
    // reject non-canonical S
    for (int i = 31; i >= 0; i--) {
        if (sig[32 + i] < kL[i]) break;
        if (sig[32 + i] > kL[i] || i == 0) return false;
    }
    gf p[4], q[4];
    if (!ed_unpackneg(q, pub)) return false;
    Sha512 s;
    s.update(sig, 32);
    s.update(pub, 32);
    s.update((const uint8_t*)msg, n);
    uint8_t hash[64], h[32], check[32];
    s.final(hash);
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = hash[i];
    mod_l(h, x);
    ed_scalarmult(p, q, h);
    gf b[4];
    gf_copy(b[0], kX); gf_copy(b[1], kY); gf_copy(b[2], kGf1); fM(b[3], kX, kY);
    ed_scalarmult(q, b, sig + 32);
    ed_add(p, q);
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    fM(tx, p[0], zi); fM(ty, p[1], zi);
    pack25519(check, ty);
    check[31] ^= gf_parity(tx) << 7;
    uint8_t r = 0;
    for (int i = 0; i < 32; i++) r |= check[i] ^ sig[i];
    return r == 0;
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

// ================================================================ AES-128 blocks, SHA-1, WPA2
void Aes128::init(const uint8_t key[16]) { aes_expand(rk, key); }
void Aes128::encrypt(const uint8_t in[16], uint8_t out[16]) const { aes_encrypt_block(rk, in, out); }

static uint8_t g_inv_sbox[256];
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

void Aes128::decrypt(const uint8_t in[16], uint8_t out[16]) const {
    if (!g_inv_sbox[0x63]) for (int i = 0; i < 256; i++) g_inv_sbox[SBOX[i]] = (uint8_t)i;
    uint8_t s[16], t[16];
    auto add_key = [&](int round) {
        for (int c = 0; c < 4; c++) {
            uint32_t w = rk[round * 4 + c];
            s[c * 4] ^= (uint8_t)(w >> 24);
            s[c * 4 + 1] ^= (uint8_t)(w >> 16);
            s[c * 4 + 2] ^= (uint8_t)(w >> 8);
            s[c * 4 + 3] ^= (uint8_t)w;
        }
    };
    for (int i = 0; i < 16; i++) s[i] = in[i];
    add_key(10);
    for (int round = 9; round >= 0; round--) {
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = g_inv_sbox[s[c * 4 + r]];
        for (int i = 0; i < 16; i++) s[i] = t[i];
        add_key(round);
        if (!round) break;
        for (int c = 0; c < 4; c++) {
            uint8_t* m = s + c * 4;
            uint8_t a = m[0], b = m[1], d = m[2], e = m[3];
            m[0] = gmul(a, 14) ^ gmul(b, 11) ^ gmul(d, 13) ^ gmul(e, 9);
            m[1] = gmul(a, 9) ^ gmul(b, 14) ^ gmul(d, 11) ^ gmul(e, 13);
            m[2] = gmul(a, 13) ^ gmul(b, 9) ^ gmul(d, 14) ^ gmul(e, 11);
            m[3] = gmul(a, 11) ^ gmul(b, 13) ^ gmul(d, 9) ^ gmul(e, 14);
        }
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

namespace {
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64];
    size_t blen = 0;
    uint64_t total = 0;
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f = i < 20 ? ((b & c) | (~b & d)) + 0x5A827999
                       : i < 40 ? (b ^ c ^ d) + 0x6ED9EBA1
                       : i < 60 ? ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDC
                                : (b ^ c ^ d) + 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void update(const uint8_t* d, size_t n) {
        total += n;
        while (n) {
            size_t c = 64 - blen < n ? 64 - blen : n;
            for (size_t i = 0; i < c; i++) buf[blen + i] = d[i];
            blen += c; d += c; n -= c;
            if (blen == 64) { block(buf); blen = 0; }
        }
    }
    void final(uint8_t out[20]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80, zero = 0;
        update(&pad, 1);
        while (blen != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
        update(len, 8);
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - j * 8));
    }
};

struct HmacSha1 {
    Sha1 inner, outer;
    explicit HmacSha1(const uint8_t* key, size_t klen) {
        uint8_t block[64] = {0};
        if (klen > 64) {
            Sha1 s;
            s.update(key, klen);
            s.final(block);
        } else {
            for (size_t i = 0; i < klen; i++) block[i] = key[i];
        }
        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; i++) { ipad[i] = block[i] ^ 0x36; opad[i] = block[i] ^ 0x5C; }
        inner.update(ipad, 64);
        outer.update(opad, 64);
    }
    void final(uint8_t out[20]) {
        uint8_t d[20];
        inner.final(d);
        outer.update(d, 20);
        outer.final(out);
    }
};
}  // namespace

void hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[20]) {
    HmacSha1 h(key, klen);
    h.inner.update(msg, mlen);
    h.final(out);
}

void wpa_psk(const char* pass, const uint8_t* ssid, size_t ssid_len, uint8_t pmk[32]) {
    const uint8_t* key = (const uint8_t*)pass;
    size_t klen = 0;
    while (pass[klen]) klen++;
    HmacSha1 base(key, klen);
    for (int block = 1; block <= 2; block++) {
        uint8_t ctr[4] = {0, 0, 0, (uint8_t)block}, u[20], t[20];
        HmacSha1 h = base;
        h.inner.update(ssid, ssid_len);
        h.inner.update(ctr, 4);
        h.final(u);
        for (int i = 0; i < 20; i++) t[i] = u[i];
        for (int it = 1; it < 4096; it++) {
            HmacSha1 r = base;
            r.inner.update(u, 20);
            r.final(u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        for (int i = 0; i < (block == 1 ? 20 : 12); i++) pmk[(block - 1) * 20 + i] = t[i];
    }
}

void prf_sha1(const uint8_t* key, size_t klen, const char* label, const uint8_t* data, size_t dlen, uint8_t* out, size_t olen) {
    size_t llen = 0, pos = 0;
    while (label[llen]) llen++;
    uint8_t zero = 0;
    for (uint8_t ctr = 0; pos < olen; ctr++) {
        uint8_t d[20];
        HmacSha1 h(key, klen);
        h.inner.update((const uint8_t*)label, llen);
        h.inner.update(&zero, 1);
        h.inner.update(data, dlen);
        h.inner.update(&ctr, 1);
        h.final(d);
        for (int i = 0; i < 20 && pos < olen; i++) out[pos++] = d[i];
    }
}

bool aes_unwrap(const uint8_t kek[16], const uint8_t* in, size_t n, uint8_t* out) {
    if (n < 16 || (n & 7)) return false;
    Aes128 aes;
    aes.init(kek);
    size_t blocks = n / 8 - 1;
    uint8_t a[8], b[16];
    for (int i = 0; i < 8; i++) a[i] = in[i];
    for (size_t i = 0; i < blocks * 8; i++) out[i] = in[8 + i];
    for (int j = 5; j >= 0; j--) {
        for (size_t i = blocks; i >= 1; i--) {
            uint64_t t = blocks * (uint64_t)j + i;
            for (int k = 0; k < 8; k++) b[k] = a[k] ^ (uint8_t)(t >> (56 - k * 8));
            for (int k = 0; k < 8; k++) b[8 + k] = out[(i - 1) * 8 + k];
            aes.decrypt(b, b);
            for (int k = 0; k < 8; k++) { a[k] = b[k]; out[(i - 1) * 8 + k] = b[8 + k]; }
        }
    }
    for (int i = 0; i < 8; i++)
        if (a[i] != 0xA6) return false;
    return true;
}

// CBC-MAC over B0, the AAD and the message.
static void ccm_tag(const Aes128& aes, const uint8_t nonce[13], const uint8_t* aad, size_t alen, const uint8_t* msg, size_t len, uint8_t tag[16]) {
    uint8_t x[16];
    x[0] = 0x59;
    for (int i = 0; i < 13; i++) x[1 + i] = nonce[i];
    x[14] = (uint8_t)(len >> 8);
    x[15] = (uint8_t)len;
    aes.encrypt(x, x);
    uint8_t first[2] = {(uint8_t)(alen >> 8), (uint8_t)alen};
    size_t used = 2;
    for (int i = 0; i < 2; i++) x[i] ^= first[i];
    for (size_t i = 0; i < alen; i++) {
        x[used++] ^= aad[i];
        if (used == 16) { aes.encrypt(x, x); used = 0; }
    }
    if (used) aes.encrypt(x, x);
    used = 0;
    for (size_t i = 0; i < len; i++) {
        x[used++] ^= msg[i];
        if (used == 16) { aes.encrypt(x, x); used = 0; }
    }
    if (used) aes.encrypt(x, x);
    for (int i = 0; i < 16; i++) tag[i] = x[i];
}

static void ccm_ctr(const Aes128& aes, const uint8_t nonce[13], uint16_t ctr, uint8_t out[16]) {
    uint8_t a[16];
    a[0] = 1;
    for (int i = 0; i < 13; i++) a[1 + i] = nonce[i];
    a[14] = (uint8_t)(ctr >> 8);
    a[15] = (uint8_t)ctr;
    aes.encrypt(a, out);
}

static void ccm_crypt(const Aes128& aes, const uint8_t nonce[13], const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t s[16];
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) ccm_ctr(aes, nonce, (uint16_t)(i / 16 + 1), s);
        out[i] = in[i] ^ s[i % 16];
    }
}

void ccmp_encrypt(const Aes128& aes, const uint8_t nonce[13], const uint8_t* aad, size_t alen, const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t tag[16], s0[16];
    ccm_tag(aes, nonce, aad, alen, in, len, tag);
    ccm_crypt(aes, nonce, in, len, out);
    ccm_ctr(aes, nonce, 0, s0);
    for (int i = 0; i < 8; i++) out[len + i] = tag[i] ^ s0[i];
}

bool ccmp_decrypt(const Aes128& aes, const uint8_t nonce[13], const uint8_t* aad, size_t alen, const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t tag[16], s0[16];
    ccm_crypt(aes, nonce, in, len, out);
    ccm_tag(aes, nonce, aad, alen, out, len, tag);
    ccm_ctr(aes, nonce, 0, s0);
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (uint8_t)(in[len + i] ^ s0[i] ^ tag[i]);
    return diff == 0;
}

void random_bytes(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        uint64_t r = cpu_random64();
        for (size_t k = 0; k < 8 && i + k < n; k++) out[i + k] = r >> (k * 8);
    }
}

}  // namespace crypto
