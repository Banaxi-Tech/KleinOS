#include "net/tls.h"
#include "lib/string.h"
#include "lib/heap.h"

namespace tls {

namespace {
inline void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
inline uint16_t get16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
inline uint32_t get24(const uint8_t* p) { return ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2]; }

void grow(uint8_t*& buf, size_t& cap, size_t need) {
    if (need <= cap) return;
    size_t nc = cap ? cap : 4096;
    while (nc < need) nc *= 2;
    uint8_t* nb = (uint8_t*)kmalloc(nc);
    if (buf) memcpy(nb, buf, cap);
    kfree(buf);
    buf = nb;
    cap = nc;
}

const uint8_t kHrrRandom[32] = {0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
                                0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};
}  // namespace

Client::Client(const char* hostname, SendFn send, void* ctx) : send_(send), ctx_(ctx) {
    size_t hn = strlen(hostname);
    if (hn > sizeof(host_) - 1) hn = sizeof(host_) - 1;
    memcpy(host_, hostname, hn);
    host_[hn] = 0;
    transcript_.init();
}

Client::~Client() {
    kfree(inbuf_);
    kfree(hsbuf_);
    kfree(app_);
    memset(priv_, 0, sizeof(priv_));
}

void Client::fail(const char* why) {
    if (state_ != State::Failed) error_ = why;
    state_ = State::Failed;
}

void Client::send_record(uint8_t type, const uint8_t* data, size_t len) {
    uint8_t hdr[5] = {type, 0x03, (uint8_t)(type == 22 && !wencrypted_ ? 0x01 : 0x03), (uint8_t)(len >> 8), (uint8_t)len};
    uint8_t* rec = (uint8_t*)kmalloc(len + 5);
    memcpy(rec, hdr, 5);
    memcpy(rec + 5, data, len);
    send_(ctx_, rec, len + 5);
    kfree(rec);
}

void Client::send_encrypted(uint8_t inner_type, const uint8_t* data, size_t len) {
    while (true) {
        size_t chunk = len > 16384 ? 16384 : len;
        size_t plen = chunk + 1;
        size_t clen = plen + 16;
        uint8_t* rec = (uint8_t*)kmalloc(clen + 5);
        rec[0] = 23; rec[1] = 3; rec[2] = 3;
        put16(rec + 3, clen);
        uint8_t* plain = (uint8_t*)kmalloc(plen);
        memcpy(plain, data, chunk);
        plain[chunk] = inner_type;
        uint8_t nonce[12];
        memcpy(nonce, wiv_, 12);
        for (int i = 0; i < 8; i++) nonce[4 + i] ^= wseq_ >> (56 - 8 * i);
        wkey_.encrypt(nonce, rec, 5, plain, plen, rec + 5, rec + 5 + plen);
        wseq_++;
        send_(ctx_, rec, clen + 5);
        kfree(plain);
        kfree(rec);
        data += chunk;
        len -= chunk;
        if (!len) break;
    }
}

void Client::start() {
    crypto::random_bytes(priv_, 32);
    crypto::x25519_public(pub_, priv_);

    uint8_t m[1024];
    size_t n = 4;   // handshake header filled at the end
    put16(m + n, 0x0303); n += 2;
    crypto::random_bytes(m + n, 32); n += 32;
    m[n++] = 32;
    crypto::random_bytes(m + n, 32); n += 32;   // legacy session id (middlebox compat)
    put16(m + n, 2); n += 2;
    put16(m + n, 0x1301); n += 2;               // TLS_AES_128_GCM_SHA256
    m[n++] = 1; m[n++] = 0;                     // compression: null
    size_t ext_len_pos = n;
    n += 2;
    size_t hl = strlen(host_);
    // server_name
    put16(m + n, 0); put16(m + n + 2, hl + 5); put16(m + n + 4, hl + 3); m[n + 6] = 0; put16(m + n + 7, hl);
    memcpy(m + n + 9, host_, hl);
    n += 9 + hl;
    // supported_groups: x25519
    put16(m + n, 10); put16(m + n + 2, 4); put16(m + n + 4, 2); put16(m + n + 6, 0x001d); n += 8;
    // signature_algorithms
    static const uint16_t sigs[] = {0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601, 0x0807, 0x0201, 0x0203};
    int ns = sizeof(sigs) / 2;
    put16(m + n, 13); put16(m + n + 2, ns * 2 + 2); put16(m + n + 4, ns * 2); n += 6;
    for (int i = 0; i < ns; i++) { put16(m + n, sigs[i]); n += 2; }
    // supported_versions: TLS 1.3
    put16(m + n, 43); put16(m + n + 2, 3); m[n + 4] = 2; put16(m + n + 5, 0x0304); n += 7;
    // psk_key_exchange_modes
    put16(m + n, 45); put16(m + n + 2, 2); m[n + 4] = 1; m[n + 5] = 1; n += 6;
    // key_share
    put16(m + n, 51); put16(m + n + 2, 38); put16(m + n + 4, 36); put16(m + n + 6, 0x001d); put16(m + n + 8, 32);
    memcpy(m + n + 10, pub_, 32);
    n += 42;
    // ALPN http/1.1
    put16(m + n, 16); put16(m + n + 2, 11); put16(m + n + 4, 9); m[n + 6] = 8; memcpy(m + n + 7, "http/1.1", 8); n += 15;
    // ec_point_formats (some stacks want it)
    put16(m + n, 11); put16(m + n + 2, 2); m[n + 4] = 1; m[n + 5] = 0; n += 6;
    put16(m + ext_len_pos, n - ext_len_pos - 2);
    m[0] = 1;
    m[1] = (n - 4) >> 16; m[2] = (n - 4) >> 8; m[3] = n - 4;
    transcript_.update(m, n);
    send_record(22, m, n);
    state_ = State::WaitServerHello;
}

