#include "sys/sysconfig.h"
#include "drivers/ata.h"
#include "drivers/rtc.h"
#include "drivers/usb_storage.h"
#include "boot/multiboot2.h"
#include "fs/fat32.h"
#include "ui/theme.h"
#include "fs/vfs.h"
#include "net/crypto.h"
#include "sys/system.h"
#include "arch/interrupts.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "lib/log.h"

static const char kConfigPath[] = "/KleinOS/config.bin";
static SysConfig g_cfg;
static int g_drive = -1;
static bool g_dirty;
static uint64_t g_dirty_since;
static bool g_userdata_loaded;
static Vector<String> g_not_loaded;      // too big for RAM: on disk, kept out of /Users

static bool read_config() {
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!fat32::read_file(kConfigPath, &data, &size)) return false;
    bool ok = size == sizeof(SysConfig) && memcmp(data, "KLEINCFG", 8) == 0;
    if (ok) memcpy(&g_cfg, data, sizeof(SysConfig));
    kfree(data);
    return ok;
}

static bool usb_install() {
    char v[8];
    return boot_arg("storage", v, sizeof(v)) && !strcmp(v, "usb");
}

// A freshly flashed USB system has no Setup to create its configuration.
static bool create_default_config() {
    SysConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.install_time = rtc_unix_time();
    strlcpy(cfg.computer_name, "KleinOS-PC", sizeof(cfg.computer_name));
    strlcpy(cfg.language, "en-US", sizeof(cfg.language));
    cfg.accent = kAccentBlue;
    return sysconfig_init_on_drive(fat32::drive(), cfg);
}

bool sysconfig_load() {
    if (usb_install()) {
        if (usb_storage_ready() && fat32::mount(kUsbDrive)) {
            if (read_config() || create_default_config()) {
                g_drive = kUsbDrive;
                klog("sysconfig: running from USB (setup_complete=%u, users=%u)", g_cfg.setup_complete, g_cfg.user_count);
                return true;
            }
        }
        klog("sysconfig: the USB system could not be opened");
        return false;
    }
    for (int i = 0; i < ata_drive_count(); i++) {
        if (!fat32::mount(i) || !read_config()) continue;
        g_drive = i;
        klog("sysconfig: install found on ata%d (setup_complete=%u, users=%u)", i, g_cfg.setup_complete, g_cfg.user_count);
        return true;
    }
    klog("sysconfig: no installed system found");
    return false;
}

bool sysconfig_present() { return g_drive >= 0; }
SysConfig& sysconfig() { return g_cfg; }
int sysconfig_drive() { return g_drive; }

bool sysconfig_save() {
    if (g_drive < 0) return false;
    return fat32::write_file(kConfigPath, &g_cfg, sizeof(SysConfig), rtc_unix_time()) && fat32::flush();
}

