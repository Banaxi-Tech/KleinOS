/* Minimal C library for QuickJS on KleinOS (C and C++ compatible).
 * Most functions are exported by the kernel (kernel/lib); the rest live in
 * libs/QuickJS/klibc.c. */
#ifndef KLIBC_H
#define KLIBC_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>
#ifdef __cplusplus
extern "C" {
#endif
/* memory */
void* malloc(size_t n);
void* calloc(size_t c, size_t n);
void* realloc(void* p, size_t n);
void free(void* p);
void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);
size_t malloc_usable_size(void* p);
#define alloca(n) __builtin_alloca(n)
/* strings */
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t n);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strcpy(char* d, const char* s);
char* strncpy(char* d, const char* s, size_t n);
char* strcat(char* d, const char* s);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* h, const char* n);
char* strdup(const char* s);
char* strpbrk(const char* s, const char* a);
long strtol(const char* s, char** e, int b);
unsigned long strtoul(const char* s, char** e, int b);
long long strtoll(const char* s, char** e, int b);
unsigned long long strtoull(const char* s, char** e, int b);
double strtod(const char* s, char** e);
int atoi(const char* s);
/* stdio */
typedef struct KFILE FILE;
extern FILE* stdout;
extern FILE* stderr;
extern FILE* stdin;
int vsnprintf(char* b, size_t n, const char* f, va_list ap);
int snprintf(char* b, size_t n, const char* f, ...);
int sprintf(char* b, const char* f, ...);
int printf(const char* f, ...);
int fprintf(FILE* fp, const char* f, ...);
int vfprintf(FILE* fp, const char* f, va_list ap);
int fputs(const char* s, FILE* fp);
int fputc(int c, FILE* fp);
int putchar(int c);
int puts(const char* s);
int fflush(FILE* fp);
size_t fwrite(const void* p, size_t s, size_t n, FILE* fp);
#define putc fputc
#define EOF (-1)
/* stdlib */
void abort(void) __attribute__((noreturn));
void exit(int code) __attribute__((noreturn));
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
int abs(int v);
long labs(long v);
long long llabs(long long v);
char* getenv(const char* n);
/* ctype */
int isdigit(int c); int isalpha(int c); int isalnum(int c); int isspace(int c); int isxdigit(int c);
int isupper(int c); int islower(int c); int isprint(int c); int toupper(int c); int tolower(int c);
/* errno */
extern int errno;
#define ERANGE 34
#define EINVAL 22
#define ENOMEM 12
#ifdef __cplusplus
}
#endif
#endif
