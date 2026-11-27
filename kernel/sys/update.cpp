#include "drivers/input.h"
#include "sys/update.h"
#include "sys/update_key.h"
#include "sys/sysconfig.h"
#include "sys/session.h"
#include "sys/system.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "drivers/rtc.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "gfx/canvas.h"
#include "lib/heap.h"
#include "lib/kstring.h"
#include "lib/log.h"
#include "lib/math.h"
#include "lib/string.h"
#include "net/crypto.h"
#include "net/http.h"
#include "net/net.h"
#include "ui/wm.h"

namespace update {
namespace {

#if defined(__aarch64__)
constexpr const char* kArch = "aarch64";
constexpr uint32_t kArchId = 3;
#elif defined(__i386__)
constexpr const char* kArch = "i386";
constexpr uint32_t kArchId = 1;
#else
constexpr const char* kArch = "x86_64";
constexpr uint32_t kArchId = 2;
#endif
constexpr uint32_t kFormat = 1, kPatchFormat = 2;
constexpr size_t kMaxPackage = 64u << 20;
constexpr uint64_t kConfirmAfterMs = 30000;
constexpr uint64_t kFirstCheckMs = 45000;
constexpr uint64_t kRecheckMs = 6ull * 3600 * 1000;
constexpr uint64_t kWorkBudgetMs = 8;
constexpr size_t kChunk = 64u << 10;

struct [[gnu::packed]] Payload {
    uint32_t comp_size, raw_size;
    uint8_t comp_sha[32], raw_sha[32];
};
struct [[gnu::packed]] Header {
    char magic[4];
    uint32_t format, arch;
    uint32_t main, sub, build;
    uint32_t min_main, min_sub, min_build;
    uint32_t reserved;
    Payload payload[2];      // kernel, initrd
    uint8_t signature[64];
};
constexpr size_t kSignedBytes = sizeof(Header) - 64;

struct Version {
    uint32_t main = 0, sub = 0, build = 0;
    int compare(const Version& o) const {
        if (main != o.main) return main < o.main ? -1 : 1;
        if (sub != o.sub) return sub < o.sub ? -1 : 1;
        if (build != o.build) return build < o.build ? -1 : 1;
        return 0;
    }
};

enum class Step { None, Manifest, Download, HashPackage, Inflate, Write, ReadBack, Commit };

State g_state = State::Unsupported;
Step g_step = Step::None;
char g_slot = 0;
char g_env_slot = 0, g_env_try = 0;
char g_env_pending[24], g_env_bad[24];
bool g_confirmed = true;

net::HttpRequest* g_req;
char g_error[120];
char g_latest[24];
char g_url[512], g_patch_url[512];
bool g_using_patch;
Version g_latest_v;
int g_progress;
bool g_auto_install;
uint64_t g_next_check;

Header g_header;
uint8_t* g_raw[2];
uint32_t g_comp_offset[2];
int g_job;                  // payload being hashed, written or read back
size_t g_pos;
crypto::Sha256 g_sha;

Version current_version() {
    Version v;
    const char* s = KLEIN_VERSION;
    v.main = (uint32_t)atoi(s);
    const char* dot = strchr(s, '.');
    if (dot) v.sub = (uint32_t)atoi(dot + 1);
    v.build = (uint32_t)system_build_number();
    return v;
}

bool parse_version(const char* s, Version* out) {
    const char* a = strchr(s, '.');
    const char* b = a ? strchr(a + 1, '.') : nullptr;
    if (!a || !b) return false;
    out->main = (uint32_t)atoi(s);
    out->sub = (uint32_t)atoi(a + 1);
    out->build = (uint32_t)atoi(b + 1);
    return true;
}

// grubenv value of `key`, copied into out; empty if absent.
void env_get(const char* text, const char* key, char* out, size_t size) {
    out[0] = 0;
    size_t klen = strlen(key);
    for (const char* p = text; *p && *p != '#'; ) {
        const char* e = strchr(p, '\n');
        if (!e) break;
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            size_t n = (size_t)(e - (p + klen + 1));
            if (n >= size) n = size - 1;
            memcpy(out, p + klen + 1, n);
            out[n] = 0;
            return;
        }
        p = e + 1;
    }
}

const char kEnvPath[] = "/boot/grubenv";

bool env_read() {
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!fat32::read_file(kEnvPath, &data, &size)) return false;
    static const char kHead[] = "# GRUB Environment Block\n";
    bool ok = size == 1024 && !memcmp(data, kHead, sizeof(kHead) - 1);
    if (ok) {
        data[1023] = 0;
        const char* body = (const char*)data + sizeof(kHead) - 1;
        char v[24];
        env_get(body, "slot", v, sizeof(v));
        g_env_slot = v[0] == 'b' ? 'b' : 'a';
        env_get(body, "try_slot", v, sizeof(v));
        g_env_try = v[0] == 'a' || v[0] == 'b' ? v[0] : 0;
        env_get(body, "pending", g_env_pending, sizeof(g_env_pending));
        env_get(body, "bad", g_env_bad, sizeof(g_env_bad));
    }
    kfree(data);
    return ok;
}

