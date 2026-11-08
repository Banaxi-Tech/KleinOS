#pragma once
// Crypto primitives for TLS 1.3: SHA-256, HMAC, HKDF, X25519, AES-128-GCM.
#include <stdint.h>
#include <stddef.h>

namespace crypto {

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t blen;
    void init();
    void update(const void* data, size_t n);
    void final(uint8_t out[32]);          // does not modify *this (works on a copy)
};
void sha256(const void* data, size_t n, uint8_t out[32]);
void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]);
void hkdf_extract(const uint8_t* salt, size_t slen, const uint8_t* ikm, size_t ilen, uint8_t out[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t ilen, uint8_t* out, size_t olen);
// TLS 1.3 HKDF-Expand-Label with "tls13 " prefix.
void hkdf_expand_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, size_t clen, uint8_t* out, size_t olen);

// X25519 (RFC 7748)
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_public(uint8_t pub[32], const uint8_t priv[32]);

// AES-128-GCM (RFC 5116), 12-byte nonce, 16-byte tag appended/verified.
struct AesGcm {
    uint32_t rk[44];
    uint8_t H[16];
    void init(const uint8_t key[16]);
    void encrypt(const uint8_t nonce[12], const uint8_t* aad, size_t alen, const uint8_t* in, size_t n, uint8_t* out, uint8_t tag[16]);
    bool decrypt(const uint8_t nonce[12], const uint8_t* aad, size_t alen, const uint8_t* in, size_t n, uint8_t* out, const uint8_t tag[16]);
};

void random_bytes(uint8_t* out, size_t n);

}  // namespace crypto
