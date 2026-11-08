#pragma once
// Minimal std::vector replacement.
#include <stddef.h>
#include <new>
#include <utility>
#include <initializer_list>
#include "lib/heap.h"

template <typename T>
class Vector {
public:
    Vector() = default;
    Vector(std::initializer_list<T> init) {
        reserve(init.size());
        for (const T& v : init) push(v);
    }
    Vector(const Vector& o) { *this = o; }
    Vector(Vector&& o) noexcept : data_(o.data_), size_(o.size_), cap_(o.cap_) {
        o.data_ = nullptr; o.size_ = o.cap_ = 0;
    }
    ~Vector() { clear(); kfree(data_); }

    Vector& operator=(const Vector& o) {
        if (this == &o) return *this;
        clear();
        reserve(o.size_);
        for (size_t i = 0; i < o.size_; i++) new (&data_[i]) T(o.data_[i]);
        size_ = o.size_;
        return *this;
    }
    Vector& operator=(Vector&& o) noexcept {
        if (this == &o) return *this;
        clear(); kfree(data_);
        data_ = o.data_; size_ = o.size_; cap_ = o.cap_;
        o.data_ = nullptr; o.size_ = o.cap_ = 0;
        return *this;
    }

    void reserve(size_t n) {
        if (n <= cap_) return;
        T* nd = (T*)kmalloc(n * sizeof(T));
        for (size_t i = 0; i < size_; i++) {
            new (&nd[i]) T(std::move(data_[i]));
            data_[i].~T();
        }
        kfree(data_);
        data_ = nd;
        cap_ = n;
    }
    void resize(size_t n) {
        reserve(n);
        for (size_t i = size_; i < n; i++) new (&data_[i]) T();
        for (size_t i = n; i < size_; i++) data_[i].~T();
        size_ = n;
    }
    void push(const T& v) { grow(); new (&data_[size_++]) T(v); }
    void push(T&& v) { grow(); new (&data_[size_++]) T(std::move(v)); }
    void push_back(const T& v) { push(v); }
    void push_back(T&& v) { push(std::move(v)); }
    template <typename... A> T& emplace(A&&... a) { grow(); new (&data_[size_]) T(std::forward<A>(a)...); return data_[size_++]; }
    void pop() { if (size_) data_[--size_].~T(); }
    void pop_back() { pop(); }
    void insert(size_t idx, const T& v) {
        if (idx >= size_) { push(v); return; }
        grow();
        new (&data_[size_]) T(std::move(data_[size_ - 1]));
        for (size_t i = size_ - 1; i > idx; i--) data_[i] = std::move(data_[i - 1]);
        data_[idx] = v;
        size_++;
    }
    void remove(size_t idx) {
        if (idx >= size_) return;
        for (size_t i = idx; i + 1 < size_; i++) data_[i] = std::move(data_[i + 1]);
        data_[--size_].~T();
    }
    void erase(size_t idx) { remove(idx); }
    // Remove first element equal to v. Returns true if found.
    bool remove_value(const T& v) {
        for (size_t i = 0; i < size_; i++) if (data_[i] == v) { remove(i); return true; }
        return false;
    }
    long index_of(const T& v) const {
        for (size_t i = 0; i < size_; i++) if (data_[i] == v) return (long)i;
        return -1;
    }
    bool contains(const T& v) const { return index_of(v) >= 0; }
    void clear() { for (size_t i = 0; i < size_; i++) data_[i].~T(); size_ = 0; }

    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }
    T& back() { return data_[size_ - 1]; }
    const T& back() const { return data_[size_ - 1]; }
    T& front() { return data_[0]; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

private:
    void grow() { if (size_ == cap_) reserve(cap_ ? cap_ * 2 : 8); }
    T* data_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
};
