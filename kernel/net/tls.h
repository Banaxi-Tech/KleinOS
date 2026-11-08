#pragma once
// Minimal TLS 1.3 client (RFC 8446): X25519 + TLS_AES_128_GCM_SHA256.
// Event driven: feed raw bytes from the transport with on_receive(), send
// whatever the client asks for via the transport callback.
//
// NOTE: certificates are NOT validated (no trust store). The connection is
// encrypted against passive eavesdroppers but not authenticated.
#include <stdint.h>
#include <stddef.h>
#include "net/crypto.h"

namespace tls {

using SendFn = int (*)(void* ctx, const uint8_t* data, size_t len);

class Client {
public:
    Client(const char* hostname, SendFn send, void* ctx);
    ~Client();
    void start();                                     // sends ClientHello
    void on_receive(const uint8_t* data, size_t len); // raw bytes from the socket
    bool ready() const { return state_ == State::Ready; }
    bool failed() const { return state_ == State::Failed; }
    bool closed() const { return closed_; }
    const char* error() const { return error_; }
    int write(const uint8_t* data, size_t len);       // application data
    size_t read(uint8_t* buf, size_t max);            // decrypted application data
    size_t available() const { return app_len_ - app_off_; }

private:
    enum class State { Idle, WaitServerHello, WaitEncrypted, Ready, Failed };
    void fail(const char* why);
    void process_records();
    void handle_record(uint8_t type, const uint8_t* body, size_t len);
    void handle_handshake(const uint8_t* msg, size_t len);
    void send_record(uint8_t type, const uint8_t* data, size_t len);
    void send_encrypted(uint8_t inner_type, const uint8_t* data, size_t len);
    void derive_handshake_keys();
    void derive_app_keys();
    void set_keys(const uint8_t secret[32], bool server);
    void push_app(const uint8_t* d, size_t n);

    char host_[256];
    SendFn send_;
    void* ctx_;
    State state_ = State::Idle;
    const char* error_ = "";
    bool closed_ = false;

    uint8_t priv_[32], pub_[32];
    uint8_t shared_[32];
    crypto::Sha256 transcript_;
    uint8_t hs_secret_[32], c_hs_[32], s_hs_[32], master_[32];

    crypto::AesGcm rkey_, wkey_;
    uint8_t riv_[12], wiv_[12];
    uint64_t rseq_ = 0, wseq_ = 0;
    bool rencrypted_ = false, wencrypted_ = false;

    uint8_t* inbuf_ = nullptr;      // raw records
    size_t in_len_ = 0, in_cap_ = 0;
    uint8_t* hsbuf_ = nullptr;      // handshake message reassembly
    size_t hs_len_ = 0, hs_cap_ = 0;
    uint8_t* app_ = nullptr;        // decrypted app data
    size_t app_len_ = 0, app_off_ = 0, app_cap_ = 0;
};

}  // namespace tls
