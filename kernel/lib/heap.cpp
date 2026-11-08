// Boundary-tag first-fit allocator with free-block coalescing.
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/log.h"
#include "arch/cpu.h"
#include "sys/panic.h"

namespace {

constexpr uint64_t kMagicUsed = 0xA110CA7EDB10C000ull;
constexpr uint64_t kMagicFree = 0xF2EEB10CF2EEB100ull;

// Header precedes each block; size includes the header and footer.
struct alignas(16) Block {   // payloads must stay 16-byte aligned on 32-bit too
    uint64_t magic;
    uint64_t size;
    Block* next_free;   // valid when free
    Block* prev_free;
};
// Footer at the end of every block: copy of size.
constexpr size_t kHeader = sizeof(Block);
constexpr size_t kFooter = 16;
constexpr size_t kMinBlock = kHeader + kFooter + 16;

uint8_t* g_start;
uint8_t* g_end;
Block* g_free_list;
size_t g_used;

inline uint64_t* footer(Block* b) { return (uint64_t*)((uint8_t*)b + b->size - kFooter); }
inline void set_size(Block* b, size_t s) {
    b->size = s;
    *footer(b) = s;
}

void list_remove(Block* b) {
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else g_free_list = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->next_free = b->prev_free = nullptr;
}

void list_push(Block* b) {
    b->magic = kMagicFree;
    b->prev_free = nullptr;
    b->next_free = g_free_list;
    if (g_free_list) g_free_list->prev_free = b;
    g_free_list = b;
}

}  // namespace

void heap_init(uintptr_t start, size_t size) {
    start = (start + 15) & ~15ull;
    size &= ~15ull;
    g_start = (uint8_t*)start;
    g_end = g_start + size;
    Block* b = (Block*)g_start;
    set_size(b, size);
    g_free_list = nullptr;
    list_push(b);
    g_used = 0;
}

void* kmalloc(size_t size) {
    if (size == 0) size = 1;
    size_t need = (size + kHeader + kFooter + 15) & ~15ull;
    if (need < kMinBlock) need = kMinBlock;
    IrqGuard guard;
    for (Block* b = g_free_list; b; b = b->next_free) {
        if (b->size < need) continue;
        list_remove(b);
        if (b->size - need >= kMinBlock) {
            Block* rest = (Block*)((uint8_t*)b + need);
            set_size(rest, b->size - need);
            list_push(rest);
            set_size(b, need);
        }
        b->magic = kMagicUsed;
        g_used += b->size;
        return (uint8_t*)b + kHeader;
    }
    klog("heap: out of memory (request %zu bytes, used %zu / %zu)", size, g_used, heap_total());
    kpanic("Out of memory");
    return nullptr;
}

void kfree(void* p) {
    if (!p) return;
    IrqGuard guard;
    Block* b = (Block*)((uint8_t*)p - kHeader);
    if (b->magic != kMagicUsed) {
        klog("heap: bad free %p (magic %llx)", p, (unsigned long long)b->magic);
        return;
    }
    g_used -= b->size;
    // merge with next
    Block* next = (Block*)((uint8_t*)b + b->size);
    if ((uint8_t*)next < g_end && next->magic == kMagicFree) {
        list_remove(next);
        next->magic = 0;
        set_size(b, b->size + next->size);
    }
    // merge with previous
    if ((uint8_t*)b > g_start) {
        uint64_t prev_size = *(uint64_t*)((uint8_t*)b - kFooter);
        Block* prev = (Block*)((uint8_t*)b - prev_size);
        if ((uint8_t*)prev >= g_start && prev->magic == kMagicFree && prev->size == prev_size) {
            list_remove(prev);
            b->magic = 0;
            set_size(prev, prev->size + b->size);
            b = prev;
        }
    }
    list_push(b);
}

void* kcalloc(size_t count, size_t size) {
    void* p = kmalloc(count * size);
    memset(p, 0, count * size);
    return p;
}

void* krealloc(void* p, size_t size) {
    if (!p) return kmalloc(size);
    if (!size) { kfree(p); return nullptr; }
    Block* b = (Block*)((uint8_t*)p - kHeader);
    size_t cap = b->size - kHeader - kFooter;
    if (cap >= size) return p;
    void* n = kmalloc(size);
    memcpy(n, p, cap);
    kfree(p);
    return n;
}

size_t heap_total() { return g_end - g_start; }
size_t heap_used() { return g_used; }

extern "C" {
void* malloc(size_t size) { return kmalloc(size); }
void* calloc(size_t c, size_t s) { return kcalloc(c, s); }
void* realloc(void* p, size_t s) { return krealloc(p, s); }
void free(void* p) { kfree(p); }
}

void* operator new(size_t s) { return kmalloc(s); }
void* operator new[](size_t s) { return kmalloc(s); }
void operator delete(void* p) noexcept { kfree(p); }
void operator delete[](void* p) noexcept { kfree(p); }
void operator delete(void* p, size_t) noexcept { kfree(p); }
void operator delete[](void* p, size_t) noexcept { kfree(p); }