void Client::set_keys(const uint8_t secret[32], bool server) {
    uint8_t key[16];
    if (server) {
        crypto::hkdf_expand_label(secret, "key", nullptr, 0, key, 16);
        crypto::hkdf_expand_label(secret, "iv", nullptr, 0, riv_, 12);
        rkey_.init(key);
        rseq_ = 0;
        rencrypted_ = true;
    } else {
        crypto::hkdf_expand_label(secret, "key", nullptr, 0, key, 16);
        crypto::hkdf_expand_label(secret, "iv", nullptr, 0, wiv_, 12);
        wkey_.init(key);
        wseq_ = 0;
        wencrypted_ = true;
    }
}

void Client::derive_handshake_keys() {
    uint8_t zero[32] = {0}, early[32], empty_hash[32], derived[32], th[32];
    crypto::hkdf_extract(nullptr, 0, zero, 32, early);
    crypto::sha256("", 0, empty_hash);
    crypto::hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
    crypto::hkdf_extract(derived, 32, shared_, 32, hs_secret_);
    transcript_.final(th);
    crypto::hkdf_expand_label(hs_secret_, "c hs traffic", th, 32, c_hs_, 32);
    crypto::hkdf_expand_label(hs_secret_, "s hs traffic", th, 32, s_hs_, 32);
    crypto::hkdf_expand_label(hs_secret_, "derived", empty_hash, 32, derived, 32);
    crypto::hkdf_extract(derived, 32, zero, 32, master_);
    set_keys(s_hs_, true);
}

void Client::derive_app_keys() {
    uint8_t th[32], c_ap[32], s_ap[32];
    transcript_.final(th);
    crypto::hkdf_expand_label(master_, "c ap traffic", th, 32, c_ap, 32);
    crypto::hkdf_expand_label(master_, "s ap traffic", th, 32, s_ap, 32);
    // client Finished (over transcript through server Finished)
    uint8_t fkey[32], verify[32];
    crypto::hkdf_expand_label(c_hs_, "finished", nullptr, 0, fkey, 32);
    crypto::hmac_sha256(fkey, 32, th, 32, verify);
    uint8_t fin[36] = {20, 0, 0, 32};
    memcpy(fin + 4, verify, 32);
    // compat CCS, then Finished under the client handshake key
    uint8_t ccs = 1;
    send_record(20, &ccs, 1);
    set_keys(c_hs_, false);
    send_encrypted(22, fin, 36);
    set_keys(c_ap, false);
    set_keys(s_ap, true);
    state_ = State::Ready;
}

void Client::handle_handshake(const uint8_t* msg, size_t len) {
    uint8_t type = msg[0];
    if (state_ == State::WaitServerHello) {
        if (type != 2) { fail("expected ServerHello"); return; }
        const uint8_t* p = msg + 4;
        size_t n = len - 4;
        if (n < 38) { fail("short ServerHello"); return; }
        if (memcmp(p + 2, kHrrRandom, 32) == 0) { fail("server requested a different key exchange group"); return; }
        size_t o = 34;
        uint8_t sid = p[o++];
        o += sid;
        if (o + 3 > n) { fail("bad ServerHello"); return; }
        uint16_t suite = get16(p + o);
        o += 3;
        if (suite != 0x1301) { fail("server picked an unsupported cipher suite"); return; }
        if (o + 2 > n) { fail("ServerHello without extensions (TLS 1.2 server?)"); return; }
        size_t elen = get16(p + o);
        o += 2;
        bool tls13 = false, have_key = false;
        size_t end = o + elen > n ? n : o + elen;
        while (o + 4 <= end) {
            uint16_t et = get16(p + o), el = get16(p + o + 2);
            const uint8_t* ev = p + o + 4;
            if (o + 4 + el > end) break;
            if (et == 43 && el == 2 && get16(ev) == 0x0304) tls13 = true;
            if (et == 51 && el >= 36 && get16(ev) == 0x001d && get16(ev + 2) == 32) {
                crypto::x25519(shared_, priv_, ev + 4);
                have_key = true;
            }
            o += 4 + el;
        }
        if (!tls13) { fail("server does not support TLS 1.3"); return; }
        if (!have_key) { fail("no X25519 key share from server"); return; }
        transcript_.update(msg, len);
        derive_handshake_keys();
        state_ = State::WaitEncrypted;
        return;
    }
    if (state_ == State::WaitEncrypted) {
        if (type == 20) {   // server Finished: verify
            uint8_t th[32], fkey[32], expect[32];
            transcript_.final(th);
            crypto::hkdf_expand_label(s_hs_, "finished", nullptr, 0, fkey, 32);
            crypto::hmac_sha256(fkey, 32, th, 32, expect);
            if (len != 36 || memcmp(expect, msg + 4, 32) != 0) { fail("server Finished verification failed"); return; }
            transcript_.update(msg, len);
            derive_app_keys();
            return;
        }
        // EncryptedExtensions(8), Certificate(11), CertificateVerify(15), CertificateRequest(13)
        if (type == 13) { fail("server requires a client certificate"); return; }
        transcript_.update(msg, len);
        return;
    }
    // post-handshake: NewSessionTicket(4), KeyUpdate(24)
    if (state_ == State::Ready && type == 24) {
        // update our receive key: s_ap_{n+1} — we don't keep s_ap, so treat as unsupported but rare.
        fail("KeyUpdate not supported");
    }
}

