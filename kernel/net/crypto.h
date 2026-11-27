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
// Ed25519 signature check (RFC 8032).
bool ed25519_verify(const uint8_t sig[64], const void* msg, size_t n, const uint8_t pub[32]);

// AES-128-GCM (RFC 5116), 12-byte nonce, 16-byte tag appended/verified.
struct AesGcm {
    uint32_t rk[44];
    uint8_t H[16];
    void init(const uint8_t key[16]);
    void encrypt(const uint8_t nonce[12], const uint8_t* aad, size_t alen, const uint8_t* in, size_t n, uint8_t* out, uint8_t tag[16]);
    bool decrypt(const uint8_t nonce[12], const uint8_t* aad, size_t alen, const uint8_t* in, size_t n, uint8_t* out, const uint8_t tag[16]);
};

// AES-128 block operations.
struct Aes128 {
    uint32_t rk[44];
    void init(const uint8_t key[16]);
    void encrypt(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt(const uint8_t in[16], uint8_t out[16]) const;
};
// WPA2-PSK: passphrase to PMK, PRF-SHA1 key derivation, EAPOL MIC, GTK unwrap.
void wpa_psk(const char* passphrase, const uint8_t* ssid, size_t ssid_len, uint8_t pmk[32]);
void prf_sha1(const uint8_t* key, size_t klen, const char* label, const uint8_t* data, size_t dlen, uint8_t* out, size_t olen);
void hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[20]);
// RFC 3394. n = wrapped length (multiple of 8); out receives n - 8 bytes. False if the check fails.
bool aes_unwrap(const uint8_t kek[16], const uint8_t* in, size_t n, uint8_t* out);
// CCMP (AES-CCM, 8-byte MIC, 13-byte nonce). encrypt writes len + 8 bytes; decrypt returns false on a bad MIC.
void ccmp_encrypt(const Aes128& aes, const uint8_t nonce[13], const uint8_t* aad, size_t alen, const uint8_t* in, size_t len, uint8_t* out);
bool ccmp_decrypt(const Aes128& aes, const uint8_t nonce[13], const uint8_t* aad, size_t alen, const uint8_t* in, size_t len, uint8_t* out);

void random_bytes(uint8_t* out, size_t n);

}  // namespace crypto
