#pragma once
// Func<R(Args...)>: small type-erased callable (like std::function) for UI callbacks.
//   Func<void()> on_click = [this] { do_something(); };
#include <utility>
#include <new>
#include "lib/heap.h"

template <typename Sig> class Func;

template <typename R, typename... A>
class Func<R(A...)> {
public:
    Func() = default;
    Func(decltype(nullptr)) {}
    template <typename F>
    Func(F f) { impl_ = new Impl<F>(std::move(f)); }
    Func(const Func& o) : impl_(o.impl_ ? o.impl_->clone() : nullptr) {}
    Func(Func&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
    ~Func() { delete impl_; }
    Func& operator=(const Func& o) { if (this != &o) { delete impl_; impl_ = o.impl_ ? o.impl_->clone() : nullptr; } return *this; }
    Func& operator=(Func&& o) noexcept { if (this != &o) { delete impl_; impl_ = o.impl_; o.impl_ = nullptr; } return *this; }
    explicit operator bool() const { return impl_ != nullptr; }
    R operator()(A... a) const { return impl_->call(std::forward<A>(a)...); }

private:
    struct Base {
        virtual ~Base() {}
        virtual R call(A... a) = 0;
        virtual Base* clone() const = 0;
    };
    template <typename F>
    struct Impl : Base {
        F f;
        Impl(F&& fn) : f(std::move(fn)) {}
        Impl(const F& fn) : f(fn) {}
        R call(A... a) override { return f(std::forward<A>(a)...); }
        Base* clone() const override { return new Impl<F>(f); }
    };
    Base* impl_ = nullptr;
};
