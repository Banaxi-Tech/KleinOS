#pragma once
// FAT32 on the first MBR partition of a drive, with long file names. One volume
// is mounted at a time. Changes reach the FAT only on flush().
#include <stdint.h>
#include <stddef.h>
#include "lib/kstring.h"
#include "lib/vector.h"

namespace fat32 {

struct DirEntry {
    String name;
    bool dir;
    uint32_t size;
    uint64_t mtime;   // unix time
};

bool mount(int drive);
bool mounted();
int drive();
uint64_t free_bytes();

bool exists(const char* path);
bool list(const char* path, Vector<DirEntry>& out);
// Contents in a new kmalloc buffer (kfree it). Zero-length files give a non-null buffer.
bool read_file(const char* path, uint8_t** data, size_t* size);
// Create or replace; parent folders are created. Same-size files are rewritten in place.
bool write_file(const char* path, const void* data, size_t size, uint64_t mtime);
bool make_dirs(const char* path);
bool remove(const char* path);                 // file or folder (recursive)
bool flush();

}  // namespace fat32
