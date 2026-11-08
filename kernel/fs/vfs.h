#pragma once
// In-memory hierarchical filesystem. /System is populated from the initrd;
// /Users/<name> holds user files (persisted to disk on installed systems).
#include <stdint.h>
#include <stddef.h>
#include "lib/kstring.h"
#include "lib/vector.h"

namespace vfs {

struct Node {
    String name;
    bool is_dir = false;
    bool readonly = false;     // initrd-backed files
    uint8_t* data = nullptr;   // file contents
    size_t size = 0;
    size_t capacity = 0;       // 0 = data points to external (initrd) memory
    uint64_t mtime = 0;        // unix time
    Node* parent = nullptr;
    Vector<Node*> children;
};

void init();
Node* root();
// Resolve an absolute path ("/Users/guest/Documents"). nullptr if missing.
Node* lookup(const char* path);
// Full path of a node into a String.
String path_of(const Node* n);

bool exists(const char* path);
bool is_dir(const char* path);
// Create a directory (and missing parents). Returns the node.
Node* mkdir(const char* path);
// Read: returns pointer to the file contents (not a copy; valid until modified).
bool read_file(const char* path, const uint8_t** data, size_t* size);
// Read into a NUL-terminated String.
bool read_text(const char* path, String& out);
// Create or replace a file (parents created).
bool write_file(const char* path, const void* data, size_t size);
bool write_text(const char* path, const char* text);
bool append_file(const char* path, const void* data, size_t size);
bool remove(const char* path);              // file or directory (recursive)
bool rename(const char* from, const char* to);
bool copy(const char* from, const char* to);
// Add an external, read-only file (initrd).
Node* add_external(const char* path, const uint8_t* data, size_t size);

// Helpers for paths
String join(const char* dir, const char* name);
String dirname(const char* path);
String basename(const char* path);
String extension(const char* path);   // lowercase, without dot ("txt")

// Called whenever something under /Users changes (persistence layer hooks in).
using ChangeHook = void (*)();
void set_change_hook(ChangeHook hook);
// Monotonic counter bumped on every change (UI can poll to refresh listings).
uint64_t generation();

// Serialize/deserialize the subtree at path (for disk persistence).
bool serialize(const char* path, Vector<uint8_t>& out);
bool deserialize(const char* path, const uint8_t* data, size_t size);

// Mount the initrd tar archive at /.
void mount_initrd(const uint8_t* tar, size_t size);

}  // namespace vfs