// grubenv keeps its size, so this rewrites the same sectors GRUB reads.
bool env_write() {
    static uint8_t blk[1024];
    memset(blk, '#', sizeof(blk));
    char text[160];
    char tr[2] = {g_env_try, 0};
    int n = snprintf(text, sizeof(text), "# GRUB Environment Block\nslot=%c\ntry_slot=%s\npending=%s\nbad=%s\n", g_env_slot, tr,
                     g_env_pending, g_env_bad);
    memcpy(blk, text, (size_t)n);
    return fat32::write_file(kEnvPath, blk, sizeof(blk), rtc_unix_time()) && fat32::flush();
}

void free_work() {
    for (uint8_t*& p : g_raw) { kfree(p); p = nullptr; }
    if (g_req) { net::http_free(g_req); g_req = nullptr; }
    g_step = Step::None;
}

void start_download();

uint8_t* g_local;           // package handed over by install_package, instead of g_req
size_t g_local_size;

const uint8_t* package_body(size_t* len) {
    if (g_local) { *len = g_local_size; return g_local; }
    return net::http_body(g_req, len);
}

void fail(const char* msg) {
    if (g_using_patch) {                // anything wrong with a patch: the full package still works
        klog("update: patch failed (%s), downloading the full update", msg);
        g_patch_url[0] = 0;
        free_work();
        start_download();
        return;
    }
    klog("update: failed: %s", msg);
    strlcpy(g_error, msg, sizeof(g_error));
    free_work();
    g_state = State::Failed;
    g_auto_install = false;
}

// Reads a string or number value of "key" from the unsigned manifest.
bool json_get(const char* json, const char* key, char* out, size_t size) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    bool quoted = *p == '"';
    if (quoted) p++;
    size_t n = 0;
    while (*p && n + 1 < size && (quoted ? *p != '"' : (*p != ',' && *p != '}'))) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

bool auto_update_enabled() {
    String path = String("/Users/") + session_user() + "/Library/Settings/prefs.txt";
    String text;
    if (!vfs::read_text(path.c_str(), text)) return true;
    const char* p = strstr(text.c_str(), "autoupdate=");
    return !p || atoi(p + 11) != 0;
}

char other_slot() { return g_slot == 'a' ? 'b' : 'a'; }

void slot_path(int job, char slot, char* out, size_t size) {
    snprintf(out, size, job == 0 ? "/boot/kernel_%c.elf" : "/boot/initrd_%c.tar", slot);
}
void target_path(int job, char* out, size_t size) { slot_path(job, other_slot(), out, size); }

void finish_manifest() {
    size_t len = 0;
    const uint8_t* body = net::http_body(g_req, &len);
    if (net::http_status(g_req) != 200 || !body || len >= 2048) { fail("The update server did not answer."); return; }
    char json[2048];
    memcpy(json, body, len);
    json[len] = 0;
    char arch[16];
    if (!json_get(json, "version", g_latest, sizeof(g_latest)) || !json_get(json, "url", g_url, sizeof(g_url)) ||
        !json_get(json, "arch", arch, sizeof(arch)) || strcmp(arch, kArch) || !parse_version(g_latest, &g_latest_v)) {
        fail("The update information could not be read.");
        return;
    }
    net::http_free(g_req);
    g_req = nullptr;
    g_step = Step::None;
    bool newer = g_latest_v.compare(current_version()) > 0;
    klog("update: latest %s, %s", g_latest, newer ? "newer than this system" : "up to date");
    if (!newer) { g_state = State::UpToDate; return; }
    char key[32];
    Version cur = current_version();
    snprintf(key, sizeof(key), "patch-%u.%u.%u", cur.main, cur.sub, cur.build);
    g_patch_url[0] = 0;
    if (json_get(json, key, g_patch_url, sizeof(g_patch_url))) klog("update: a patch from this version is available");
    g_state = State::Available;
    if (g_auto_install && strcmp(g_latest, g_env_bad)) install();
}

