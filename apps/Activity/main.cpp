// Activity — system monitor: CPU/frame rate, memory, open windows, loaded
// modules and devices. Samples twice a second.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "sys/dl.h"
#include "sys/system.h"
#include "drivers/ata.h"
#include "drivers/pci.h"
#include "net/net.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "boot/multiboot2.h"
#include "lib/heap.h"
#include "lib/math.h"
#include "lib/string.h"
#include "lib/kstring.h"
#include "lib/vector.h"

namespace {

constexpr int kHist = 120;          // samples kept (2 Hz -> one minute)
constexpr uint64_t kSampleMs = 500;
constexpr int kTabHeaderH = 56;

enum class Tab : uint8_t { Cpu, Memory, Windows, Modules, Devices };

void fmt_size(char* buf, size_t n, uint64_t bytes) {
    if (bytes < 1024) snprintf(buf, n, "%llu B", (unsigned long long)bytes);
    else if (bytes < 1024ull * 1024) snprintf(buf, n, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) snprintf(buf, n, "%.1f MB", bytes / 1048576.0);
    else snprintf(buf, n, "%.2f GB", bytes / 1073741824.0);
}

Rect R(float x, float y, float w, float h) {
    return Rect((int)(x + 0.5f), (int)(y + 0.5f), (int)(w + 0.5f), (int)(h + 0.5f));
}

void push_sample(float* vals, int& n, float v) {
    if (n < kHist) { vals[n++] = v; return; }
    memmove(vals, vals + 1, (kHist - 1) * sizeof(float));
    vals[kHist - 1] = v;
}

// Catmull-Rom point between p1 and p2 at t in 0..1.
float catmull(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

// Smooth line graph with a gradient fill. Values run oldest -> newest across r.
void draw_graph(Canvas& c, Rect r, const float* v, int n, float maxv, Color line) {
    const Theme& t = theme();
    // grid
    for (int g = 1; g <= 3; g++) {
        int gy = r.bottom() - r.h * g / 4;
        c.fill_rect(Rect(r.x, gy, r.w, 1), t.dark ? rgba(255, 255, 255, 20) : rgba(0, 0, 0, 18));
    }
    if (n < 2 || maxv <= 0) return;

    // sample points
    const int steps = 6;
    int total = (n - 1) * steps + 1;
    float* py = (float*)kmalloc(sizeof(float) * total);
    float* px = (float*)kmalloc(sizeof(float) * total);
    auto val = [&](int i) { return v[kclamp(i, 0, n - 1)]; };
    for (int s = 0; s < total; s++) {
        int seg = s / steps;
        float tt = (float)(s % steps) / steps;
        float yv = seg >= n - 1 ? val(n - 1) : catmull(val(seg - 1), val(seg), val(seg + 1), val(seg + 2), tt);
        if (yv < 0) yv = 0;
        if (yv > maxv) yv = maxv;
        px[s] = r.x + (float)s * (r.w - 1) / (total - 1);
        py[s] = r.bottom() - 1 - yv / maxv * (r.h - 2);
    }
    // gradient fill, one column at a time
    for (int x = r.x; x < r.right(); x++) {
        float fx = (x - r.x) * (total - 1) / (float)(r.w - 1 > 0 ? r.w - 1 : 1);
        int s = (int)fx;
        if (s >= total - 1) s = total - 2;
        if (s < 0) s = 0;
        float k = fx - s;
        float y = py[s] + (py[s + 1] - py[s]) * k;
        int top = (int)y;
        if (top >= r.bottom()) continue;
        c.fill_gradient(Rect(x, top, 1, r.bottom() - top), with_alpha(line, 120), with_alpha(line, 6), 0);
    }
    // the curve itself
    for (int s = 0; s + 1 < total; s++)
        c.draw_line(px[s], py[s], px[s + 1], py[s + 1], line, 2.0f);
    // newest value marker
    c.fill_circle(px[total - 1], py[total - 1], 3.5f, line);
    c.fill_circle(px[total - 1], py[total - 1], 1.5f, hex(0xFFFFFF));
    kfree(py);
    kfree(px);
}

void draw_card(Canvas& c, Rect r) {
    const Theme& t = theme();
    c.fill_round_rect(r, 12, t.dark ? rgba(255, 255, 255, 14) : rgba(255, 255, 255, 255));
    c.stroke_round_rect(r, 12, t.separator, 1);
}

void stat(Canvas& c, Rect r, const char* label, const char* value) {
    const Theme& t = theme();
    draw_card(c, r);
    c.draw_text_in(Rect(r.x + 14, r.y + 10, r.w - 28, 16), label, t.text_secondary, 12, Font::Medium);
    c.draw_text_in(Rect(r.x + 14, r.y + 30, r.w - 28, 24), value, t.text, 17, Font::Bold);
}

class ActivityWindow : public Window {
public:
    ActivityWindow() : Window("Activity", 700, 540, WF_RESIZABLE) {
        set_min_size(560, 420);
        seg_ = (SegmentedControl*)add(new SegmentedControl(Rect(0, 0, 460, 30), {"CPU", "Memory", "Windows", "Modules", "Devices"}));
        seg_->on_change = [this](int i) { tab_ = (Tab)i; scroll_ = 0; sel_win_ = nullptr; invalidate(); };
        quit_ = (Button*)add(new Button(Rect(0, 0, 110, 30), "Quit Window", Button::Destructive));
        quit_->on_click = [this] { quit_selected(); };
        quit_->visible = false;
        layout();
        sample();
        set_ticking(true);
    }

    void on_resize() override { layout(); }

    void on_tick(uint64_t now) override {
        if (now - last_ms_ >= kSampleMs || last_ms_ == 0) {
            last_ms_ = now;
            sample();
            invalidate();
        }
    }

    void on_scroll(int x, int y, int delta) override {
        if (tab_ == Tab::Cpu || tab_ == Tab::Memory) { Window::on_scroll(x, y, delta); return; }
        scroll_ += delta * 28;
        clamp_scroll();
        invalidate();
    }

    void on_mouse_down(int x, int y, int button) override {
        if (widget_at(x, y)) { Window::on_mouse_down(x, y, button); return; }
        if (tab_ == Tab::Windows && y >= list_top() && y < list_bottom()) {
            const Vector<Window*>& ws = wm::windows();
            int i = (y - list_top() + scroll_) / kWinRowH;
            if (i >= 0 && i < (int)ws.size()) {
                sel_win_ = ws[i];
                invalidate();
            }
        }
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        int w = width(), h = height();
        c.clear(t.window_bg);
        c.fill_rect(Rect(0, kTabHeaderH - 1, w, 1), t.separator);
        // title line for the current tab
        switch (tab_) {
        case Tab::Cpu: paint_cpu(c); break;
        case Tab::Memory: paint_memory(c); break;
        case Tab::Windows: paint_windows(c); break;
        case Tab::Modules: paint_modules(c); break;
        case Tab::Devices: paint_devices(c); break;
        }
        for (Widget* wd : widgets()) {
            if (!wd->visible) continue;
            c.save();
            wd->paint(c);
            c.restore();
        }
        (void)h;
    }

private:
    static constexpr int kWinRowH = 40;
    static constexpr int kRowH = 24;

    void layout() {
        int w = width(), h = height();
        int sw = w - 40 < 460 ? w - 40 : 460;
        seg_->bounds = Rect((w - sw) / 2, 13, sw, 30);
        quit_->bounds = Rect(w - 20 - 110, h - 44, 110, 30);
        clamp_scroll();
        invalidate();
    }

    int list_top() const { return kTabHeaderH + 34; }
    int list_bottom() const { return height() - 56; }

    void clamp_scroll() {
        int content = content_height();
        int view = list_bottom() - list_top();
        int max = content - view;
        if (max < 0) max = 0;
        if (scroll_ > max) scroll_ = max;
        if (scroll_ < 0) scroll_ = 0;
    }

    int content_height() const {
        switch (tab_) {
        case Tab::Windows: return (int)wm::windows().size() * kWinRowH;
        case Tab::Modules: return (int)dl_modules().size() * kRowH;
        case Tab::Devices: return device_rows() * kRowH;
        default: return 0;
        }
    }

    int device_rows() const {
        int rows = 4 /* section headers */ + 6 /* net */ + 1;
        for (int i = 0; i < ata_drive_count(); i++) { AtaDrive* d = ata_drive(i); if (d && d->present) rows++; }
        rows += pci_count();
        return rows + 4;
    }

    void sample() {
        push_sample(fps_, fps_n_, wm::fps());
        uint64_t tot = heap_total(), used = heap_used();
        push_sample(mem_, mem_n_, tot ? (float)(used * 100.0 / (double)tot) : 0.0f);
    }

    Widget* widget_at(int x, int y) {
        for (size_t i = widgets().size(); i > 0; i--) {
            Widget* w = widgets()[i - 1];
            if (w->visible && w->bounds.contains(x, y)) return w;
        }
        return nullptr;
    }

    void quit_selected() {
        if (!sel_win_) return;
        const Vector<Window*>& ws = wm::windows();
        for (size_t i = 0; i < ws.size(); i++) {
            if (ws[i] == sel_win_) {
                Window* w = sel_win_;
                sel_win_ = nullptr;
                w->close();
                invalidate();
                return;
            }
        }
        sel_win_ = nullptr;
    }

    // ---------------------------------------------------------- CPU
    void paint_cpu(Canvas& c) {
        const Theme& t = theme();
        int w = width(), h = height();
        float fps = wm::fps();
        char fps_s[24];
        snprintf(fps_s, sizeof(fps_s), "%.0f", fps);
        c.draw_text_in(Rect(20, kTabHeaderH + 12, 200, 16), "Frame rate", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(20, kTabHeaderH + 28, 160, 40), fps_s, t.text, 34, Font::Bold);
        char ms[32];
        snprintf(ms, sizeof(ms), "%.1f ms per frame", fps > 0.5f ? 1000.0f / fps : 0.0f);
        c.draw_text_in(Rect(92, kTabHeaderH + 46, 200, 20), ms, t.text_secondary, 12, Font::Regular);

        int gy = kTabHeaderH + 84;
        int gh = h - gy - 150;
        if (gh < 90) gh = 90;
        Rect card(20, gy, w - 40, gh + 24);
        draw_card(c, card);
        draw_graph(c, Rect(card.x + 14, card.y + 12, card.w - 28, gh), fps_, fps_n_, kmax(60.0f, max_of(fps_, fps_n_)), t.accent);

        int cy = gy + gh + 24 + 14;
        int cw = (w - 40 - 24) / 3;
        char up[32], frames[32];
        uint64_t s = timer_ms() / 1000;
        snprintf(up, sizeof(up), "%lluh %02llum %02llus", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
        snprintf(frames, sizeof(frames), "%llu", (unsigned long long)wm::frame_count());
        stat(c, Rect(20, cy, cw, 66), "Uptime", up);
        stat(c, Rect(20 + cw + 12, cy, cw, 66), "Frames rendered", frames);
        char ver[48];
        snprintf(ver, sizeof(ver), "%s.%d %s", KLEIN_VERSION, system_build_number(), KLEIN_CODENAME);
        stat(c, Rect(20 + 2 * (cw + 12), cy, cw, 66), "System", ver);

        draw_card(c, Rect(20, cy + 78, w - 40, 50));
        c.draw_text_in(Rect(34, cy + 78, 110, 50), "Processor", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(136, cy + 78, w - 180, 50), cpu_brand(), t.text, 13, Font::Medium);
    }

    static float max_of(const float* v, int n) {
        float m = 0;
        for (int i = 0; i < n; i++) if (v[i] > m) m = v[i];
        return m;
    }

    // ---------------------------------------------------------- Memory
    void paint_memory(Canvas& c) {
        const Theme& t = theme();
        int w = width(), h = height();
        size_t tot = heap_total(), used = heap_used();
        float frac = tot ? (float)used / (float)tot : 0;
        char pct[16], used_s[32], free_s[32], tot_s[32], mem_s[32];
        snprintf(pct, sizeof(pct), "%.0f%%", frac * 100);
        fmt_size(used_s, sizeof(used_s), used);
        fmt_size(free_s, sizeof(free_s), tot > used ? tot - used : 0);
        fmt_size(tot_s, sizeof(tot_s), tot);
        fmt_size(mem_s, sizeof(mem_s), boot_info().mem_total);

        // ring chart
        Rect card(20, kTabHeaderH + 14, 230, 214);
        draw_card(c, card);
        float cx = card.x + card.w / 2.0f, cy = card.y + 110;
        float ro = 74, ri = 56;
        c.fill_arc(cx, cy, ro, ri, 0, 6.28f, with_alpha(t.text, t.dark ? 36 : 28));
        float a1 = -1.5708f + 6.28318f * frac;
        if (frac > 0.001f) c.fill_arc(cx, cy, ro, ri, -1.5708f, a1, hex(0xFF6AA8));
        c.draw_text_in(Rect(card.x, cy - 16, card.w, 22), pct, t.text, 22, Font::Bold, Align::Center);
        c.draw_text_in(Rect(card.x, cy + 8, card.w, 16), "heap in use", t.text_secondary, 11, Font::Regular, Align::Center);

        // legend
        int lx = 276;
        const char* keys[] = {"Used", "Free", "Heap total", "Physical RAM"};
        char vals[4][40];
        snprintf(vals[0], 40, "%s", used_s);
        snprintf(vals[1], 40, "%s", free_s);
        snprintf(vals[2], 40, "%s", tot_s);
        snprintf(vals[3], 40, "%s", mem_s);
        for (int i = 0; i < 4; i++) {
            int y = kTabHeaderH + 22 + i * 46;
            c.fill_round_rect(R(lx, y + 9, 10, 10), 3, i == 0 ? hex(0xFF6AA8) : i == 1 ? with_alpha(t.text, 60) : hex(0x6AA8FF));
            c.draw_text_in(Rect(lx + 22, y, 200, 28), keys[i], t.text_secondary, 13, Font::Medium);
            c.draw_text_in(Rect(lx + 22, y + 20, w - lx - 50, 24), vals[i], t.text, 17, Font::Bold);
        }

        // history
        int gy = kTabHeaderH + 250;
        int gh = h - gy - 40;
        if (gh < 80) gh = 80;
        Rect hist(20, gy, w - 40, gh + 24);
        draw_card(c, hist);
        c.draw_text_in(Rect(hist.x + 14, hist.y + 8, 300, 16), "Heap use over the last minute", t.text_secondary, 12, Font::Medium);
        draw_graph(c, Rect(hist.x + 14, hist.y + 28, hist.w - 28, gh - 8), mem_, mem_n_, 100.0f, hex(0xFF6AA8));
    }

    // ---------------------------------------------------------- Windows
    void paint_windows(Canvas& c) {
        const Theme& t = theme();
        int w = width();
        c.draw_text_in(Rect(20, kTabHeaderH + 10, w - 40, 18),
                       "Open windows, front-most last. Select one and press Quit to close it.", t.text_secondary, 12);
        const Vector<Window*>& ws = wm::windows();
        int top = list_top();
        int bottom = list_bottom();
        Rect area(20, top, w - 40, bottom - top);
        draw_card(c, area);
        c.save();
        c.clip_to(area);
        bool any_selected = false;
        for (size_t i = 0; i < ws.size(); i++) {
            Window* win = ws[i];
            int y = top + (int)i * kWinRowH - scroll_;
            if (y + kWinRowH < top || y > bottom) continue;
            bool sel = win == sel_win_;
            if (sel) any_selected = true;
            Rect row(area.x + 6, y + 3, area.w - 12, kWinRowH - 6);
            if (sel) c.fill_round_rect(row, 8, t.accent);
            Color fg = sel ? t.accent_text : t.text;
            Color sub = sel ? t.accent_text : t.text_secondary;
            // app id as a pill
            const char* id = win->app_id()[0] ? win->app_id() : "-";
            int pw = text_width(id, 12, Font::Medium) + 16;
            Rect pill(row.x + 10, row.y + 8, pw, row.h - 16);
            c.fill_round_rect(pill, pill.h / 2.0f, sel ? rgba(255, 255, 255, 60) : with_alpha(t.accent, t.dark ? 70 : 50));
            c.draw_text_in(pill, id, sel ? t.accent_text : t.text, 12, Font::Medium, Align::Center);
            c.draw_text_in(Rect(pill.right() + 14, row.y, row.w - pw - 170, row.h), win->title(), fg, 14, Font::Medium);
            c.draw_text_in(Rect(row.right() - 150, row.y, 140, row.h), win->minimized() ? "minimized" : (win->focused() ? "front" : ""), sub, 12,
                           Font::Regular, Align::Right);
        }
        if (ws.empty()) c.draw_text_in(area.inset(16), "No windows are open.", t.text_secondary, 14);
        c.restore();
        if (!any_selected) sel_win_ = nullptr;
        quit_->visible = true;
        quit_->enabled = sel_win_ != nullptr;
    }

    // ---------------------------------------------------------- Modules
    void paint_modules(Canvas& c) {
        const Theme& t = theme();
        int w = width();
        const Vector<DlModule*>& mods = dl_modules();
        Rect area(20, kTabHeaderH + 12, w - 40, list_bottom() - kTabHeaderH - 12 + 40);
        draw_card(c, area);
        int nx = area.x + 14, sx = area.x + 230, rx = sx + 90, ix = rx + 80, px = ix + 80;
        int hy = area.y + 8;
        c.draw_text_in(Rect(nx, hy, 200, 18), "Module", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(sx, hy, 80, 18), "Size", t.text_secondary, 12, Font::Medium, Align::Right);
        c.draw_text_in(Rect(rx, hy, 70, 18), "Relocs", t.text_secondary, 12, Font::Medium, Align::Right);
        c.draw_text_in(Rect(ix, hy, 70, 18), "Imports", t.text_secondary, 12, Font::Medium, Align::Right);
        c.draw_text_in(Rect(px, hy, area.right() - px - 14, 18), "Path", t.text_secondary, 12, Font::Medium);
        c.fill_rect(Rect(area.x + 8, area.y + 30, area.w - 16, 1), t.separator);

        int top = area.y + 36;
        int bottom = area.bottom() - 10;
        c.save();
        c.clip_to(Rect(area.x, top, area.w, bottom - top));
        for (size_t i = 0; i < mods.size(); i++) {
            DlModule* m = mods[i];
            int y = top + (int)i * kRowH - scroll_;
            if (y + kRowH < top || y > bottom) continue;
            if (i % 2 == 1) c.fill_rect(Rect(area.x + 8, y, area.w - 16, kRowH), t.dark ? rgba(255, 255, 255, 8) : rgba(0, 0, 0, 6));
            char size[32];
            fmt_size(size, sizeof(size), m->file_size);
            char rel[16], imp[16];
            snprintf(rel, sizeof(rel), "%d", m->relocations);
            snprintf(imp, sizeof(imp), "%d", m->imports);
            c.draw_text_in(Rect(nx, y, sx - nx - 10, kRowH), m->name.c_str(), t.text, 13, Font::Medium);
            c.draw_text_in(Rect(sx, y, 80, kRowH), size, t.text_secondary, 12, Font::Regular, Align::Right);
            c.draw_text_in(Rect(rx, y, 70, kRowH), rel, t.text_secondary, 12, Font::Regular, Align::Right);
            c.draw_text_in(Rect(ix, y, 70, kRowH), imp, t.text_secondary, 12, Font::Regular, Align::Right);
            c.draw_text_in(Rect(px, y, area.right() - px - 14, kRowH), m->path.c_str(), t.text_tertiary, 12, Font::Regular);
        }
        c.restore();
        size_t total = 0;
        for (size_t i = 0; i < mods.size(); i++) total += mods[i]->file_size;
        char tot[32];
        fmt_size(tot, sizeof(tot), total);
        String foot = String::format("%zu modules, %s total, %zu kernel symbols", mods.size(), tot, kernel_symbol_count());
        c.draw_text_in(Rect(20, area.bottom() + 8, w - 40, 18), foot.c_str(), t.text_secondary, 12, Font::Regular);
    }

    // ---------------------------------------------------------- Devices
    void paint_devices(Canvas& c) {
        const Theme& t = theme();
        int w = width();
        Rect area(20, kTabHeaderH + 12, w - 40, list_bottom() - kTabHeaderH - 12 + 40);
        draw_card(c, area);
        int top = area.y + 14 - scroll_;
        int bottom = area.bottom() - 6;
        c.save();
        c.clip_to(Rect(area.x, area.y, area.w, area.h));
        int x = area.x + 16, right = area.right() - 16;
        int y = top;
        auto section = [&](const char* title) {
            c.draw_text_in(Rect(x, y, right - x, kRowH), title, t.accent, 12, Font::Bold);
            y += kRowH;
        };
        auto line = [&](const char* k, const char* v) {
            if (y + kRowH >= area.y && y <= bottom) {
                c.draw_text_in(Rect(x + 14, y, 130, kRowH), k, t.text_secondary, 12, Font::Medium);
                c.draw_text_in(Rect(x + 150, y, right - x - 150, kRowH), v, t.text, 12, Font::Regular);
            }
            y += kRowH;
        };

        section("NETWORK");
        net::Status st = net::status();
        char ip[16], gw[16], dns[16], mac[32], rx[48], tx[48];
        net::ip_to_string(st.ip, ip);
        net::ip_to_string(st.gateway, gw);
        net::ip_to_string(st.dns, dns);
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
        char rxb[32], txb[32];
        fmt_size(rxb, sizeof(rxb), st.rx_bytes);
        fmt_size(txb, sizeof(txb), st.tx_bytes);
        snprintf(rx, sizeof(rx), "%llu packets, %s", (unsigned long long)st.rx_packets, rxb);
        snprintf(tx, sizeof(tx), "%llu packets, %s", (unsigned long long)st.tx_packets, txb);
        line("Adapter", net::nic_name());
        line("Link", st.link ? (st.configured ? "up, configured" : "up, no address") : "down");
        line("Address", st.configured ? ip : "-");
        line("Gateway / DNS", st.configured ? (String(gw) + "  /  " + dns).c_str() : "-");
        line("MAC", mac);
        line("Received", rx);
        line("Sent", tx);
        (void)tx;
        y += 6;

        section("STORAGE (ATA)");
        bool any = false;
        for (int i = 0; i < ata_drive_count(); i++) {
            AtaDrive* d = ata_drive(i);
            if (!d || !d->present) continue;
            any = true;
            char size[32];
            fmt_size(size, sizeof(size), d->sectors * 512);
            String v = String::format("%s  %s  %s", d->model, size, d->atapi ? "(ATAPI)" : "");
            line(i == 0 ? "Primary master" : i == 1 ? "Primary slave" : i == 2 ? "Secondary master" : "Secondary slave", v.c_str());
        }
        if (!any) line("Drives", "none detected");
        y += 6;

        section("PCI DEVICES");
        int n = pci_count();
        for (int i = 0; i < n; i++) {
            const PciDevice* d = pci_get(i);
            if (!d) continue;
            if (y + kRowH >= area.y && y <= bottom) {
                char loc[24], ids[24];
                snprintf(loc, sizeof(loc), "%02x:%02x.%x", d->bus, d->slot, d->func);
                snprintf(ids, sizeof(ids), "%04x:%04x", d->vendor, d->device);
                c.draw_text_in(Rect(x + 14, y, 80, kRowH), loc, t.text_tertiary, 12, Font::Regular);
                c.draw_text_in(Rect(x + 96, y, 86, kRowH), ids, t.text_tertiary, 12, Font::Regular);
                c.draw_text_in(Rect(x + 190, y, right - x - 190, kRowH), pci_class_name(d->class_code, d->subclass), t.text, 12, Font::Medium);
            }
            y += kRowH;
        }
        c.restore();
        int content = y - top + 14;
        int view = area.h;
        if (content > view) {
            int max = content - view;
            int bar_h = kmax(30, view * view / content);
            int bar_y = area.y + (view - bar_h) * (scroll_ > max ? max : scroll_) / (max > 0 ? max : 1);
            c.fill_round_rect(Rect(area.right() - 7, bar_y + 2, 5, bar_h - 4), 2.5f, t.dark ? rgba(255, 255, 255, 60) : rgba(0, 0, 0, 60));
        }
    }

    // ---------------------------------------------------------- state
    SegmentedControl* seg_ = nullptr;
    Button* quit_ = nullptr;
    Tab tab_ = Tab::Cpu;
    int scroll_ = 0;
    Window* sel_win_ = nullptr;
    uint64_t last_ms_ = 0;
    float fps_[kHist] = {};
    int fps_n_ = 0;
    float mem_[kHist] = {};
    int mem_n_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0xFF8A5C), hex(0xD93A2B));
    // pulse line
    float u = r.w / 96.0f, x = (float)r.x, y = (float)r.y;
    PointF pts[] = {{16, 52}, {32, 52}, {39, 30}, {50, 70}, {58, 42}, {63, 52}, {80, 52}};
    for (int i = 0; i + 1 < 7; i++)
        c.draw_line(x + pts[i].x * u, y + pts[i].y * u, x + pts[i + 1].x * u, y + pts[i + 1].y * u, hex(0xFFFFFF), 4.5f * u);
    c.fill_circle(x + 80 * u, y + 52 * u, 3.5f * u, hex(0xFFFFFF));
}

Window* create() { return new ActivityWindow(); }

AppRegistrar reg({"activity", "Activity", icon, create, "System", true, false, 100, "Frame rate, memory, windows, modules and devices"});

}  // namespace
