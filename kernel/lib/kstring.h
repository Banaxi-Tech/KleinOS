#pragma once
// Small owning, growable string (UTF-8 bytes).
#include <stddef.h>
#include <stdarg.h>
#include "lib/string.h"
#include "lib/heap.h"

class String {
public:
    String() { set("", 0); }
    String(const char* s) { set(s ? s : "", s ? strlen(s) : 0); }
    String(const char* s, size_t n) { set(s, n); }
    String(const String& o) { set(o.data_, o.len_); }
    String(String&& o) noexcept : data_(o.data_), len_(o.len_), cap_(o.cap_) { o.data_ = nullptr; o.len_ = o.cap_ = 0; }
    ~String() { kfree(data_); }

    String& operator=(const String& o) { if (this != &o) { kfree(data_); data_ = nullptr; set(o.data_, o.len_); } return *this; }
    String& operator=(String&& o) noexcept {
        if (this != &o) { kfree(data_); data_ = o.data_; len_ = o.len_; cap_ = o.cap_; o.data_ = nullptr; o.len_ = o.cap_ = 0; }
        return *this;
    }
    String& operator=(const char* s) { kfree(data_); data_ = nullptr; set(s ? s : "", s ? strlen(s) : 0); return *this; }

    static String format(const char* fmt, ...) __attribute__((format(printf, 1, 2))) {
        char tmp[512];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);
        if (n < (int)sizeof(tmp)) return String(tmp, n);
        String s;
        s.reserve(n + 1);
        va_start(ap, fmt);
        vsnprintf(s.data_, n + 1, fmt, ap);
        va_end(ap);
        s.len_ = n;
        return s;
    }

    const char* c_str() const { return data_ ? data_ : ""; }
    char* data() { return data_; }
    size_t size() const { return len_; }
    size_t length() const { return len_; }
    bool empty() const { return len_ == 0; }
    char operator[](size_t i) const { return data_[i]; }
    char& operator[](size_t i) { return data_[i]; }

    void reserve(size_t n) {
        if (n + 1 <= cap_) return;
        size_t nc = cap_ ? cap_ : 16;
        while (nc < n + 1) nc *= 2;
        char* nd = (char*)kmalloc(nc);
        if (data_) memcpy(nd, data_, len_ + 1); else nd[0] = 0;
        kfree(data_);
        data_ = nd;
        cap_ = nc;
    }
    String& append(const char* s, size_t n) {
        reserve(len_ + n);
        memcpy(data_ + len_, s, n);
        len_ += n;
        data_[len_] = 0;
        return *this;
    }
    String& append(const char* s) { return append(s, strlen(s)); }
    String& append(const String& s) { return append(s.data_, s.len_); }
    String& append(char c) { return append(&c, 1); }
    String& operator+=(const char* s) { return append(s); }
    String& operator+=(const String& s) { return append(s); }
    String& operator+=(char c) { return append(c); }
    String operator+(const String& o) const { String r(*this); r.append(o); return r; }
    String operator+(const char* o) const { String r(*this); r.append(o); return r; }

    void insert(size_t pos, const char* s, size_t n) {
        if (pos > len_) pos = len_;
        reserve(len_ + n);
        memmove(data_ + pos + n, data_ + pos, len_ - pos + 1);
        memcpy(data_ + pos, s, n);
        len_ += n;
    }
    void erase(size_t pos, size_t n) {
        if (pos >= len_) return;
        if (pos + n > len_) n = len_ - pos;
        memmove(data_ + pos, data_ + pos + n, len_ - pos - n + 1);
        len_ -= n;
    }
    void clear() { if (data_) { data_[0] = 0; } len_ = 0; }
    String substr(size_t pos, size_t n = (size_t)-1) const {
        if (pos >= len_) return String();
        if (n > len_ - pos) n = len_ - pos;
        return String(data_ + pos, n);
    }
    long find(const char* needle, size_t from = 0) const {
        if (from > len_) return -1;
        const char* p = strstr(data_ + from, needle);
        return p ? (long)(p - data_) : -1;
    }
    long find(char c, size_t from = 0) const {
        for (size_t i = from; i < len_; i++) if (data_[i] == c) return (long)i;
        return -1;
    }
    long rfind(char c) const {
        for (size_t i = len_; i > 0; i--) if (data_[i - 1] == c) return (long)(i - 1);
        return -1;
    }
    bool starts_with(const char* p) const { return strncmp(c_str(), p, strlen(p)) == 0; }
    bool ends_with(const char* p) const { size_t n = strlen(p); return n <= len_ && strcmp(data_ + len_ - n, p) == 0; }
    String lower() const { String r(*this); for (size_t i = 0; i < len_; i++) r.data_[i] = (char)tolower(r.data_[i]); return r; }
    String trim() const {
        size_t a = 0, b = len_;
        while (a < b && isspace(data_[a])) a++;
        while (b > a && isspace(data_[b - 1])) b--;
        return substr(a, b - a);
    }

    bool operator==(const String& o) const { return len_ == o.len_ && memcmp(c_str(), o.c_str(), len_) == 0; }
    bool operator==(const char* o) const { return strcmp(c_str(), o) == 0; }
    bool operator!=(const String& o) const { return !(*this == o); }
    bool operator!=(const char* o) const { return !(*this == o); }
    bool operator<(const String& o) const { return strcmp(c_str(), o.c_str()) < 0; }

private:
    void set(const char* s, size_t n) {
        data_ = nullptr; len_ = 0; cap_ = 0;
        reserve(n);
        memcpy(data_, s, n);
        data_[n] = 0;
        len_ = n;
    }
    char* data_ = nullptr;
    size_t len_ = 0;
    size_t cap_ = 0;
};