bool sysconfig_init_on_drive(int drive, const SysConfig& cfg) {
    if (fat32::drive() != drive && !fat32::mount(drive)) return false;
    g_cfg = cfg;
    memcpy(g_cfg.magic, "KLEINCFG", 8);
    g_cfg.version = 2;
    g_drive = drive;
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

PasswordChangeResult sysconfig_change_password(const char* login, const char* current_password,
                                              const char* new_password) {
    if (!sysconfig_present() || !login) return PasswordChangeResult::Unavailable;
    for (uint32_t i = 0; i < g_cfg.user_count && i < 4; i++) {
        UserAccount& u = g_cfg.users[i];
        if (strcmp(u.login, login)) continue;
        if (!sysconfig_check_password(u, current_password)) return PasswordChangeResult::IncorrectPassword;
        char previous[65];
        memcpy(previous, u.password_hash, sizeof(previous));
        memset(u.password_hash, 0, sizeof(u.password_hash));
        sysconfig_hash_password(u.login, new_password, u.password_hash);
        bool saved = sysconfig_save();
        if (!saved) memcpy(u.password_hash, previous, sizeof(previous));
        memset(previous, 0, sizeof(previous));
        return saved ? PasswordChangeResult::Saved : PasswordChangeResult::SaveFailed;
    }
    return PasswordChangeResult::Unavailable;
}

// ---------------------------------------------------------------- user data
// /Users lives on disk as ordinary files and in RAM while running.
static constexpr size_t kMaxFileInRam = 32u << 20;
static constexpr size_t kMaxTotalInRam = 256u << 20;
static size_t g_loaded_bytes;

// macOS and Windows droppings: never shown, never deleted.
static bool foreign_clutter(const char* n) {
    return !strncmp(n, "._", 2) || !strcmp(n, ".DS_Store") || !strcmp(n, ".Trashes") || !strcmp(n, ".fseventsd") ||
           !strcmp(n, ".Spotlight-V100") || !strcmp(n, "System Volume Information");
}

static void load_tree(const String& path) {
    Vector<fat32::DirEntry> items;
    if (!fat32::list(path.c_str(), items)) return;
    vfs::mkdir(path.c_str());
    for (fat32::DirEntry& e : items) {
        if (foreign_clutter(e.name.c_str())) continue;
        String child = path + "/" + e.name;
        if (e.dir) { load_tree(child); continue; }
        if (e.size > kMaxFileInRam || g_loaded_bytes + e.size > kMaxTotalInRam) {
            klog("userdata: %s is too large to load, leaving it on disk", child.c_str());
            g_not_loaded.push(child.lower());
            continue;
        }
        uint8_t* data = nullptr;
        size_t size = 0;
        if (!fat32::read_file(child.c_str(), &data, &size)) { g_not_loaded.push(child.lower()); continue; }
        vfs::write_file(child.c_str(), data, size);
        kfree(data);
        g_loaded_bytes += size;
        if (vfs::Node* n = vfs::lookup(child.c_str())) {
            n->mtime = e.mtime;
            n->unsaved = false;
        }
    }
}

void userdata_load() {
    if (g_drive < 0) return;
    g_loaded_bytes = 0;
    load_tree(String("/Users"));
    g_userdata_loaded = true;
    klog("userdata: restored %zu bytes of user files", g_loaded_bytes);
}

// Characters FAT can't store become '_', and trailing dots and spaces are dropped.
static String disk_name(const String& name) {
    String out = name;
    char* s = (char*)out.c_str();
    for (char* p = s; *p; p++)
        if ((uint8_t)*p < 32 || strchr("\\/:*?\"<>|", *p)) *p = '_';
    size_t n = out.size();
    while (n > 1 && (s[n - 1] == '.' || s[n - 1] == ' ')) n--;
    return out.substr(0, n);
}

static bool was_left_on_disk(const String& path) {
    String low = path.lower();
    for (String& s : g_not_loaded)
        if (s == low) return true;
    return false;
}

static bool sync_tree(vfs::Node* dir, const String& path) {
    Vector<fat32::DirEntry> disk;
    if (!fat32::list(path.c_str(), disk)) {
        if (!fat32::make_dirs(path.c_str())) return false;
    }
    bool ok = true;
    Vector<bool> kept;
    for (size_t i = 0; i < disk.size(); i++) kept.push(false);
    for (vfs::Node* c : dir->children) {
        String name = disk_name(c->name);
        String child = path + "/" + name;
        String low = name.lower();
        int found = -1;
        for (size_t i = 0; i < disk.size(); i++)
            if (disk[i].name.lower() == low) { found = (int)i; kept[i] = true; }
        if (c->is_dir) {
            if (found >= 0 && !disk[found].dir) ok = fat32::remove(child.c_str()) && ok;
            ok = sync_tree(c, child) && ok;
            continue;
        }
        if (found >= 0 && disk[found].dir) { ok = fat32::remove(child.c_str()) && ok; found = -1; }
        if (found < 0 || c->unsaved || disk[found].size != c->size) {
            if (fat32::write_file(child.c_str(), c->data, c->size, c->mtime)) c->unsaved = false;
            else ok = false;
        }
    }
    for (size_t i = 0; i < disk.size(); i++) {
        if (kept[i] || foreign_clutter(disk[i].name.c_str())) continue;
        String child = path + "/" + disk[i].name;
        if (!was_left_on_disk(child)) ok = fat32::remove(child.c_str()) && ok;
    }
    return ok;
}

bool userdata_save_now() {
    if (g_drive < 0 || !g_userdata_loaded) return false;
    vfs::Node* users = vfs::lookup("/Users");
    if (!users) return false;
    bool ok = sync_tree(users, String("/Users"));
    ok = fat32::flush() && ok;
    if (!ok) klog("userdata: some files could not be saved");
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