void finish_download() {
    size_t len = 0;
    const uint8_t* body = package_body(&len);
    if ((!g_local && net::http_status(g_req) != 200) || !body) { fail("The update could not be downloaded."); return; }
    if (len < sizeof(Header) || len > kMaxPackage) { fail("The update package has the wrong size."); return; }
    memcpy(&g_header, body, sizeof(Header));
    const Header& h = g_header;
    if (memcmp(h.magic, "KUPD", 4) || h.format != (g_using_patch ? kPatchFormat : kFormat)) { fail("This is not a KleinOS update package."); return; }
    // nothing else in the package is trusted until this passes
    if (!crypto::ed25519_verify(h.signature, body, kSignedBytes, kUpdatePublicKey)) {
        fail("The update's signature is invalid, so it was rejected.");
        return;
    }
    if (h.arch != kArchId) { fail("The update is for a different processor type."); return; }
    Version v{h.main, h.sub, h.build}, low{h.min_main, h.min_sub, h.min_build}, cur = current_version();
    if (v.compare(cur) <= 0) { fail("The update is not newer than this system."); return; }
    if (g_using_patch && cur.compare(low) != 0) { fail("This patch is for a different version."); return; }
    if (cur.compare(low) < 0) { fail("This update needs a newer system. Install KleinOS from USB."); return; }
    uint64_t total = sizeof(Header) + (uint64_t)h.payload[0].comp_size + h.payload[1].comp_size;
    if (total != len) { fail("The update package is incomplete."); return; }
    g_comp_offset[0] = sizeof(Header);
    g_comp_offset[1] = sizeof(Header) + h.payload[0].comp_size;
    snprintf(g_latest, sizeof(g_latest), "%u.%u.%u", h.main, h.sub, h.build);
    g_state = State::Verifying;
    g_step = Step::HashPackage;
    g_job = 0;
    g_pos = 0;
    g_sha.init();
    g_progress = 0;
}

void step_hash() {
    size_t len = 0;
    const uint8_t* body = package_body(&len);
    const Payload& p = g_header.payload[g_job];
    uint64_t end = timer_ms() + kWorkBudgetMs;
    while (g_pos < p.comp_size && timer_ms() < end) {
        size_t n = kmin<size_t>(kChunk, p.comp_size - g_pos);
        g_sha.update(body + g_comp_offset[g_job] + g_pos, n);
        g_pos += n;
    }
    if (g_pos < p.comp_size) return;
    uint8_t got[32];
    g_sha.final(got);
    if (memcmp(got, p.comp_sha, 32)) { fail("The update was damaged during download."); return; }
    g_pos = 0;
    g_sha.init();
    if (++g_job == 2) { g_job = 0; g_step = Step::Inflate; }
}

// Patch stream: u32 op count, diff length, extra length; ops of { u32 add, u32 extra, i32 seek }; diff bytes; extra bytes.
// Each op takes `add` bytes from the old file (top bit: unchanged, otherwise plus diff bytes), then `extra` new bytes, then moves in the old file.
constexpr uint32_t kPatchExact = 0x80000000u;

bool apply_patch(const uint8_t* base, size_t base_size, const uint8_t* patch, size_t patch_size, uint8_t* out, size_t out_size) {
    uint32_t head[3];
    if (patch_size < sizeof(head)) return false;
    memcpy(head, patch, sizeof(head));
    uint64_t need = sizeof(head) + (uint64_t)head[0] * 12 + head[1] + head[2];
    if (need != patch_size) return false;
    const uint8_t* ops = patch + sizeof(head);
    const uint8_t* diff = ops + (size_t)head[0] * 12;
    const uint8_t* extra = diff + head[1];
    uint64_t np = 0, dp = 0, ep = 0;
    int64_t op = 0;
    for (uint32_t i = 0; i < head[0]; i++) {
        uint32_t add, ext;
        int32_t seek;
        memcpy(&add, ops + (size_t)i * 12, 4);
        memcpy(&ext, ops + (size_t)i * 12 + 4, 4);
        memcpy(&seek, ops + (size_t)i * 12 + 8, 4);
        bool exact = add & kPatchExact;
        add &= ~kPatchExact;
        if (np + add > out_size || op < 0 || (uint64_t)op + add > base_size || (!exact && dp + add > head[1])) return false;
        if (exact) {
            memcpy(out + np, base + op, add);
        } else {
            for (uint32_t j = 0; j < add; j++) out[np + j] = (uint8_t)(base[op + j] + diff[dp + j]);
            dp += add;
        }
        np += add;
        op += add;
        if (np + ext > out_size || ep + ext > head[2]) return false;
        memcpy(out + np, extra + ep, ext);
        np += ext;
        ep += ext;
        op += seek;
    }
    return np == out_size && dp == head[1] && ep == head[2];
}

