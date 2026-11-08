#include "sys/dl.h"
#include "fs/vfs.h"
#include "lib/log.h"
#include "lib/heap.h"
#include "ui/app.h"

namespace {

#ifdef __i386__
constexpr int kElfClass = 1, kElfMachine = 3;
struct Elf_Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint32_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Elf_Phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct Elf_Dyn {
    int32_t tag;
    uint32_t val;
};
struct Elf_Sym {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t shndx;
};
struct Elf_Reloc {   // Elf32_Rel: the addend lives at the target
    uint32_t offset, info;
    uint32_t sym() const { return info >> 8; }
    uint32_t kind() const { return info & 0xFF; }
    uintptr_t addend(const uintptr_t* where) const { return *where; }
};
enum { DT_RELOCS = 17, DT_RELOCS_SZ = 18 };
#else
constexpr int kElfClass = 2, kElfMachine = 62;
struct Elf_Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Elf_Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Elf_Dyn {
    int64_t tag;
    uint64_t val;
};
struct Elf_Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};
struct Elf_Reloc {   // Elf_Reloc
    uint64_t offset, info;
    int64_t addend_;
    uint32_t sym() const { return info >> 32; }
    uint32_t kind() const { return info & 0xFFFFFFFF; }
    uintptr_t addend(const uintptr_t*) const { return addend_; }
};
enum { DT_RELOCS = 7, DT_RELOCS_SZ = 8 };
#endif

enum {
    PT_LOAD = 1, PT_DYNAMIC = 2,
    DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6,
    DT_INIT = 12, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27,
    R_NONE = 0, R_WORD = 1, R_GLOB_DAT = 6, R_JUMP_SLOT = 7, R_RELATIVE = 8,   // same numbers on i386 and x86-64
    STB_WEAK = 2, SHN_UNDEF = 0,
};

// Kernel symbol hash table (open addressing).
struct KSym { const char* name; uint64_t addr; };
KSym* g_ksyms;
size_t g_ksym_cap, g_ksym_count;
char* g_ksym_text;

Vector<DlModule*> g_modules;

uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

void ksym_insert(const char* name, uint64_t addr) {
    size_t i = hash_str(name) & (g_ksym_cap - 1);
    while (g_ksyms[i].name) {
        if (strcmp(g_ksyms[i].name, name) == 0) return;
        i = (i + 1) & (g_ksym_cap - 1);
    }
    g_ksyms[i] = KSym{name, addr};
    g_ksym_count++;
}

// Look up a defined symbol in a loaded module.
void* module_lookup(DlModule* m, const char* name) {
    const Elf_Sym* syms = (const Elf_Sym*)m->symtab;
    for (uint32_t i = 1; i < m->nsyms; i++) {
        const Elf_Sym& s = syms[i];
        if (s.shndx == SHN_UNDEF || !s.name) continue;
        uint8_t bind = s.info >> 4;
        if (bind == 0) continue;   // local
        if (strcmp(m->strtab + s.name, name) == 0) return m->base + s.value;
    }
    return nullptr;
}

void* resolve(const char* name) {
    void* p = kernel_symbol(name);
    if (p) return p;
    for (DlModule* m : g_modules)
        if (m->is_library)
            if ((p = module_lookup(m, name))) return p;
    return nullptr;
}

}  // namespace

bool dl_init() {
    const uint8_t* data;
    size_t size;
    if (!vfs::read_file("/System/kernel.sym", &data, &size)) {
        klog("dl: /System/kernel.sym missing");
        return false;
    }
    g_ksym_text = (char*)kmalloc(size + 1);
    memcpy(g_ksym_text, data, size);
    g_ksym_text[size] = 0;
    size_t lines = 0;
    for (size_t i = 0; i < size; i++) if (g_ksym_text[i] == '\n') lines++;
    g_ksym_cap = 1024;
    while (g_ksym_cap < lines * 2) g_ksym_cap *= 2;
    g_ksyms = (KSym*)kcalloc(g_ksym_cap, sizeof(KSym));
    char* p = g_ksym_text;
    while (*p) {
        char* line_end = strchr(p, '\n');
        if (line_end) *line_end = 0;
        char* sp = strchr(p, ' ');
        if (sp) {
            *sp = 0;
            uint64_t addr = strtoul(p, nullptr, 16);
            ksym_insert(sp + 1, addr);
        }
        if (!line_end) break;
        p = line_end + 1;
    }
    klog("dl: %zu kernel symbols exported", g_ksym_count);
    return true;
}

void* kernel_symbol(const char* name) {
    if (!g_ksyms) return nullptr;
    size_t i = hash_str(name) & (g_ksym_cap - 1);
    while (g_ksyms[i].name) {
        if (strcmp(g_ksyms[i].name, name) == 0) return (void*)g_ksyms[i].addr;
        i = (i + 1) & (g_ksym_cap - 1);
    }
    return nullptr;
}

size_t kernel_symbol_count() { return g_ksym_count; }
const Vector<DlModule*>& dl_modules() { return g_modules; }

void* dl_sym(DlModule* m, const char* name) { return module_lookup(m, name); }

