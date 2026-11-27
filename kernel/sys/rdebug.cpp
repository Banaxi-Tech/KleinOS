#include "sys/rdebug.h"
#include "sys/dl.h"
#include "sys/system.h"
#include "sys/update.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "drivers/power.h"
#include "drivers/rtc.h"
#include "fs/fat32.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"
#include "net/crypto.h"
#include "net/net.h"
#include "ui/wm.h"

#if defined(__aarch64__)
#define FRAME_PC(f) ((f)->pc)
#else
#define FRAME_PC(f) ((f)->rip)
#endif

namespace rdebug {

namespace {

constexpr char kConfigPath[] = "/KleinOS/rdebug.cfg";
constexpr char kConfigMagic[8] = {'K', 'D', 'B', 'G', 'C', 'F', 'G', '1'};
constexpr char kHello[6] = {'K', 'D', 'B', 'G', '1', '\n'};
constexpr size_t kMaxFrame = 64 * 1024, kOutSize = 256 * 1024, kMaxUpload = 32 * 1024 * 1024;
constexpr size_t kHelloSize = 6 + 16 + 32 + 16, kClientSize = 32 + 16 + 32;
constexpr uint64_t kHandshakeMs = 10000, kSessionIdleMs = 10 * 60 * 1000;
constexpr int kMaxBreakpoints = 16;

struct Config {
    char magic[8];
    uint32_t enabled;
    uint8_t salt[16];
    uint8_t psk[32];          // derived from the password; the password itself is not kept
    uint32_t has_password;
};

enum class Phase { None, Handshake, Session };

enum Cmd : uint8_t {
    CmdInfo = 1, CmdLog, CmdPeek, CmdPoke, CmdSymbol, CmdModules,
    CmdBreak, CmdUnbreak, CmdContinue, CmdStep, CmdRegs, CmdStatus,
    CmdReboot = 20,
    CmdUpdateCheck = 30, CmdUpdateStatus, CmdUpdateInstall, CmdUploadBegin, CmdUploadChunk, CmdUploadCommit,
};

struct Breakpoint { uintptr_t addr; uint8_t orig; bool used; };

Config g_cfg;
Phase g_phase;
int g_sock = -1;
uint64_t g_deadline;
int g_failures;
uint64_t g_locked_until;
char g_peer[16];

uint8_t g_priv[32], g_pub[32], g_server_nonce[16];
crypto::Sha256 g_transcript;
crypto::AesGcm g_rx, g_tx;
uint64_t g_rx_count, g_tx_count;

uint8_t g_in[kMaxFrame + 32];
size_t g_in_len;
uint8_t g_plain[kMaxFrame + 32];
uint8_t g_reply[kMaxFrame + 32];
uint8_t g_out[kOutSize];
size_t g_out_len;

uint8_t* g_upload;
size_t g_upload_size, g_upload_got;

Breakpoint g_breaks[kMaxBreakpoints];
InterruptFrame* g_stop_frame;
volatile bool g_resume, g_step_next;
Breakpoint* g_rearm;      // removed while stepping off it, goes back after one instruction
bool g_in_serve;

void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8)); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); }
uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8); return v; }
uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (i * 8); return v; }

