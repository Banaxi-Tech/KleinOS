#pragma once
#include <stdint.h>
#include <stddef.h>

void heap_init(uintptr_t start, size_t size);
void* kmalloc(size_t size);                 // 16-byte aligned
void* kcalloc(size_t count, size_t size);
void* krealloc(void* p, size_t size);
void kfree(void* p);
size_t heap_total();
size_t heap_used();

extern "C" {
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* p, size_t size);
void free(void* p);
}