DlModule* dl_open(const char* path, bool is_library, String* error) {
    auto fail = [&](const char* msg) -> DlModule* {
        klog("dl: %s: %s", path, msg);
        if (error) *error = msg;
        return nullptr;
    };
    const uint8_t* file;
    size_t fsize;
    if (!vfs::read_file(path, &file, &fsize)) return fail("not found");
    if (fsize < sizeof(Elf_Ehdr)) return fail("too small");
    const Elf_Ehdr* eh = (const Elf_Ehdr*)file;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != kElfClass || eh->machine != kElfMachine)
        return fail("built for a different CPU architecture");
    if (eh->type != 3) return fail("not a shared object (ET_DYN)");

    const Elf_Phdr* ph = (const Elf_Phdr*)(file + eh->phoff);
    uint64_t span = 0;
    const Elf_Phdr* dynph = nullptr;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > span) span = ph[i].vaddr + ph[i].memsz;
        if (ph[i].type == PT_DYNAMIC) dynph = &ph[i];
    }
    if (!span || !dynph) return fail("no loadable segments / dynamic section");

    // page-aligned image
    uint8_t* raw = (uint8_t*)kmalloc(span + 4096);
    uint8_t* base = (uint8_t*)(((uintptr_t)raw + 4095) & ~4095ull);
    memset(base, 0, span);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].offset + ph[i].filesz > fsize) { kfree(raw); return fail("segment out of bounds"); }
        memcpy(base + ph[i].vaddr, file + ph[i].offset, ph[i].filesz);
    }

    const Elf_Dyn* dyn = (const Elf_Dyn*)(base + dynph->vaddr);
    uint64_t symtab = 0, strtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, hash = 0;
    uint64_t init_array = 0, init_arraysz = 0, init = 0;
    for (; dyn->tag != DT_NULL; dyn++) {
        switch (dyn->tag) {
        case DT_SYMTAB: symtab = dyn->val; break;
        case DT_STRTAB: strtab = dyn->val; break;
        case DT_RELOCS: rela = dyn->val; break;
        case DT_RELOCS_SZ: relasz = dyn->val; break;
        case DT_JMPREL: jmprel = dyn->val; break;
        case DT_PLTRELSZ: pltrelsz = dyn->val; break;
        case DT_HASH: hash = dyn->val; break;
        case DT_INIT_ARRAY: init_array = dyn->val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = dyn->val; break;
        case DT_INIT: init = dyn->val; break;
        }
    }
    if (!symtab || !strtab || !hash) { kfree(raw); return fail("missing symtab/strtab/hash"); }

    DlModule* m = new DlModule();
    m->path = path;
    m->name = vfs::basename(path);
    m->base = base;
    m->mem_size = span;
    m->file_size = fsize;
    m->is_library = is_library;
    m->symtab = base + symtab;
    m->strtab = (const char*)(base + strtab);
    m->nsyms = ((const uint32_t*)(base + hash))[1];   // nchain == number of symbols

    const Elf_Sym* syms = (const Elf_Sym*)m->symtab;
    auto apply = [&](const Elf_Reloc* r, size_t count) -> bool {
        for (size_t i = 0; i < count; i++) {
            uint32_t type = r[i].kind();
            uint32_t si = r[i].sym();
            uintptr_t* where = (uintptr_t*)(base + r[i].offset);
            uintptr_t A = r[i].addend(where);
            uintptr_t S = 0;
            if (si) {
                const Elf_Sym& s = syms[si];
                const char* name = m->strtab + s.name;
                if (s.shndx != SHN_UNDEF) {
                    S = (uintptr_t)(base + s.value);
                } else {
                    void* addr = resolve(name);
                    if (!addr && (s.info >> 4) != STB_WEAK) {
                        String msg = String::format("unresolved symbol %s", name);
                        klog("dl: %s: %s", path, msg.c_str());
                        if (error) *error = msg;
                        return false;
                    }
                    S = (uintptr_t)addr;
                    m->imports++;
                }
            }
            switch (type) {
            case R_NONE: break;
            case R_WORD: *where = S + A; break;
            case R_GLOB_DAT:
            case R_JUMP_SLOT: *where = S; break;
            case R_RELATIVE: *where = (uintptr_t)base + A; break;
            default: {
                String msg = String::format("unsupported relocation type %u", type);
                klog("dl: %s: %s", path, msg.c_str());
                if (error) *error = msg;
                return false;
            }
            }
            m->relocations++;
        }
        return true;
    };
    if ((rela && !apply((const Elf_Reloc*)(base + rela), relasz / sizeof(Elf_Reloc))) ||
        (jmprel && !apply((const Elf_Reloc*)(base + jmprel), pltrelsz / sizeof(Elf_Reloc)))) {
        delete m;
        kfree(raw);
        return nullptr;
    }

    g_modules.push(m);
    klog("dl: loaded %s at %p (%zu KiB, %d relocs, %d imports)", m->name.c_str(), base, (size_t)(span / 1024),
         m->relocations, m->imports);

    app_set_loading_module(m->name.c_str());
    if (init) ((void (*)())(base + init))();
    if (init_array) {
        auto* fns = (void (**)())(base + init_array);
        for (size_t i = 0; i < init_arraysz / sizeof(void*); i++)
            if (fns[i] && (uintptr_t)fns[i] != (uintptr_t)-1) fns[i]();
    }
    return m;
}

int dl_load_dir(const char* dir, bool libraries) {
    vfs::Node* d = vfs::lookup(dir);
    if (!d || !d->is_dir) return 0;
    // sort names
    Vector<String> names;
    for (vfs::Node* c : d->children)
        if (!c->is_dir && vfs::extension(c->name.c_str()) == "dl") names.push(c->name);
    for (size_t i = 1; i < names.size(); i++)
        for (size_t j = i; j > 0 && names[j] < names[j - 1]; j--) {
            String t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    int n = 0;
    for (String& name : names)
        if (dl_open(vfs::join(dir, name.c_str()).c_str(), libraries)) n++;
    return n;
}
