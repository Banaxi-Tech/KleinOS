#include "fs/fat32.h"
#include "drivers/ata.h"
#include "drivers/rtc.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"

namespace fat32 {
namespace {

constexpr uint32_t kEoc = 0x0FFFFFFF;
constexpr uint8_t kAttrDir = 0x10, kAttrVolume = 0x08, kAttrLfn = 0x0F;
constexpr int kMaxName = 255;

int g_drive = -1;
uint64_t g_fat_lba, g_data_lba, g_fsinfo_lba;
uint32_t g_spc, g_bpc, g_fat_sectors, g_root, g_clusters, g_fats, g_hint = 2;
uint32_t* g_fat;
uint8_t* g_fat_dirty;
uint8_t* g_tail;           // one cluster, for partial reads and writes
bool g_fsinfo_cleared;

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr32(uint8_t* p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

uint64_t cluster_lba(uint32_t c) { return g_data_lba + (uint64_t)(c - 2) * g_spc; }
uint32_t fat_get(uint32_t c) { return g_fat[c] & 0x0FFFFFFF; }
void fat_set(uint32_t c, uint32_t v) {
    g_fat[c] = (g_fat[c] & 0xF0000000) | (v & 0x0FFFFFFF);
    g_fat_dirty[c / 128] = 1;
}
bool valid(uint32_t c) { return c >= 2 && c < g_clusters + 2; }
bool chain_end(uint32_t v) { return v >= 0x0FFFFFF8 || !valid(v); }

uint32_t alloc_cluster() {
    for (uint32_t i = 0; i < g_clusters; i++) {
        uint32_t c = g_hint + i;
        if (c >= g_clusters + 2) c -= g_clusters;
        if (fat_get(c) == 0) {
            fat_set(c, kEoc);
            g_hint = c + 1 < g_clusters + 2 ? c + 1 : 2;
            return c;
        }
    }
    return 0;
}

void free_chain(uint32_t c) {
    uint32_t guard = g_clusters;
    while (valid(c) && guard--) {
        uint32_t n = fat_get(c);
        fat_set(c, 0);
        c = n;
    }
}

uint32_t alloc_chain(uint32_t n) {
    uint32_t first = 0, last = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = alloc_cluster();
        if (!c) { free_chain(first); return 0; }
        if (last) fat_set(last, c);
        else first = c;
        last = c;
    }
    return first;
}

uint32_t clusters_for(size_t size) { return (uint32_t)((size + g_bpc - 1) / g_bpc); }

uint32_t free_clusters() {
    uint32_t n = 0;
    for (uint32_t c = 2; c < g_clusters + 2; c++)
        if (fat_get(c) == 0) n++;
    return n;
}

// Next run of consecutive clusters from c, at most `max`. *after = cluster following the run.
uint32_t run_length(uint32_t c, uint32_t max, uint32_t* after) {
    uint32_t n = 1, cur = c;
    for (;;) {
        uint32_t next = fat_get(cur);
        if (n >= max || next != cur + 1 || chain_end(next)) { *after = next; return n; }
        cur = next;
        n++;
    }
}

bool write_chain(uint32_t c, const uint8_t* data, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        if (!valid(c)) return false;
        uint32_t whole = (uint32_t)((size - pos) / g_bpc);
        if (!whole) {
            memset(g_tail, 0, g_bpc);
            memcpy(g_tail, data + pos, size - pos);
            return ata_write(g_drive, cluster_lba(c), g_spc, g_tail);
        }
        uint32_t after;
        uint32_t n = run_length(c, whole, &after);
        if (!ata_write(g_drive, cluster_lba(c), n * g_spc, data + pos)) return false;
        pos += (size_t)n * g_bpc;
        c = after;
    }
    return true;
}

bool read_chain(uint32_t c, uint8_t* data, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        if (!valid(c)) return false;
        uint32_t whole = (uint32_t)((size - pos) / g_bpc);
        if (!whole) {
            if (!ata_read(g_drive, cluster_lba(c), g_spc, g_tail)) return false;
            memcpy(data + pos, g_tail, size - pos);
            return true;
        }
        uint32_t after;
        uint32_t n = run_length(c, whole, &after);
        if (!ata_read(g_drive, cluster_lba(c), n * g_spc, data + pos)) return false;
        pos += (size_t)n * g_bpc;
        c = after;
    }
    return true;
}

// ---- directories are loaded whole, edited in memory and written back
struct Dir {
    uint8_t* data = nullptr;
    size_t bytes = 0;
    Vector<uint32_t> clusters;
    ~Dir() { kfree(data); }
};

