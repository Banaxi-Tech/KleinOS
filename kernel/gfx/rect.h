#pragma once

struct Point {
    int x = 0, y = 0;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    constexpr Rect() = default;
    constexpr Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}

    constexpr int right() const { return x + w; }
    constexpr int bottom() const { return y + h; }
    constexpr bool empty() const { return w <= 0 || h <= 0; }
    constexpr bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    constexpr bool contains(Point p) const { return contains(p.x, p.y); }
    constexpr bool intersects(const Rect& o) const {
        return !empty() && !o.empty() && x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    constexpr Rect intersect(const Rect& o) const {
        int nx = x > o.x ? x : o.x, ny = y > o.y ? y : o.y;
        int nr = right() < o.right() ? right() : o.right(), nb = bottom() < o.bottom() ? bottom() : o.bottom();
        if (nr <= nx || nb <= ny) return Rect();
        return Rect(nx, ny, nr - nx, nb - ny);
    }
    constexpr Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        int nx = x < o.x ? x : o.x, ny = y < o.y ? y : o.y;
        int nr = right() > o.right() ? right() : o.right(), nb = bottom() > o.bottom() ? bottom() : o.bottom();
        return Rect(nx, ny, nr - nx, nb - ny);
    }
    constexpr Rect inset(int d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
    constexpr Rect inset(int dx, int dy) const { return Rect(x + dx, y + dy, w - 2 * dx, h - 2 * dy); }
    constexpr Rect expand(int d) const { return inset(-d); }
    constexpr Rect offset(int dx, int dy) const { return Rect(x + dx, y + dy, w, h); }
    constexpr Point center() const { return Point{x + w / 2, y + h / 2}; }
    constexpr bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    constexpr bool operator!=(const Rect& o) const { return !(*this == o); }
};
