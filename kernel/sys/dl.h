#pragma once
// .dl dynamic libraries: ELF64 shared objects (x86-64) loaded at runtime and
// linked against the kernel's exported symbols (/System/kernel.sym) and
// previously loaded libraries in /System/Libraries.
#include <stdint.h>
#include <stddef.h>
#include "lib/kstring.h"
#include "lib/vector.h"

struct DlModule {
    String name;        // "Calculator.dl"
    String path;        // "/System/Apps/Calculator.dl"
    uint8_t* base = nullptr;
    size_t mem_size = 0;
    size_t file_size = 0;
    bool is_library = false;
    // dynamic symbol table
    const void* symtab = nullptr;
    const char* strtab = nullptr;
    uint32_t nsyms = 0;
    int relocations = 0;
    int imports = 0;
};

// Parse /System/kernel.sym. Must run before any dl_open.
bool dl_init();
// Load a .dl file; runs its constructors (apps register themselves there).
DlModule* dl_open(const char* path, bool is_library = false, String* error = nullptr);
void* dl_sym(DlModule* m, const char* name);
// Load every .dl in a directory (sorted by name). Returns number loaded.
int dl_load_dir(const char* dir, bool libraries);
const Vector<DlModule*>& dl_modules();
// Address of a kernel symbol (mangled name), or nullptr.
void* kernel_symbol(const char* name);
size_t kernel_symbol_count();