bool dir_load(uint32_t first, Dir& d) {
    uint32_t c = first, guard = 65536;
    while (valid(c) && guard--) {
        d.clusters.push(c);
        c = fat_get(c);
    }
    if (d.clusters.empty()) return false;
    d.bytes = d.clusters.size() * (size_t)g_bpc;
    d.data = (uint8_t*)kmalloc(d.bytes);
    if (!d.data) return false;
    for (size_t i = 0; i < d.clusters.size(); i++)
        if (!ata_read(g_drive, cluster_lba(d.clusters[i]), g_spc, d.data + i * g_bpc)) return false;
    return true;
}

bool dir_save(const Dir& d) {
    for (size_t i = 0; i < d.clusters.size(); i++)
        if (!ata_write(g_drive, cluster_lba(d.clusters[i]), g_spc, d.data + i * g_bpc)) return false;
    return true;
}

uint8_t short_checksum(const uint8_t* e) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

void short_to_text(const uint8_t* e, char* out) {
    int n = 0;
    bool lower_base = e[12] & 0x08, lower_ext = e[12] & 0x10;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char ch = (char)e[i];
        out[n++] = lower_base && ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char ch = (char)e[i];
            out[n++] = lower_ext && ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
        }
    }
    out[n] = 0;
}

int utf16_to_utf8(const uint16_t* in, int count, char* out, int max) {
    int n = 0;
    for (int i = 0; i < count && in[i]; i++) {
        uint32_t u = in[i];
        if (u >= 0xD800 && u <= 0xDFFF) u = '?';
        if (u < 0x80 && n + 1 < max) out[n++] = (char)u;
        else if (u < 0x800 && n + 2 < max) { out[n++] = (char)(0xC0 | (u >> 6)); out[n++] = (char)(0x80 | (u & 63)); }
        else if (u >= 0x800 && n + 3 < max) {
            out[n++] = (char)(0xE0 | (u >> 12));
            out[n++] = (char)(0x80 | ((u >> 6) & 63));
            out[n++] = (char)(0x80 | (u & 63));
        }
    }
    out[n] = 0;
    return n;
}

int utf8_to_utf16(const char* s, uint16_t* out, int max) {
    int n = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p && n < max) {
        uint32_t u = *p;
        int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
        if (extra) u &= 0x3F >> extra;
        p++;
        for (int i = 0; i < extra && *p; i++) u = (u << 6) | (*p++ & 63);
        out[n++] = u > 0xFFFF ? '?' : (uint16_t)u;
    }
    return n;
}

bool same_name(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

// f(name, first_slot_offset, short_entry_offset); returning true stops the walk.
template <typename F>
void each_entry(Dir& d, F f) {
    uint16_t lfn[kMaxName + 13];
    bool have = false;
    uint8_t sum = 0;
    size_t start = 0;
    for (size_t off = 0; off + 32 <= d.bytes; off += 32) {
        const uint8_t* e = d.data + off;
        if (e[0] == 0) return;
        if (e[0] == 0xE5) { have = false; continue; }
        if (e[11] == kAttrLfn) {
            int ord = e[0] & 0x3F;
            if (e[0] & 0x40) {
                memset(lfn, 0, sizeof(lfn));
                have = true;
                sum = e[13];
                start = off;
            }
            if (ord < 1 || (ord - 1) * 13 + 13 > kMaxName + 13) { have = false; continue; }
            uint16_t* u = lfn + (ord - 1) * 13;
            static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int i = 0; i < 13; i++) u[i] = rd16(e + pos[i]);
            continue;
        }
        if (e[11] & kAttrVolume) { have = false; continue; }
        char name[780];
        bool use_long = have && short_checksum(e) == sum;
        if (use_long) {
            for (int i = 0; i < kMaxName + 13; i++)
                if (lfn[i] == 0xFFFF) lfn[i] = 0;
            utf16_to_utf8(lfn, kMaxName + 13, name, sizeof(name));
        } else {
            short_to_text(e, name);
        }
        size_t first = use_long ? start : off;
        have = false;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (f(name, first, off)) return;
    }
}

bool dir_find(Dir& d, const char* name, size_t* first, size_t* entry) {
    bool found = false;
    each_entry(d, [&](const char* n, size_t a, size_t b) {
        if (!same_name(n, name)) return false;
        *first = a;
        *entry = b;
        found = true;
        return true;
    });
    return found;
}

uint32_t entry_cluster(const uint8_t* e) { return ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26); }