bool same_bytes(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

void save_config() {
    if (!fat32::write_file(kConfigPath, &g_cfg, sizeof(g_cfg), rtc_unix_time()) || !fat32::flush())
        klog("rdebug: settings could not be saved, they last until restart");
}

void make_nonce(uint8_t out[12], uint64_t count) {
    memset(out, 0, 4);
    put64(out + 4, count);
}

void flush_out() {
    if (g_sock < 0 || !g_out_len) return;
    int sent = net::tcp_send(g_sock, g_out, g_out_len);
    if (sent <= 0) return;
    memmove(g_out, g_out + sent, g_out_len - sent);
    g_out_len -= sent;
}

void queue_raw(const void* data, size_t n) {
    if (g_out_len + n > kOutSize) return;
    memcpy(g_out + g_out_len, data, n);
    g_out_len += n;
}

void send_frame(const uint8_t* data, size_t n) {
    uint8_t hdr[4], nonce[12], tag[16];
    if (g_out_len + n + 20 > kOutSize) return;
    put32(hdr, (uint32_t)(n + 16));
    queue_raw(hdr, 4);
    make_nonce(nonce, g_tx_count++);
    g_tx.encrypt(nonce, nullptr, 0, data, n, g_out + g_out_len, tag);
    g_out_len += n;
    queue_raw(tag, 16);
}

void reply_status(uint8_t status, const void* body = nullptr, size_t n = 0) {
    g_reply[0] = status;
    if (n > kMaxFrame - 1) n = kMaxFrame - 1;
    if (n) memcpy(g_reply + 1, body, n);
    send_frame(g_reply, n + 1);
}

void reply_text(const char* text) { reply_status(0, text, strlen(text)); }
void reply_error(const char* text) { reply_status(1, text, strlen(text)); }

// Remove every breakpoint from the code.
void clear_all_breakpoints() {
    for (Breakpoint& b : g_breaks) {
        if (!b.used) continue;
        *(volatile uint8_t*)b.addr = b.orig;
        b.used = false;
    }
}

void close_session(const char* why) {
    if (g_sock >= 0) net::tcp_close(g_sock);
    if (g_phase == Phase::Session) {
        klog("rdebug: %s disconnected (%s)", g_peer, why);
        clear_all_breakpoints();
        g_resume = true;
    }
    g_sock = -1;
    g_phase = Phase::None;
    g_in_len = g_out_len = 0;
    g_peer[0] = 0;
    kfree(g_upload);
    g_upload = nullptr;
    g_upload_size = g_upload_got = 0;
    crypto::random_bytes(g_priv, sizeof(g_priv));   // session secrets don't linger
}

void start_handshake(int sock) {
    g_sock = sock;
    g_phase = Phase::Handshake;
    g_in_len = g_out_len = 0;
    g_rx_count = g_tx_count = 0;
    g_deadline = timer_ms() + kHandshakeMs;
    crypto::random_bytes(g_priv, sizeof(g_priv));
    crypto::random_bytes(g_server_nonce, sizeof(g_server_nonce));
    crypto::x25519_public(g_pub, g_priv);
    uint8_t hello[kHelloSize];
    memcpy(hello, kHello, 6);
    memcpy(hello + 6, g_cfg.salt, 16);
    memcpy(hello + 22, g_pub, 32);
    memcpy(hello + 54, g_server_nonce, 16);
    g_transcript.init();
    g_transcript.update(hello, sizeof(hello));
    queue_raw(hello, sizeof(hello));
}

void login_failed() {
    g_failures++;
    int shift = g_failures > 10 ? 10 : g_failures;
    g_locked_until = timer_ms() + ((uint64_t)5000 << shift);
    klog("rdebug: failed login from %s (%d in a row)", g_peer, g_failures);
    close_session("bad password");
}

void finish_handshake() {
    const uint8_t* client_pub = g_in, *mac = g_in + 48;
    g_transcript.update(g_in, 48);
    uint8_t th[32], shared[32], prk[32], mac_key[32], want[32];
    g_transcript.final(th);
    crypto::x25519(shared, g_priv, client_pub);
    uint8_t zero[32] = {};
    if (same_bytes(shared, zero, 32)) { login_failed(); return; }
    crypto::hkdf_extract(shared, 32, g_cfg.psk, 32, prk);
    crypto::hkdf_expand(prk, (const uint8_t*)"mac", 3, mac_key, 32);
    uint8_t msg[6 + 32];
    memcpy(msg, "client", 6);
    memcpy(msg + 6, th, 32);
    crypto::hmac_sha256(mac_key, 32, msg, sizeof(msg), want);
    if (!same_bytes(want, mac, 32)) { login_failed(); return; }
    memcpy(msg, "server", 6);
    crypto::hmac_sha256(mac_key, 32, msg, sizeof(msg), want);
    queue_raw(want, 32);
    uint8_t key[16];
    crypto::hkdf_expand(prk, (const uint8_t*)"c2s", 3, key, 16);
    g_rx.init(key);
    crypto::hkdf_expand(prk, (const uint8_t*)"s2c", 3, key, 16);
    g_tx.init(key);
    memset(shared, 0, sizeof(shared));
    memset(prk, 0, sizeof(prk));
    memset(mac_key, 0, sizeof(mac_key));
    g_failures = 0;
    g_phase = Phase::Session;
    g_deadline = timer_ms() + kSessionIdleMs;
    klog("rdebug: debugger connected from %s", g_peer);
    char body[64];
    snprintf(body, sizeof(body), "Connected from %s", g_peer);
    wm::notify("Wi-Fi Debugging", body);
}

// Only the first 4 GiB are identity mapped; anything else would fault.
bool valid_range(uint64_t addr, uint64_t len) { return len <= kMaxFrame && addr + len <= 0x100000000ull && addr + len >= addr; }

Breakpoint* find_break(uintptr_t addr) {
    for (Breakpoint& b : g_breaks) if (b.used && b.addr == addr) return &b;
    return nullptr;
}

void handle_info() {
    static char text[512];
    net::Status st = net::status();
    char ip[16];
    net::ip_to_string(st.ip, ip);
    snprintf(text, sizeof(text), "%s\narch %s\nip %s\nnic %s\nmodules %zu\nuptime %llu ms\nupdate slot %c\n", system_version_string(),
             sizeof(void*) == 8 ? "x86_64" : "i386", ip, net::nic_name(), dl_modules().size(), (unsigned long long)timer_ms(),
             update::active_slot() ? update::active_slot() : '-');
    reply_text(text);
}

void handle_modules() {
    static char text[32000];
    size_t n = 0;
    for (DlModule* m : dl_modules()) {
        int w = snprintf(text + n, sizeof(text) - n, "%016llx %8zu %s\n", (unsigned long long)(uintptr_t)m->base, m->mem_size, m->path.c_str());
        if (w <= 0 || n + w >= sizeof(text)) break;
        n += w;
    }
    reply_status(0, text, n);
}

void handle_regs() {
    if (!g_stop_frame) { reply_error("not stopped"); return; }
    static char text[1024];
    InterruptFrame* f = g_stop_frame;
#if defined(__aarch64__)
    int n = snprintf(text, sizeof(text), "pc %016llx\nsp %016llx\npstate %016llx\n", (unsigned long long)f->pc,
                     (unsigned long long)f->sp, (unsigned long long)f->pstate);
    for (int i = 0; i < 31 && n < (int)sizeof(text) - 32; i++)
        n += snprintf(text + n, sizeof(text) - n, "x%d %016llx\n", i, (unsigned long long)f->x[i]);
#else
    snprintf(text, sizeof(text), "rip %016llx\nrsp %016llx\nrbp %016llx\nrflags %016llx\nrax %016llx\nrbx %016llx\nrcx %016llx\nrdx %016llx\nrsi %016llx\nrdi %016llx\n",
             (unsigned long long)f->rip, (unsigned long long)f->rsp, (unsigned long long)f->rbp, (unsigned long long)f->rflags,
             (unsigned long long)f->rax, (unsigned long long)f->rbx, (unsigned long long)f->rcx, (unsigned long long)f->rdx,
             (unsigned long long)f->rsi, (unsigned long long)f->rdi);
#endif
    reply_text(text);
}

void handle_update_status() {
    static char text[256];
    snprintf(text, sizeof(text), "state %d\nprogress %d\nlatest %s\nerror %s\n", (int)update::state(), update::progress(), update::latest_version(), update::error());
    reply_text(text);
}

void handle_command(const uint8_t* p, size_t n) {
    if (!n) { reply_error("empty command"); return; }
    uint8_t cmd = p[0];
    p++; n--;
    switch (cmd) {
    case CmdInfo: handle_info(); return;
    case CmdLog: {
        static char text[kMaxFrame - 16];
        size_t len = klog_snapshot(text, sizeof(text));
        reply_status(0, text, len);
        return;
    }
    case CmdPeek: {
        if (n < 12) break;
        uint64_t addr = get64(p);
        uint32_t len = get32(p + 8);
        if (!valid_range(addr, len)) { reply_error("address out of range"); return; }
        reply_status(0, (const void*)(uintptr_t)addr, len);
        return;
    }
    case CmdPoke: {
        if (n < 8) break;
        uint64_t addr = get64(p);
        if (!valid_range(addr, n - 8)) { reply_error("address out of range"); return; }
        for (size_t i = 8; i < n; i++) ((volatile uint8_t*)(uintptr_t)addr)[i - 8] = p[i];
        reply_text("ok");
        return;
    }
    case CmdSymbol: {
        char name[256];
        size_t len = n < sizeof(name) - 1 ? n : sizeof(name) - 1;
        memcpy(name, p, len);
        name[len] = 0;
        void* a = kernel_symbol(name);
        if (!a) { reply_error("no such symbol"); return; }
        uint8_t out[8];
        put64(out, (uint64_t)(uintptr_t)a);
        reply_status(0, out, 8);
        return;
    }
    case CmdModules: handle_modules(); return;
#if defined(__aarch64__)
    case CmdBreak: case CmdUnbreak: case CmdContinue: case CmdStep:
        reply_error("breakpoints and stepping are not supported on ARM yet");
        return;
#else
    case CmdBreak: {
        if (n < 8) break;
        uint64_t addr = get64(p);
        if (!valid_range(addr, 1)) { reply_error("address out of range"); return; }
        if (find_break((uintptr_t)addr)) { reply_text("already set"); return; }
        for (Breakpoint& b : g_breaks) {
            if (b.used) continue;
            b.addr = (uintptr_t)addr;
            b.orig = *(volatile uint8_t*)b.addr;
            b.used = true;
            *(volatile uint8_t*)b.addr = 0xCC;
            reply_text("ok");
            return;
        }
        reply_error("no free breakpoints");
        return;
    }
    case CmdUnbreak: {
        if (n < 8) break;
        Breakpoint* b = find_break((uintptr_t)get64(p));
        if (!b) { reply_error("no breakpoint there"); return; }
        *(volatile uint8_t*)b->addr = b->orig;
        b->used = false;
        if (g_rearm == b) g_rearm = nullptr;
        reply_text("ok");
        return;
    }
    case CmdContinue: case CmdStep:
        if (!g_stop_frame) { reply_error("not stopped"); return; }
        g_step_next = cmd == CmdStep;
        g_resume = true;
        reply_text("ok");
        return;
#endif
    case CmdRegs: handle_regs(); return;
    case CmdStatus: {
        static char text[64];
        if (g_stop_frame) snprintf(text, sizeof(text), "stopped at %016llx", (unsigned long long)FRAME_PC(g_stop_frame));
        else strlcpy(text, "running", sizeof(text));
        reply_text(text);
        return;
    }
    case CmdReboot:
        reply_text("ok");
        flush_out();
        power_reboot();
    case CmdUpdateCheck:
        if (update::check()) reply_text("ok"); else reply_error("the updater is busy or not available on this boot");
        return;
    case CmdUpdateStatus: handle_update_status(); return;
    case CmdUpdateInstall:
        if (update::install()) reply_text("ok"); else reply_error("no update is ready to install");
        return;
    case CmdUploadBegin: {
        if (n < 4) break;
        uint32_t size = get32(p);
        if (!size || size > kMaxUpload) { reply_error("bad size"); return; }
        kfree(g_upload);
        g_upload = (uint8_t*)kmalloc(size);
        g_upload_size = g_upload ? size : 0;
        g_upload_got = 0;
        if (g_upload) reply_text("ok"); else reply_error("out of memory");
        return;
    }
    case CmdUploadChunk:
        if (!g_upload || g_upload_got + n > g_upload_size) { reply_error("unexpected data"); return; }
        memcpy(g_upload + g_upload_got, p, n);
        g_upload_got += n;
        reply_text("ok");
        return;
    case CmdUploadCommit:
        if (!g_upload || g_upload_got != g_upload_size) { reply_error("upload incomplete"); return; }
        // the updater checks the signature like any other update and keeps the buffer
        if (update::install_package(g_upload, g_upload_size)) { g_upload = nullptr; g_upload_size = g_upload_got = 0; reply_text("ok"); }
        else reply_error(update::error()[0] ? update::error() : "the updater is busy or not available on this boot");
        return;
    default: reply_error("unknown command"); return;
    }
    reply_error("malformed command");
}

// Decrypts and handles every complete frame that has arrived.
void process_frames() {
    while (g_phase == Phase::Session && g_in_len >= 4 && g_out_len + kMaxFrame + 64 <= kOutSize) {
        uint32_t len = get32(g_in);
        if (len < 16 || len > kMaxFrame + 16) { close_session("bad frame"); return; }
        if (g_in_len < 4 + len) return;
        uint8_t nonce[12];
        make_nonce(nonce, g_rx_count++);
        if (!g_rx.decrypt(nonce, nullptr, 0, g_in + 4, len - 16, g_plain, g_in + 4 + len - 16)) { close_session("bad frame"); return; }
        size_t used = 4 + len;
        memmove(g_in, g_in + used, g_in_len - used);
        g_in_len -= used;
        g_deadline = timer_ms() + kSessionIdleMs;
        handle_command(g_plain, len - 16);
    }
}

void serve() {
    if (g_in_serve) return;
    g_in_serve = true;
    uint64_t now = timer_ms();
    if (g_phase == Phase::None) {
        int s = g_cfg.enabled ? net::tcp_accept() : -1;
        if (s >= 0) {
            net::ip_to_string(net::tcp_peer_ip(s), g_peer);
            if (now < g_locked_until) net::tcp_close(s);
            else start_handshake(s);
        }
    }
    if (g_phase != Phase::None) {
        if (net::tcp_state(g_sock) != net::TcpState::Open || net::tcp_peer_closed(g_sock)) close_session("connection closed");
        else if (now > g_deadline) { if (g_phase == Phase::Handshake) login_failed(); else close_session("idle"); }
    }
    if (g_phase != Phase::None) {
        size_t room = sizeof(g_in) - g_in_len;
        if (room) g_in_len += net::tcp_recv(g_sock, g_in + g_in_len, room);
        if (g_phase == Phase::Handshake && g_in_len >= kClientSize) {
            finish_handshake();
            if (g_phase == Phase::Session) { memmove(g_in, g_in + kClientSize, g_in_len - kClientSize); g_in_len -= kClientSize; }
        }
        process_frames();
        flush_out();
    }
    g_in_serve = false;
}

void poll_hook() { if (g_cfg.enabled || g_phase != Phase::None) serve(); }

// The whole machine waits here, the debugger connection stays up.
void stop_here(InterruptFrame* f) {
    g_stop_frame = f;
    g_resume = false;
    g_step_next = false;
    klog("rdebug: stopped at %p", (void*)(uintptr_t)FRAME_PC(f));
    sti();
    while (!g_resume && g_phase == Phase::Session) {
        net::poll();
        serve();
    }
    cli();
    flush_out();
    g_stop_frame = nullptr;
}

#if !defined(__aarch64__)
constexpr uintptr_t kTrapFlag = 1 << 8;

// #BP and #DB. Returns false if it wasn't ours, which still ends in a panic.
bool trap(InterruptFrame* f) {
    if (f->vector == 1) {
        if (g_rearm) { *(volatile uint8_t*)g_rearm->addr = 0xCC; g_rearm = nullptr; }
        if (g_step_next && g_phase == Phase::Session) stop_here(f);
        if (!(g_step_next && g_phase == Phase::Session)) f->rflags &= ~kTrapFlag;
        return true;
    }
    Breakpoint* b = find_break(f->rip - 1);
    if (!b) return false;
    f->rip -= 1;
    *(volatile uint8_t*)b->addr = b->orig;
    if (g_in_serve || net::in_poll() || g_phase != Phase::Session) {
        // stopping inside the network code would stop the debugger's own connection
        klog("rdebug: breakpoint at %p removed, it is in code the debugger itself needs", (void*)b->addr);
        b->used = false;
        return true;
    }
    stop_here(f);
    if (b->used) { g_rearm = b; f->rflags |= kTrapFlag; }
    else if (g_step_next) f->rflags |= kTrapFlag;
    return true;
}
#endif

}  // namespace

