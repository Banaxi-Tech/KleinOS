#include "lib/string.h"
#include "lib/heap.h"

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
#ifdef __i386__
    // Atom's byte-string moves are slow; words first
    size_t words = n >> 2;
    n &= 3;
    asm volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(words) : : "memory");
#endif
    asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    // overlapping, copy backwards
    d += n - 1;
    s += n - 1;
    asm volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
#ifdef __i386__
    uint32_t fill = (uint8_t)c * 0x01010101u;
    size_t words = n >> 2;
    n &= 3;
    asm volatile("rep stosl" : "+D"(dst), "+c"(words) : "a"(fill) : "memory");
#endif
    asm volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (uint8_t)a[i] - (uint8_t)b[i];
    }
    return 0;
}

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }
int abs(int v) { return v < 0 ? -v : v; }

int strcasecmp(const char* a, const char* b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((uint8_t)a[i]), y = tolower((uint8_t)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* strcat(char* dst, const char* src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char* strchr(const char* s, int c) {
    for (; *s; s++)
        if (*s == (char)c) return (char*)s;
    return c == 0 ? (char*)s : nullptr;
}

char* strrchr(const char* s, int c) {
    const char* last = nullptr;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    return c == 0 ? (char*)s : (char*)last;
}

char* strstr(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (!n) return (char*)hay;
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, n) == 0) return (char*)hay;
    return nullptr;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (uint8_t)c) return (void*)(p + i);
    return nullptr;
}

char* strpbrk(const char* s, const char* accept) {
    for (; *s; s++)
        if (strchr(accept, *s)) return (char*)s;
    return nullptr;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)kmalloc(n);
    memcpy(d, s, n);
    return d;
}

unsigned long strtoul(const char* s, char** end, int base) {
    while (isspace(*s)) s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long v = 0;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char*)s;
    return v;
}

long strtol(const char* s, char** end, int base) {
    while (isspace(*s)) s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;
    long v = (long)strtoul(s, end, base);
    return neg ? -v : v;
}

double strtod(const char* s, char** end) {
    while (isspace(*s)) s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;
    double v = 0;
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double f = 0.1;
        while (isdigit(*s)) { v += (*s++ - '0') * f; f *= 0.1; }
    }
    if (*s == 'e' || *s == 'E') {
        const char* save = s;
        s++;
        bool eneg = false;
        if (*s == '-') { eneg = true; s++; }
        else if (*s == '+') s++;
        if (!isdigit(*s)) s = save;
        else {
            int e = 0;
            while (isdigit(*s)) e = e * 10 + (*s++ - '0');
            double m = 1;
            while (e--) m *= 10;
            v = eneg ? v / m : v * m;
        }
    }
    if (end) *end = (char*)s;
    return neg ? -v : v;
}

int atoi(const char* s) { return (int)strtol(s, nullptr, 10); }

// ------------------------------------------------------------------ printf
struct Out {
    char* buf;
    size_t size;
    size_t pos;
    void put(char c) {
        if (pos + 1 < size) buf[pos] = c;
        pos++;
    }
};

static void out_str(Out& o, const char* s, int width, bool left, int prec) {
    if (!s) s = "(null)";
    int len = (int)strlen(s);
    if (prec >= 0 && len > prec) len = prec;
    if (!left) for (int i = len; i < width; i++) o.put(' ');
    for (int i = 0; i < len; i++) o.put(s[i]);
    if (left) for (int i = len; i < width; i++) o.put(' ');
}

static void out_num(Out& o, unsigned long long v, int base, bool neg, int width, bool left, bool zero,
                    bool upper, bool plus, int prec) {
    char tmp[32];
    int n = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    while (n < prec) tmp[n++] = '0';
    char sign = neg ? '-' : (plus ? '+' : 0);
    int total = n + (sign ? 1 : 0);
    if (!left && !zero) for (int i = total; i < width; i++) o.put(' ');
    if (sign) o.put(sign);
    if (!left && zero) for (int i = total; i < width; i++) o.put('0');
    while (n) o.put(tmp[--n]);
    if (left) for (int i = total; i < width; i++) o.put(' ');
}

static void out_float(Out& o, double v, int prec, int width, bool left, bool zero, bool plus) {
    if (prec < 0) prec = 6;
    char tmp[64];
    int n = 0;
    bool neg = v < 0;
    if (neg) v = -v;
    if (v != v) { out_str(o, "nan", width, left, -1); return; }
    if (v > 1e18) { out_str(o, neg ? "-inf" : "inf", width, left, -1); return; }
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10;
    v += r;
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    char ibuf[24];
    int in = 0;
    if (ip == 0) ibuf[in++] = '0';
    while (ip) { ibuf[in++] = '0' + ip % 10; ip /= 10; }
    if (neg) tmp[n++] = '-';
    else if (plus) tmp[n++] = '+';
    while (in) tmp[n++] = ibuf[--in];
    if (prec > 0) {
        tmp[n++] = '.';
        for (int i = 0; i < prec && n < 60; i++) {
            frac *= 10;
            int d = (int)frac;
            tmp[n++] = '0' + d;
            frac -= d;
        }
    }
    tmp[n] = 0;
    if (zero && !left) {
        int pad = width - n;
        int start = (tmp[0] == '-' || tmp[0] == '+') ? 1 : 0;
        if (start) o.put(tmp[0]);
        for (int i = 0; i < pad; i++) o.put('0');
        for (int i = start; i < n; i++) o.put(tmp[i]);
        return;
    }
    out_str(o, tmp, width, left, -1);
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    Out o{buf, size, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') { o.put(*fmt); continue; }
        fmt++;
        bool left = false, zero = false, plus = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ' || *fmt == '#') {}
            else break;
        }
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 2; fmt++; }
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            out_num(o, v < 0 ? -(unsigned long long)v : v, 10, v < 0, width, left, zero, false, plus, prec);
            break;
        }
        case 'u': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            out_num(o, v, 10, false, width, left, zero, false, plus, prec);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            out_num(o, v, 16, false, width, left, zero, *fmt == 'X', false, prec);
            break;
        }
        case 'o': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            out_num(o, v, 8, false, width, left, zero, false, false, prec);
            break;
        }
        case 'p': {
            o.put('0'); o.put('x');
            out_num(o, (uintptr_t)va_arg(ap, void*), 16, false, 16, false, true, false, false, -1);
            break;
        }
        case 'c': o.put((char)va_arg(ap, int)); break;
        case 's': out_str(o, va_arg(ap, const char*), width, left, prec); break;
        case 'f': case 'g': case 'e': out_float(o, va_arg(ap, double), prec, width, left, zero, plus); break;
        case '%': o.put('%'); break;
        case 0: fmt--; break;
        default: o.put('%'); o.put(*fmt); break;
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

}  // extern "C"

size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

uint32_t utf8_next(const char** sp) {
    const uint8_t* s = (const uint8_t*)*sp;
    uint32_t c = *s;
    if (!c) return 0;
    int extra = 0;
    if (c < 0x80) extra = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { *sp = (const char*)(s + 1); return 0xFFFD; }
    s++;
    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0) != 0x80) { *sp = (const char*)s; return 0xFFFD; }
        c = (c << 6) | (*s++ & 0x3F);
    }
    *sp = (const char*)s;
    return c;
}

int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F);
    return 4;
}