uint64_t decode_time(const uint8_t* e) {
    uint32_t date = rd16(e + 24), tm = rd16(e + 22);
    if (!date) return 0;
    int y = 1980 + (int)(date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1) return 0;
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint64_t days = (uint64_t)(era * 146097 + doe - 719468);
    return days * 86400 + (tm >> 11) * 3600 + ((tm >> 5) & 63) * 60 + (tm & 31) * 2;
}

void set_entry(uint8_t* e, uint32_t cluster, uint32_t size, uint64_t mtime) {
    wr16(e + 20, cluster >> 16);
    wr16(e + 26, cluster);
    wr32(e + 28, size);
    DateTime t = unix_to_datetime(mtime);
    uint32_t date = t.year >= 1980 ? ((uint32_t)(t.year - 1980) << 9) | (t.month << 5) | t.day : (1u << 5) | 1;
    uint32_t tm = (t.hour << 11) | (t.minute << 5) | (t.second / 2);
    wr16(e + 22, tm);
    wr16(e + 24, date);
}

bool short_taken(Dir& d, const uint8_t* s) {
    for (size_t off = 0; off + 32 <= d.bytes; off += 32) {
        const uint8_t* e = d.data + off;
        if (e[0] == 0) return false;
        if (e[0] != 0xE5 && e[11] != kAttrLfn && !memcmp(e, s, 11)) return true;
    }
    return false;
}

bool dir_grow(Dir& d) {
    uint32_t c = alloc_cluster();
    if (!c) return false;
    fat_set(d.clusters[d.clusters.size() - 1], c);
    d.clusters.push(c);
    uint8_t* bigger = (uint8_t*)krealloc(d.data, d.bytes + g_bpc);
    if (!bigger) return false;
    d.data = bigger;
    memset(d.data + d.bytes, 0, g_bpc);
    d.bytes += g_bpc;
    return true;
}

bool dir_add(Dir& d, const char* name, uint8_t attr, uint32_t cluster, uint32_t size, uint64_t mtime) {
    uint16_t units[kMaxName + 1];
    int len = utf8_to_utf16(name, units, kMaxName);
    if (!len) return false;
    int lfn_slots = (len + 12) / 13;
    size_t need = (size_t)lfn_slots + 1;
    size_t at = (size_t)-1;
    for (;;) {
        size_t run = 0;
        for (size_t off = 0; off + 32 <= d.bytes; off += 32) {
            uint8_t b = d.data[off];
            if (b == 0 || b == 0xE5) {
                if (++run == need) { at = off + 32 - need * 32; break; }
            } else {
                run = 0;
            }
        }
        if (at != (size_t)-1) break;
        if (!dir_grow(d)) return false;
    }
    uint8_t sn[11];
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    do {
        static const char kHex[] = "0123456789ABCDEF";
        memset(sn, ' ', 11);
        sn[0] = 'K';
        for (int i = 0; i < 7; i++) sn[1 + i] = (uint8_t)kHex[(h >> (i * 4)) & 15];
        h++;
    } while (short_taken(d, sn));
    uint8_t sum = short_checksum(sn);
    static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int i = 0; i < lfn_slots; i++) {
        int ord = lfn_slots - i;
        uint8_t* e = d.data + at + (size_t)i * 32;
        memset(e, 0, 32);
        e[0] = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        e[11] = kAttrLfn;
        e[13] = sum;
        for (int k = 0; k < 13; k++) {
            int u = (ord - 1) * 13 + k;
            uint32_t v = u < len ? units[u] : u == len ? 0 : 0xFFFF;
            wr16(e + pos[k], v);
        }
    }
    uint8_t* e = d.data + at + (size_t)lfn_slots * 32;
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    e[11] = attr;
    set_entry(e, cluster, size, mtime);
    return true;
}

void dir_erase(Dir& d, size_t first, size_t entry) {
    for (size_t off = first; off <= entry; off += 32) d.data[off] = 0xE5;
}

// Path helpers. Component `index` of an absolute path into out; false past the end.
bool next_component(const char*& p, char* out, size_t size) {
    while (*p == '/') p++;
    if (!*p) return false;
    size_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 < size) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return true;
}