// Rebuilds one payload from the running slot's copy of the file.
uint8_t* patch_payload(const uint8_t* stream, size_t stream_size, const Payload& p) {
    char path[40];
    slot_path(g_job, g_slot, path, sizeof(path));
    uint8_t* base = nullptr;
    size_t base_size = 0;
    if (!fat32::read_file(path, &base, &base_size)) return nullptr;
    uint8_t* out = (uint8_t*)kmalloc(p.raw_size);
    bool ok = out && apply_patch(base, base_size, stream, stream_size, out, p.raw_size);
    kfree(base);
    if (ok) {
        uint8_t got[32];
        crypto::sha256(out, p.raw_size, got);
        ok = !memcmp(got, p.raw_sha, 32);
    }
    if (!ok) { kfree(out); return nullptr; }
    return out;
}

void step_inflate() {
    size_t len = 0;
    const uint8_t* body = package_body(&len);
    const Payload& p = g_header.payload[g_job];
    int out = 0;
    char* raw = zlib_inflate(body + g_comp_offset[g_job], (int)p.comp_size, g_using_patch ? (int)p.comp_size * 4 : (int)p.raw_size, &out);
    if (g_using_patch) {
        uint8_t* rebuilt = raw ? patch_payload((const uint8_t*)raw, (size_t)out, p) : nullptr;
        kfree(raw);
        if (!rebuilt) { fail("The patch did not produce the expected files."); return; }
        raw = (char*)rebuilt;
    } else if (!raw || (uint32_t)out != p.raw_size) {
        kfree(raw);
        fail("The update could not be unpacked.");
        return;
    }
    g_raw[g_job] = (uint8_t*)raw;
    if (++g_job < 2) return;
    net::http_free(g_req);
    g_req = nullptr;
    g_state = State::Installing;
    g_step = Step::Write;
    g_job = 0;
    g_pos = 0;
}

// Replaces the inactive slot. The running system and its boot entry are untouched.
void step_write() {
    char path[40];
    for (int job = 0; job < 2; job++) {
        target_path(job, path, sizeof(path));
        if (fat32::exists(path)) fat32::remove(path);
    }
    fat32::flush();
    uint64_t need = (uint64_t)g_header.payload[0].raw_size + g_header.payload[1].raw_size;
    if (fat32::free_bytes() < need + (1u << 20)) { fail("There isn't enough free space for this update."); return; }
    for (int job = 0; job < 2; job++) {
        target_path(job, path, sizeof(path));
        if (!fat32::write_file(path, g_raw[job], g_header.payload[job].raw_size, rtc_unix_time())) { fail("Could not write the update to disk."); return; }
        g_progress = 25 + job * 25;
    }
    if (!fat32::flush()) { fail("Could not write the update to disk."); return; }
    g_progress = 50;
    g_step = Step::ReadBack;
}

void step_read_back() {
    char path[40];
    for (int job = 0; job < 2; job++) {
        const Payload& p = g_header.payload[job];
        target_path(job, path, sizeof(path));
        uint8_t* data = nullptr;
        size_t size = 0;
        if (!fat32::read_file(path, &data, &size)) { fail("Could not check the update on disk."); return; }
        uint8_t got[32];
        crypto::sha256(data, size, got);
        bool same = size == p.raw_size && !memcmp(got, p.raw_sha, 32);
        kfree(data);
        if (!same) { fail("The update did not write correctly to disk."); return; }
    }
    g_progress = 90;
    g_step = Step::Commit;
}

// Everything is verified on disk: arm the one-shot boot.
void step_commit() {
    g_env_try = other_slot();
    strlcpy(g_env_pending, g_latest, sizeof(g_env_pending));
    if (!env_write()) { fail("Could not finish the update."); return; }
    free_work();
    g_state = State::Ready;
    g_progress = 100;
    klog("update: %s installed to slot %c, active after restart", g_latest, g_env_try);
    char body[80];
    snprintf(body, sizeof(body), "KleinOS %s is ready. Restart to finish.", g_latest);
    wm::notify("Software Update", body, "settings");
}