void Client::push_app(const uint8_t* d, size_t n) {
    if (app_off_ > 0 && app_off_ == app_len_) { app_off_ = app_len_ = 0; }
    if (app_off_ > 65536) {
        memmove(app_, app_ + app_off_, app_len_ - app_off_);
        app_len_ -= app_off_;
        app_off_ = 0;
    }
    grow(app_, app_cap_, app_len_ + n);
    memcpy(app_ + app_len_, d, n);
    app_len_ += n;
}

void Client::handle_record(uint8_t type, const uint8_t* body, size_t len) {
    if (type == 20) return;   // ChangeCipherSpec: ignore
    uint8_t* plain = nullptr;
    if (type == 23 && rencrypted_) {
        if (len < 17) { fail("short encrypted record"); return; }
        uint8_t nonce[12];
        memcpy(nonce, riv_, 12);
        for (int i = 0; i < 8; i++) nonce[4 + i] ^= rseq_ >> (56 - 8 * i);
        uint8_t aad[5] = {23, 3, 3, (uint8_t)(len >> 8), (uint8_t)len};
        plain = (uint8_t*)kmalloc(len);
        if (!rkey_.decrypt(nonce, aad, 5, body, len - 16, plain, body + len - 16)) {
            kfree(plain);
            fail("record decryption failed (bad MAC)");
            return;
        }
        rseq_++;
        size_t n = len - 16;
        while (n > 0 && plain[n - 1] == 0) n--;
        if (n == 0) { kfree(plain); fail("empty inner record"); return; }
        type = plain[n - 1];
        body = plain;
        len = n - 1;
    }
    if (type == 21) {   // alert
        if (len >= 2 && body[1] == 0) closed_ = true;
        else if (len >= 2) {
            static char msg[48];
            snprintf(msg, sizeof(msg), "server alert %d", body[1]);
            fail(msg);
        }
    } else if (type == 22) {
        grow(hsbuf_, hs_cap_, hs_len_ + len);
        memcpy(hsbuf_ + hs_len_, body, len);
        hs_len_ += len;
        size_t off = 0;
        while (hs_len_ - off >= 4) {
            size_t mlen = get24(hsbuf_ + off + 1) + 4;
            if (hs_len_ - off < mlen) break;
            handle_handshake(hsbuf_ + off, mlen);
            if (state_ == State::Failed) break;
            off += mlen;
        }
        memmove(hsbuf_, hsbuf_ + off, hs_len_ - off);
        hs_len_ -= off;
    } else if (type == 23) {
        if (state_ == State::Ready) push_app(body, len);
    }
    kfree(plain);
}

void Client::process_records() {
    size_t off = 0;
    while (state_ != State::Failed && in_len_ - off >= 5) {
        uint8_t type = inbuf_[off];
        size_t len = get16(inbuf_ + off + 3);
        if (len > 16384 + 256) { fail("oversized record"); break; }
        if (in_len_ - off < 5 + len) break;
        handle_record(type, inbuf_ + off + 5, len);
        off += 5 + len;
    }
    memmove(inbuf_, inbuf_ + off, in_len_ - off);
    in_len_ -= off;
}

void Client::on_receive(const uint8_t* data, size_t len) {
    if (state_ == State::Failed) return;
    grow(inbuf_, in_cap_, in_len_ + len);
    memcpy(inbuf_ + in_len_, data, len);
    in_len_ += len;
    process_records();
}

int Client::write(const uint8_t* data, size_t len) {
    if (state_ != State::Ready) return -1;
    send_encrypted(23, data, len);
    return (int)len;
}

size_t Client::read(uint8_t* buf, size_t max) {
    size_t n = app_len_ - app_off_;
    if (n > max) n = max;
    memcpy(buf, app_ + app_off_, n);
    app_off_ += n;
    return n;
}

}  // namespace tls