bool walk_dir(const char* path, bool create, uint32_t* out, uint64_t mtime = 0) {
    uint32_t c = g_root;
    char part[780];
    const char* p = path;
    while (next_component(p, part, sizeof(part))) {
        Dir d;
        if (!dir_load(c, d)) return false;
        size_t first, entry;
        if (dir_find(d, part, &first, &entry)) {
            const uint8_t* e = d.data + entry;
            if (!(e[11] & kAttrDir)) return false;
            c = entry_cluster(e);
            continue;
        }
        if (!create) return false;
        uint32_t nc = alloc_cluster();
        if (!nc) return false;
        memset(g_tail, 0, g_bpc);
        memset(g_tail, ' ', 11);
        g_tail[0] = '.';
        g_tail[11] = kAttrDir;
        set_entry(g_tail, nc, 0, mtime);
        memset(g_tail + 32, ' ', 11);
        g_tail[32] = g_tail[33] = '.';
        g_tail[32 + 11] = kAttrDir;
        set_entry(g_tail + 32, c == g_root ? 0 : c, 0, mtime);
        if (!ata_write(g_drive, cluster_lba(nc), g_spc, g_tail)) return false;
        if (!dir_add(d, part, kAttrDir, nc, 0, mtime) || !dir_save(d)) return false;
        c = nc;
    }
    *out = c;
    return true;
}

// Split "/a/b/c" into parent "/a/b" and leaf "c".
bool split(const char* path, char* parent, size_t psize, const char** leaf) {
    const char* slash = strrchr(path, '/');
    if (!slash || !slash[1]) return false;
    size_t n = (size_t)(slash - path);
    if (n >= psize) return false;
    memcpy(parent, path, n);
    parent[n] = 0;
    *leaf = slash + 1;
    return true;
}

bool lookup(const char* path, Dir& d, size_t* first, size_t* entry) {
    char parent[1024];
    const char* leaf;
    uint32_t c;
    if (!split(path, parent, sizeof(parent), &leaf) || !walk_dir(parent, false, &c) || !dir_load(c, d)) return false;
    return dir_find(d, leaf, first, entry);
}

void remove_tree(uint32_t dir) {
    Dir d;
    if (dir_load(dir, d)) {
        each_entry(d, [&](const char*, size_t, size_t entry) {
            const uint8_t* e = d.data + entry;
            uint32_t c = entry_cluster(e);
            if (e[11] & kAttrDir) remove_tree(c);
            else free_chain(c);
            return false;
        });
    }
    free_chain(dir);
}

void clear_fsinfo() {
    if (g_fsinfo_cleared || !g_fsinfo_lba) return;
    g_fsinfo_cleared = true;
    uint8_t sec[512];
    if (!ata_read(g_drive, g_fsinfo_lba, 1, sec)) return;
    wr32(sec + 488, 0xFFFFFFFF);    // free count and next-free hint: unknown
    wr32(sec + 492, 0xFFFFFFFF);
    ata_write(g_drive, g_fsinfo_lba, 1, sec);
}

}  // namespace

bool mount(int drv) {
    kfree(g_fat); kfree(g_fat_dirty); kfree(g_tail);
    g_fat = nullptr; g_fat_dirty = nullptr; g_tail = nullptr;
    g_drive = -1;
    g_fsinfo_cleared = false;
    uint8_t sec[512];
    if (!ata_read(drv, 0, 1, sec) || sec[510] != 0x55 || sec[511] != 0xAA) return false;
    const uint8_t* part_entry = sec + 446;
    if (part_entry[4] != 0x0B && part_entry[4] != 0x0C) return false;
    uint64_t part = rd32(part_entry + 8);
    if (!ata_read(drv, part, 1, sec) || rd16(sec + 11) != 512 || rd16(sec + 22) != 0) return false;
    g_spc = sec[13];
    g_fats = sec[16];
    g_fat_sectors = rd32(sec + 36);
    g_root = rd32(sec + 44);
    uint32_t reserved = rd16(sec + 14), total = rd32(sec + 32), info = rd16(sec + 48);
    if (!g_spc || !g_fats || !g_fat_sectors || !total) return false;
    g_bpc = g_spc * 512;
    g_fat_lba = part + reserved;
    g_data_lba = g_fat_lba + (uint64_t)g_fats * g_fat_sectors;
    g_fsinfo_lba = info && info != 0xFFFF ? part + info : 0;
    g_clusters = (total - reserved - g_fats * g_fat_sectors) / g_spc;
    uint32_t in_table = g_fat_sectors * 128 - 2;
    if (g_clusters > in_table) g_clusters = in_table;
    if (g_clusters < 65525) return false;
    g_fat = (uint32_t*)kmalloc(g_fat_sectors * 512u);
    g_fat_dirty = (uint8_t*)kcalloc(g_fat_sectors, 1);
    g_tail = (uint8_t*)kmalloc(g_bpc);
    if (!g_fat || !g_fat_dirty || !g_tail || !ata_read(drv, g_fat_lba, g_fat_sectors, g_fat)) return false;
    g_hint = 2;
    g_drive = drv;
    klog("fat32: mounted drive %d, %u clusters of %u bytes", drv, g_clusters, g_bpc);
    return true;
}