void poll() {
    uint64_t now = timer_ms();
    if (!g_confirmed && now >= kConfirmAfterMs && input_last_ms() >= kConfirmAfterMs) {
        // this slot ran and someone used it after the settling time: make it the default.
        // Without that input a black or frozen screen would count as a good update;
        // restarting before then goes back to the previous version.
        g_confirmed = true;
        g_env_slot = g_slot;
        g_env_pending[0] = 0;
        g_env_try = 0;
        env_write();
        klog("update: slot %c confirmed", g_slot);
    }
    switch (g_step) {
        case Step::Manifest:
            if (net::http_state(g_req) == net::HttpState::Failed) fail("Could not reach the update server.");
            else if (net::http_state(g_req) == net::HttpState::Done) finish_manifest();
            break;
        case Step::Download: {
            net::HttpState s = net::http_state(g_req);
            size_t total = net::http_content_length(g_req);
            if (total) g_progress = (int)((uint64_t)net::http_received(g_req) * 100 / total);
            if (s == net::HttpState::Failed) {
                klog("update: download error after %u bytes: %s", (unsigned)net::http_received(g_req), net::http_error(g_req));
                fail("The download was interrupted.");
            }
            else if (s == net::HttpState::Done) finish_download();
            break;
        }
        case Step::HashPackage: step_hash(); break;
        case Step::Inflate: step_inflate(); break;
        case Step::Write: step_write(); break;
        case Step::ReadBack: step_read_back(); break;
        case Step::Commit: step_commit(); break;
        case Step::None:
            if (g_confirmed && now >= g_next_check && (g_state == State::Idle || g_state == State::UpToDate || g_state == State::Failed) &&
                !system_is_dev_build() && net::is_up() && auto_update_enabled()) {
                g_next_check = now + kRecheckMs;
                g_auto_install = true;
                check();
            }
            break;
    }
}

void start_download() {
    kfree(g_local);
    g_local = nullptr;
    g_using_patch = g_patch_url[0] != 0;
    klog("update: downloading the %s", g_using_patch ? "patch" : "full update");
    g_req = net::http_get(g_using_patch ? g_patch_url : g_url);
    if (!g_req) { fail("Could not start the download."); return; }
    g_state = State::Downloading;
    g_step = Step::Download;
    g_progress = 0;
}

}  // namespace

void init() {
    char v[4];
    if (system_boot_mode() != BootMode::Installed || !boot_arg("slot", v, sizeof(v)) || (v[0] != 'a' && v[0] != 'b')) {
        klog("update: not available on this boot");
        return;
    }
    if (!fat32::mounted() || !env_read()) {
        klog("update: this installation has no update slots");
        return;
    }
    g_slot = v[0];
    // try_slot is cleared by GRUB when it boots it, so a pending version with
    // nothing left to try while we run the old slot means that boot failed
    if (g_slot == g_env_slot && !g_env_try && g_env_pending[0]) {
        klog("update: %s did not start, staying on slot %c", g_env_pending, g_slot);
        strlcpy(g_env_bad, g_env_pending, sizeof(g_env_bad));
        g_env_pending[0] = 0;
        env_write();
    }
    g_confirmed = g_slot == g_env_slot;
    g_next_check = kFirstCheckMs;
    g_state = g_env_try ? State::Ready : State::Idle;
    system_add_poll_hook(poll);
    klog("update: running from slot %c%s", g_slot, g_confirmed ? "" : " (on trial)");
}

State state() { return g_state; }

bool check() {
    if (g_state == State::Unsupported || g_state == State::Ready || g_step != Step::None) return false;
    if (!g_confirmed) { strlcpy(g_error, "Finishing the last update. Use the computer for a moment, then try again.", sizeof(g_error)); g_state = State::Failed; return false; }
    if (!net::is_up()) { strlcpy(g_error, "Not connected to the network.", sizeof(g_error)); g_state = State::Failed; return false; }
    char url[128];
    snprintf(url, sizeof(url), "https://github.com/Banaxi-Tech/KleinOS/releases/latest/download/latest-%s.json", kArch);
    g_req = net::http_get(url);
    if (!g_req) { fail("Could not start the update check."); return false; }
    g_state = State::Checking;
    g_step = Step::Manifest;
    return true;
}

bool install_package(uint8_t* data, size_t size) {
    if (g_state == State::Unsupported || g_step != Step::None) return false;
    kfree(g_local);
    g_local = data;
    g_local_size = size;
    g_using_patch = false;
    g_error[0] = 0;
    g_progress = 0;
    g_state = State::Downloading;
    finish_download();
    return g_state != State::Failed;
}

bool install() {
    if (g_state != State::Available || g_step != Step::None) return false;
    start_download();
    return g_step == Step::Download;
}

const char* latest_version() { return g_latest; }
const char* error() { return g_error; }
int progress() { return g_progress; }
char active_slot() { return g_slot; }

}  // namespace update
