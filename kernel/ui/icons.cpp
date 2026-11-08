// App icon base + line-art UI symbols.
#include "ui/app.h"
#include "lib/math.h"

void icon_base(Canvas& c, Rect r, Color top, Color bottom) {
    float rad = r.w * 0.225f;
    // soft contact shadow
    c.draw_shadow(r.inset(r.w / 16), rad, r.w * 0.06f, rgba(0, 0, 0, 70), 0, r.w / 24);
    c.fill_gradient(r, top, bottom, rad);
    // glossy top highlight
    Rect hl(r.x + 1, r.y + 1, r.w - 2, r.h / 2);
    c.save();
    c.clip_to(hl);
    c.fill_gradient(Rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2), rgba(255, 255, 255, 70), rgba(255, 255, 255, 0), rad - 1);
    c.restore();
    c.stroke_round_rect(r, rad, rgba(255, 255, 255, 60), 1.0f);
}

void icon_text(Canvas& c, Rect r, const char* text, Color color, float ratio, Font font) {
    int size = (int)(r.h * ratio);
    if (size < 6) size = 6;
    c.draw_text_in(r, text, color, size, font, Align::Center);
}

void draw_symbol(Canvas& c, Rect r, Symbol s, Color col, float sw) {
    float x = r.x, y = r.y, w = r.w, h = r.h;
    float cx = x + w / 2, cy = y + h / 2;
    float u = (w < h ? w : h) / 16.0f;   // design grid: 16x16
    auto L = [&](float x0, float y0, float x1, float y1) {
        c.draw_line(cx + (x0 - 8) * u, cy + (y0 - 8) * u, cx + (x1 - 8) * u, cy + (y1 - 8) * u, col, sw);
    };
    auto C = [&](float px, float py, float rr, bool fill = false) {
        if (fill) c.fill_circle(cx + (px - 8) * u, cy + (py - 8) * u, rr * u, col);
        else c.stroke_circle(cx + (px - 8) * u, cy + (py - 8) * u, rr * u, col, sw);
    };
    auto RR = [&](float x0, float y0, float x1, float y1, float rad, bool fill = false) {
        Rect rr((int)(cx + (x0 - 8) * u), (int)(cy + (y0 - 8) * u), (int)((x1 - x0) * u), (int)((y1 - y0) * u));
        if (fill) c.fill_round_rect(rr, rad * u, col);
        else c.stroke_round_rect(rr, rad * u, col, sw);
    };
    auto ARC = [&](float px, float py, float rr, float a0, float a1) {
        c.fill_arc(cx + (px - 8) * u, cy + (py - 8) * u, rr * u + sw / 2, rr * u - sw / 2, a0, a1, col);
    };
    const float PI = 3.14159265f;
    switch (s) {
    case Symbol::Close: L(4, 4, 12, 12); L(12, 4, 4, 12); break;
    case Symbol::Minus: L(3, 8, 13, 8); break;
    case Symbol::Plus: L(3, 8, 13, 8); L(8, 3, 8, 13); break;
    case Symbol::Check: L(3, 8.5f, 6.5f, 12); L(6.5f, 12, 13, 4.5f); break;
    case Symbol::ChevronLeft: L(10, 3, 5, 8); L(5, 8, 10, 13); break;
    case Symbol::ChevronRight: L(6, 3, 11, 8); L(11, 8, 6, 13); break;
    case Symbol::ChevronUp: L(3, 10, 8, 5); L(8, 5, 13, 10); break;
    case Symbol::ChevronDown: L(3, 6, 8, 11); L(8, 11, 13, 6); break;
    case Symbol::Search: C(7, 7, 4.5f); L(10.3f, 10.3f, 14, 14); break;
    case Symbol::Gear:
        C(8, 8, 2.3f);
        for (int i = 0; i < 8; i++) {
            float a = i * PI / 4;
            L(8 + cosf(a) * 4.2f, 8 + sinf(a) * 4.2f, 8 + cosf(a) * 6.5f, 8 + sinf(a) * 6.5f);
        }
        C(8, 8, 4.6f);
        break;
    case Symbol::Wifi:
        ARC(8, 13, 9, -PI * 0.78f, -PI * 0.22f);
        ARC(8, 13, 6, -PI * 0.78f, -PI * 0.22f);
        ARC(8, 13, 3, -PI * 0.78f, -PI * 0.22f);
        C(8, 13, 1.2f, true);
        break;
    case Symbol::WifiOff:
        ARC(8, 13, 9, -PI * 0.78f, -PI * 0.22f);
        ARC(8, 13, 6, -PI * 0.78f, -PI * 0.22f);
        L(3, 3, 13, 13);
        break;
    case Symbol::Battery: RR(1.5f, 5, 13.5f, 11, 2); RR(3, 6.5f, 11, 9.5f, 1, true); L(14.8f, 7, 14.8f, 9); break;
    case Symbol::Volume: {
        PointF p[] = {{cx - 6 * u, cy - 2 * u}, {cx - 3 * u, cy - 2 * u}, {cx + 1 * u, cy - 5.5f * u},
                      {cx + 1 * u, cy + 5.5f * u}, {cx - 3 * u, cy + 2 * u}, {cx - 6 * u, cy + 2 * u}};
        for (auto& q : p) { q.x -= c.origin_x() * 0; }
        c.fill_polygon(p, 6, col);
        ARC(9, 8, 3, -PI / 3, PI / 3);
        ARC(9, 8, 5.5f, -PI / 3, PI / 3);
        break;
    }
    case Symbol::Sun:
        C(8, 8, 3);
        for (int i = 0; i < 8; i++) {
            float a = i * PI / 4;
            L(8 + cosf(a) * 5, 8 + sinf(a) * 5, 8 + cosf(a) * 7, 8 + sinf(a) * 7);
        }
        break;
    case Symbol::Moon:
        C(8, 8, 6, true);
        c.fill_circle(cx + 3 * u, cy - 2.5f * u, 5.2f * u, rgba(0, 0, 0, 0));
        // carve: draw crescent by overpainting with transparent isn't possible -> use ring arcs
        break;
    case Symbol::Bell:
        ARC(8, 7, 4, PI, 2 * PI);
        L(4, 7, 4, 11); L(12, 7, 12, 11); L(2.5f, 11.5f, 13.5f, 11.5f); C(8, 13.5f, 1.2f, true);
        break;
    case Symbol::Power: ARC(8, 8.5f, 5.5f, -PI * 0.3f, PI * 1.3f); L(8, 2, 8, 8); break;
    case Symbol::Folder: RR(1.5f, 4.5f, 14.5f, 13, 1.5f); L(1.5f, 6, 6, 6); L(6, 6, 7.5f, 4.5f); break;
    case Symbol::File: RR(3.5f, 1.5f, 12.5f, 14.5f, 1.5f); L(5.5f, 6, 10.5f, 6); L(5.5f, 8.5f, 10.5f, 8.5f); L(5.5f, 11, 9, 11); break;
    case Symbol::Home: L(2, 8, 8, 2.5f); L(8, 2.5f, 14, 8); RR(4, 7, 12, 14, 1); break;
    case Symbol::Trash: L(2.5f, 4, 13.5f, 4); L(6, 4, 6.5f, 2); L(6.5f, 2, 9.5f, 2); L(9.5f, 2, 10, 4); RR(4, 4, 12, 14.5f, 1.5f); L(6.5f, 6.5f, 6.5f, 12); L(9.5f, 6.5f, 9.5f, 12); break;
    case Symbol::Refresh: ARC(8, 8, 5.5f, -PI * 0.15f, PI * 1.5f); L(13.5f, 3, 13.5f, 7); L(13.5f, 7, 9.5f, 7); break;
    case Symbol::Share: RR(3, 6, 13, 14.5f, 1.5f); L(8, 1.5f, 8, 10); L(5.5f, 4, 8, 1.5f); L(8, 1.5f, 10.5f, 4); break;
    case Symbol::Play: {
        PointF p[] = {{cx - 3.5f * u, cy - 5.5f * u}, {cx + 5.5f * u, cy}, {cx - 3.5f * u, cy + 5.5f * u}};
        c.fill_polygon(p, 3, col);
        break;
    }
    case Symbol::Pause: RR(4, 3, 6.8f, 13, 1, true); RR(9.2f, 3, 12, 13, 1, true); break;
    case Symbol::Stop: RR(4, 4, 12, 12, 1.5f, true); break;
    case Symbol::SkipForward: {
        PointF p[] = {{cx - 6 * u, cy - 4.5f * u}, {cx + 0.5f * u, cy}, {cx - 6 * u, cy + 4.5f * u}};
        PointF q[] = {{cx + 0 * u, cy - 4.5f * u}, {cx + 6.5f * u, cy}, {cx + 0 * u, cy + 4.5f * u}};
        c.fill_polygon(p, 3, col); c.fill_polygon(q, 3, col);
        break;
    }
    case Symbol::SkipBack: {
        PointF p[] = {{cx + 6 * u, cy - 4.5f * u}, {cx - 0.5f * u, cy}, {cx + 6 * u, cy + 4.5f * u}};
        PointF q[] = {{cx + 0 * u, cy - 4.5f * u}, {cx - 6.5f * u, cy}, {cx + 0 * u, cy + 4.5f * u}};
        c.fill_polygon(p, 3, col); c.fill_polygon(q, 3, col);
        break;
    }
    case Symbol::Grid: RR(2, 2, 7, 7, 1.2f); RR(9, 2, 14, 7, 1.2f); RR(2, 9, 7, 14, 1.2f); RR(9, 9, 14, 14, 1.2f); break;
    case Symbol::List: L(5, 4, 14, 4); L(5, 8, 14, 8); L(5, 12, 14, 12); C(2.5f, 4, 0.9f, true); C(2.5f, 8, 0.9f, true); C(2.5f, 12, 0.9f, true); break;
    case Symbol::Star: {
        PointF p[10];
        for (int i = 0; i < 10; i++) {
            float a = -PI / 2 + i * PI / 5;
            float rr = (i % 2 ? 2.8f : 6.8f) * u;
            p[i] = {cx + cosf(a) * rr, cy + 0.6f * u + sinf(a) * rr};
        }
        c.fill_polygon(p, 10, col);
        break;
    }
    case Symbol::Heart:
        C(5.5f, 6, 3, true); C(10.5f, 6, 3, true);
        {
            PointF p[] = {{cx - 5.4f * u, cy - 1 * u}, {cx + 5.4f * u, cy - 1 * u}, {cx, cy + 6 * u}};
            c.fill_polygon(p, 3, col);
        }
        break;
    case Symbol::Lock: RR(3, 7, 13, 14.5f, 1.8f, true); ARC(8, 7, 3.2f, PI, 2 * PI); L(4.8f, 7, 4.8f, 7.5f); L(11.2f, 7, 11.2f, 7.5f); break;
    case Symbol::User: C(8, 5.5f, 3); ARC(8, 15, 6, PI * 1.05f, PI * 1.95f); break;
    case Symbol::Download: L(8, 2, 8, 11); L(4.5f, 7.5f, 8, 11); L(8, 11, 11.5f, 7.5f); L(3, 14, 13, 14); break;
    case Symbol::Globe: C(8, 8, 6.3f); L(1.8f, 8, 14.2f, 8); c.stroke_circle(cx, cy, 6.3f * u, col, sw);
        c.fill_ellipse(cx, cy, 2.6f * u, 6.3f * u, rgba(0, 0, 0, 0));
        ARC(8, 8, 6.3f, 0, 0.01f);
        L(3, 4.5f, 13, 4.5f); L(3, 11.5f, 13, 11.5f);
        L(8, 1.7f, 8, 14.3f);
        break;
    case Symbol::Clock: C(8, 8, 6.3f); L(8, 4, 8, 8); L(8, 8, 11, 10); break;
    case Symbol::Calendar: RR(2, 3, 14, 14, 2); L(2, 6.5f, 14, 6.5f); L(5, 1.5f, 5, 4); L(11, 1.5f, 11, 4); break;
    case Symbol::Edit: L(3, 13, 4, 10); L(4, 10, 11, 3); L(11, 3, 13, 5); L(13, 5, 6, 12); L(6, 12, 3, 13); break;
    case Symbol::Info: C(8, 8, 6.3f); L(8, 7, 8, 11.5f); C(8, 4.8f, 0.9f, true); break;
    case Symbol::Warning: L(8, 2, 14.5f, 13.5f); L(14.5f, 13.5f, 1.5f, 13.5f); L(1.5f, 13.5f, 8, 2); L(8, 6, 8, 9.5f); C(8, 11.5f, 0.9f, true); break;
    case Symbol::Sidebar: RR(1.5f, 2.5f, 14.5f, 13.5f, 2); L(6, 2.5f, 6, 13.5f); break;
    case Symbol::Bluetooth: L(5, 4.5f, 11, 10.5f); L(11, 10.5f, 8, 13.5f); L(8, 13.5f, 8, 2.5f); L(8, 2.5f, 11, 5.5f); L(11, 5.5f, 5, 11.5f); break;
    case Symbol::Airplane: L(2, 9, 14, 6); L(8, 7.5f, 5, 2.5f); L(8, 7.5f, 10, 13); L(3, 6, 4, 8.5f); break;
    case Symbol::Keyboard: RR(1, 4, 15, 12.5f, 1.5f); L(4, 10, 12, 10); for (int i = 0; i < 5; i++) C(3.5f + i * 2.25f, 7, 0.6f, true); break;
    case Symbol::Display: RR(1.5f, 2.5f, 14.5f, 11, 1.5f); L(8, 11, 8, 13.5f); L(5, 13.5f, 11, 13.5f); break;
    case Symbol::Cpu: RR(4, 4, 12, 12, 1.5f); RR(6.3f, 6.3f, 9.7f, 9.7f, 0.5f, true);
        for (int i = 0; i < 3; i++) { float p = 6 + i * 2; L(p, 1.5f, p, 4); L(p, 12, p, 14.5f); L(1.5f, p, 4, p); L(12, p, 14.5f, p); }
        break;
    case Symbol::Music: L(6, 12, 6, 3); L(6, 3, 13, 1.8f); L(13, 1.8f, 13, 10.5f); C(4.3f, 12.2f, 1.9f, true); C(11.3f, 10.7f, 1.9f, true); break;
    case Symbol::Image: RR(1.5f, 2.5f, 14.5f, 13.5f, 2); C(5.5f, 6, 1.4f, true); L(2, 12, 6.5f, 8); L(6.5f, 8, 9, 10.5f); L(9, 10.5f, 11, 8.5f); L(11, 8.5f, 14, 11.5f); break;
    case Symbol::Terminal: RR(1.5f, 2.5f, 14.5f, 13.5f, 2); L(4, 6, 6.5f, 8); L(6.5f, 8, 4, 10); L(8, 10.5f, 12, 10.5f); break;
    case Symbol::Restart: ARC(8, 8.5f, 5.5f, -PI * 0.5f, PI * 1.25f); L(8, 3, 10.5f, 1.5f); L(8, 3, 10.5f, 4.8f); break;
    }
}
