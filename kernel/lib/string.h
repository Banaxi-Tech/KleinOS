#pragma once
// Freestanding C string/memory functions + printf family.
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

extern "C" {
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* hay, const char* needle);
void* memchr(const void* s, int c, size_t n);
char* strpbrk(const char* s, const char* accept);
char* strdup(const char* s);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
double strtod(const char* s, char** end);
int atoi(const char* s);
int toupper(int c);
int tolower(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isxdigit(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int abs(int v);
}

// Safe bounded copy, always NUL-terminates (returns strlen(src)).
size_t strlcpy(char* dst, const char* src, size_t size);
// Decode one UTF-8 code point from *s, advancing *s. Returns 0 at end.
uint32_t utf8_next(const char** s);
// Encode a code point as UTF-8 into out (>= 5 bytes). Returns bytes written.
int utf8_encode(uint32_t cp, char* out);
