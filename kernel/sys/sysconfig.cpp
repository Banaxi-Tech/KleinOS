#include "sys/sysconfig.h"
#include "drivers/ata.h"
#include "fs/vfs.h"
#include "net/crypto.h"
#include "sys/system.h"
#include "arch/interrupts.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/log.h"

static SysConfig g_cfg;
static int g_drive = -1;
static uint64_t g_data_lba, g_data_sectors;
static bool g_dirty;
static uint64_t g_dirty_since;

static_assert(sizeof(SysConfig) <= 4096, "config must fit in 8 sectors");

bool sysconfig_find_data_partition(int drive, uint64_t* lba, uint64_t* sectors) {
    AtaDrive* d = ata_drive(drive);
    if (!d || !d->present || d->atapi) return false;
    uint8_t mbr[512];
    if (!ata_read(drive, 0, 1, mbr)) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = mbr + 446 + i * 16;
        if (e[4] == 0x7F) {
            *lba = (uint32_t)(e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24));
            *sectors = (uint32_t)(e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24));
            return *sectors > 16;
        }
    }
    return false;
}

static uint32_t checksum(const uint8_t* d, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

bool sysconfig_load() {
    for (int i = 0; i < ata_drive_count(); i++) {
        uint64_t lba, n;
        if (!sysconfig_find_data_partition(i, &lba, &n)) continue;
        uint8_t* buf = (uint8_t*)kmalloc(4096);
        bool ok = ata_read(i, lba, 8, buf);
        if (ok && memcmp(buf, "KLEINCFG", 8) == 0) {
            memcpy(&g_cfg, buf, sizeof(SysConfig));
            g_drive = i;
            g_data_lba = lba;
            g_data_sectors = n;
            kfree(buf);
            klog("sysconfig: install found on ata%d (setup_complete=%u, users=%u)", i, g_cfg.setup_complete, g_cfg.user_count);
            return true;
        }
        kfree(buf);
    }
    klog("sysconfig: no installed system found");
    return false;
}

bool sysconfig_present() { return g_drive >= 0; }
SysConfig& sysconfig() { return g_cfg; }
int sysconfig_drive() { return g_drive; }

bool sysconfig_save() {
    if (g_drive < 0) return false;
    uint8_t* buf = (uint8_t*)kcalloc(4096, 1);
    memcpy(buf, &g_cfg, sizeof(SysConfig));
    bool ok = ata_write(g_drive, g_data_lba, 8, buf);
    kfree(buf);
    return ok;
}

bool sysconfig_init_on_drive(int drive, const SysConfig& cfg) {
    uint64_t lba, n;
    if (!sysconfig_find_data_partition(drive, &lba, &n)) return false;
    g_cfg = cfg;
    memcpy(g_cfg.magic, "KLEINCFG", 8);
    g_cfg.version = 1;
    g_drive = drive;
    g_data_lba = lba;
    g_data_sectors = n;
    // clear the user data area header
    uint8_t* zero = (uint8_t*)kcalloc(4096, 1);
    ata_write(drive, lba + 8, 8, zero);
    kfree(zero);
    return sysconfig_save();
}

void sysconfig_hash_password(const char* login, const char* password, char out[65]) {
    if (!password || !*password) { out[0] = 0; return; }
    crypto::Sha256 s;
    s.init();
    s.update("KleinOS-salt:", 13);
    s.update(login, strlen(login));
    s.update(":", 1);
    s.update(password, strlen(password));
    uint8_t h[32];
    s.final(h);
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = hx[h[i] >> 4]; out[i * 2 + 1] = hx[h[i] & 15]; }
    out[64] = 0;
}

bool sysconfig_check_password(const UserAccount& u, const char* password) {
    if (!u.password_hash[0]) return true;
    char h[65];
    sysconfig_hash_password(u.login, password, h);
    return strcmp(h, u.password_hash) == 0;
}

// ---------------------------------------------------------------- user data
void userdata_load() {
    if (g_drive < 0 || !g_cfg.data_bytes) return;
    uint32_t bytes = g_cfg.data_bytes;
    uint64_t max = (g_data_sectors - 8) * 512;
    if (bytes > max) { klog("userdata: bad size %u", bytes); return; }
    uint32_t sectors = (bytes + 511) / 512;
    uint8_t* buf = (uint8_t*)kmalloc(sectors * 512);
    if (ata_read(g_drive, g_data_lba + 8, sectors, buf) && checksum(buf, bytes) == g_cfg.data_checksum) {
        vfs::deserialize("/Users", buf, bytes);
        klog("userdata: restored %u bytes of user files", bytes);
    } else {
        klog("userdata: checksum mismatch, ignoring saved files");
    }
    kfree(buf);
}

bool userdata_save_now() {
    if (g_drive < 0) return false;
    Vector<uint8_t> blob;
    if (!vfs::serialize("/Users", blob)) return false;
    uint64_t max = (g_data_sectors - 8) * 512;
    if (blob.size() > max) { klog("userdata: %zu bytes exceed the data partition", blob.size()); return false; }
    uint32_t exact = (uint32_t)blob.size();
    uint32_t sum = checksum(blob.data(), exact);
    uint32_t sectors = (exact + 511) / 512;
    blob.resize(sectors * 512);
    bool ok = ata_write(g_drive, g_data_lba + 8, sectors, blob.data());
    if (ok) {
        g_cfg.data_bytes = exact;
        g_cfg.data_checksum = sum;
        ok = sysconfig_save();
    }
    g_dirty = false;
    return ok;
}

static void on_change() {
    if (!g_dirty) g_dirty_since = timer_ms();
    g_dirty = true;
}

static void autosave_hook() {
    if (g_dirty && timer_ms() - g_dirty_since > 2000) {
        if (userdata_save_now()) klog("userdata: saved");
    }
}

void userdata_enable_autosave() {
    if (g_drive < 0) return;
    vfs::set_change_hook(on_change);
    system_add_poll_hook(autosave_hook);
}