void init() {
    uint8_t* data;
    size_t size;
    if (fat32::read_file(kConfigPath, &data, &size)) {
        if (size == sizeof(Config) && !memcmp(data, kConfigMagic, 8)) memcpy(&g_cfg, data, sizeof(Config));
        kfree(data);
    }
    memcpy(g_cfg.magic, kConfigMagic, 8);
#if !defined(__aarch64__)
    set_debug_trap(trap);
#endif
    if (g_cfg.enabled && g_cfg.has_password) net::tcp_listen(kPort);
    else g_cfg.enabled = 0;
    system_add_poll_hook(poll_hook);
    if (g_cfg.enabled) klog("rdebug: Wi-Fi Debugging is on");
}

bool has_password() { return g_cfg.has_password != 0; }
bool enabled() { return g_cfg.enabled != 0; }
const char* peer() { return g_phase == Phase::Session ? g_peer : nullptr; }

bool set_password(const char* password) {
    if (strlen(password) < (size_t)kMinPassword) return false;
    crypto::random_bytes(g_cfg.salt, sizeof(g_cfg.salt));
    crypto::wpa_psk(password, g_cfg.salt, sizeof(g_cfg.salt), g_cfg.psk);
    g_cfg.has_password = 1;
    if (g_phase != Phase::None) close_session("password changed");
    g_failures = 0;
    g_locked_until = 0;
    save_config();
    return true;
}

bool set_enabled(bool on) {
    if (on && !g_cfg.has_password) return false;
    g_cfg.enabled = on;
    if (on) net::tcp_listen(kPort);
    else {
        net::tcp_listen(0);
        if (g_phase != Phase::None) close_session("turned off");
    }
    save_config();
    klog("rdebug: Wi-Fi Debugging %s", on ? "on" : "off");
    return true;
}

}  // namespace rdebug