bool mounted() { return g_drive >= 0; }
int drive() { return g_drive; }
uint64_t free_bytes() { return mounted() ? (uint64_t)free_clusters() * g_bpc : 0; }

bool exists(const char* path) {
    if (!mounted()) return false;
    Dir d;
    size_t a, b;
    return lookup(path, d, &a, &b);
}

bool list(const char* path, Vector<DirEntry>& out) {
    out.clear();
    uint32_t c;
    if (!mounted() || !walk_dir(path, false, &c)) return false;
    Dir d;
    if (!dir_load(c, d)) return false;
    each_entry(d, [&](const char* name, size_t, size_t entry) {
        const uint8_t* e = d.data + entry;
        out.push(DirEntry{String(name), (e[11] & kAttrDir) != 0, rd32(e + 28), decode_time(e)});
        return false;
    });
    return true;
}

bool read_file(const char* path, uint8_t** data, size_t* size) {
    if (!mounted()) return false;
    Dir d;
    size_t a, b;
    if (!lookup(path, d, &a, &b)) return false;
    const uint8_t* e = d.data + b;
    if (e[11] & kAttrDir) return false;
    uint32_t n = rd32(e + 28);
    uint8_t* buf = (uint8_t*)kmalloc(n ? n : 1);
    if (!buf) return false;
    if (n && !read_chain(entry_cluster(e), buf, n)) { kfree(buf); return false; }
    *data = buf;
    *size = n;
    return true;
}

bool write_file(const char* path, const void* data, size_t size, uint64_t mtime) {
    char parent[1024];
    const char* leaf;
    uint32_t pc;
    if (!mounted() || !split(path, parent, sizeof(parent), &leaf) || !walk_dir(parent, true, &pc, mtime)) return false;
    clear_fsinfo();
    Dir d;
    if (!dir_load(pc, d)) return false;
    size_t first, entry;
    bool existed = dir_find(d, leaf, &first, &entry);
    uint32_t old = 0;
    if (existed) {
        const uint8_t* e = d.data + entry;
        if (e[11] & kAttrDir) return false;
        old = entry_cluster(e);
        if (rd32(e + 28) == size && size) {
            if (!write_chain(old, (const uint8_t*)data, size)) return false;
            set_entry(d.data + entry, old, (uint32_t)size, mtime);
            return dir_save(d);
        }
    }
    uint32_t count = clusters_for(size);
    // the new copy goes down first; only tight disks free the old one early
    if (old && free_clusters() < count) {
        free_chain(old);
        old = 0;
    }
    uint32_t chain = count ? alloc_chain(count) : 0;
    if (count && !chain) return false;
    if (!write_chain(chain, (const uint8_t*)data, size)) { free_chain(chain); return false; }
    if (existed) {
        set_entry(d.data + entry, chain, (uint32_t)size, mtime);
    } else if (!dir_add(d, leaf, 0x20, chain, (uint32_t)size, mtime)) {
        free_chain(chain);
        return false;
    }
    if (!dir_save(d)) return false;
    if (old) free_chain(old);
    return true;
}

bool make_dirs(const char* path) {
    uint32_t c;
    if (!mounted()) return false;
    clear_fsinfo();
    return walk_dir(path, true, &c, rtc_unix_time());
}

bool remove(const char* path) {
    if (!mounted()) return false;
    clear_fsinfo();
    Dir d;
    size_t first, entry;
    if (!lookup(path, d, &first, &entry)) return false;
    const uint8_t* e = d.data + entry;
    uint32_t c = entry_cluster(e);
    if (e[11] & kAttrDir) remove_tree(c);
    else free_chain(c);
    dir_erase(d, first, entry);
    return dir_save(d);
}

bool flush() {
    if (!mounted()) return false;
    for (uint32_t s = 0; s < g_fat_sectors; s++) {
        if (!g_fat_dirty[s]) continue;
        for (uint32_t f = 0; f < g_fats; f++)
            if (!ata_write(g_drive, g_fat_lba + (uint64_t)f * g_fat_sectors + s, 1, (uint8_t*)g_fat + s * 512u)) return false;
        g_fat_dirty[s] = 0;
    }
    return ata_flush(g_drive);
}

}  // namespace fat32
