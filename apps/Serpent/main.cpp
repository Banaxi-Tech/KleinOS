// Serpent: a snake on a grid. Arrows/WASD steer, Space pauses.
// Body segments are drawn as rounded, glossy circles with a color gradient
// from the head to the tail, and move smoothly between grid cells.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "gfx/canvas.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "drivers/power.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "lib/math.h"
#include "lib/kstring.h"

namespace {

constexpr int kCols = 26, kRows = 18, kCell = 20;
constexpr int kGX = 10, kGY = 62;              // grid origin in content space
constexpr int kPanelX = 4, kPanelY = 56, kPanelW = 532, kPanelH = 372;
constexpr int kMaxLen = kCols * kRows;

const Color kHeadCol = hex(0xA6FFBE);
const Color kTailCol = hex(0x17704A);
const Color kFoodCol = hex(0xFF5A5F);

// ---- best score: <home>/Library/Games/serpent.txt
String best_path() {
    String dir = String("/Users/") + session_user() + "/Library/Games";
    vfs::mkdir(dir.c_str());
    return dir + "/serpent.txt";
}
int load_best() {
    String s;
    if (!vfs::read_text(best_path().c_str(), s)) return 0;
    return atoi(s.c_str());
}
void save_best(int v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d\n", v);
    vfs::write_text(best_path().c_str(), buf);
}

void pill(Canvas& c, Rect r, const char* label, const char* value) {
    const Theme& t = theme();
    c.fill_round_rect(r, r.h / 2.0f, t.dark ? rgba(255, 255, 255, 26) : rgba(0, 0, 0, 14));
    c.draw_text_in(Rect(r.x + 14, r.y, r.w / 2, r.h), label, t.text_secondary, 12, Font::Medium);
    c.draw_text_in(Rect(r.x + r.w / 2, r.y, r.w / 2 - 14, r.h), value, t.text, 15, Font::Mono, Align::Right);
}

class SerpentWindow : public Window {
public:
    SerpentWindow() : Window("Serpent", 540, 470, WF_NO_ZOOM) {
        auto* nb = (Button*)add(new Button(Rect(390, 12, 140, 32), "New Game", Button::Primary));
        nb->on_click = [this] {
            new_game();
            invalidate();
        };
        best_ = load_best();
        new_game();
        set_ticking(true);
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        draw_status(c);
        draw_board(c);
        const Theme& t = theme();
        c.draw_text_in(Rect(0, 436, width(), 20), "Arrows or WASD to steer   ·   Space to pause",
                       t.text_secondary, 13, Font::Regular, Align::Center);
    }

    void on_key(const KeyEvent& e) override {
        int dx = 0, dy = 0;
        bool pause = false, restart = false;
        if (e.key == Key::Up) dy = -1;
        else if (e.key == Key::Down) dy = 1;
        else if (e.key == Key::Left) dx = -1;
        else if (e.key == Key::Right) dx = 1;
        else if (e.key == Key::Space) pause = true;
        else if (e.key == Key::Char) {
            uint32_t ch = e.ch;
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch == 'w') dy = -1;
            else if (ch == 's') dy = 1;
            else if (ch == 'a') dx = -1;
            else if (ch == 'd') dx = 1;
            else if (ch == 'p') pause = true;
            else if (ch == 'r') restart = true;
        }
        if (restart) {
            new_game();
            invalidate();
            return;
        }
        if (pause) {
            if (state_ == Over) new_game();
            else if (state_ == Ready) start_running();
            else toggle_pause();
            invalidate();
            return;
        }
        if (dx || dy) turn(dx, dy);
    }

    void on_tick(uint64_t now) override {
        now_ = now;
        if (state_ == Running && since(last_step_) >= (uint64_t)interval()) step();
        if (state_ == Running || state_ == Ready || (state_ == Over && since(die_at_) < 400)) invalidate();
    }

private:
    enum State : uint8_t { Ready, Running, Paused, Over };

    uint64_t since(uint64_t t0) const { return now_ > t0 ? now_ - t0 : 0; }
    int level() const { return score_ / 5 + 1; }
    int interval() const {
        int ms = 150 - (level() - 1) * 9;
        return ms < 60 ? 60 : ms;
    }

    void new_game() {
        len_ = 4;
        for (int i = 0; i < len_; i++) {
            bx_[i] = px_[i] = kCols / 2 - i;
            by_[i] = py_[i] = kRows / 2;
        }
        dx_ = ndx_ = 1;
        dy_ = ndy_ = 0;
        score_ = 0;
        state_ = Ready;
        spawn_food();
    }

    bool on_snake(int x, int y, int upto) const {
        for (int i = 0; i < upto; i++)
            if (bx_[i] == x && by_[i] == y) return true;
        return false;
    }

    void spawn_food() {
        for (int tries = 0; tries < 4000; tries++) {
            int x = (int)(cpu_random64() % kCols), y = (int)(cpu_random64() % kRows);
            if (!on_snake(x, y, len_)) {
                fx_ = x;
                fy_ = y;
                return;
            }
        }
        for (int y = 0; y < kRows; y++)
            for (int x = 0; x < kCols; x++)
                if (!on_snake(x, y, len_)) {
                    fx_ = x;
                    fy_ = y;
                    return;
                }
    }

    void start_running() {
        state_ = Running;
        last_step_ = timer_ms();
        now_ = last_step_;
    }

    void toggle_pause() {
        if (state_ == Running) {
            state_ = Paused;
            paused_at_ = timer_ms();
        } else if (state_ == Paused) {
            state_ = Running;
            last_step_ += timer_ms() - paused_at_;   // keep the step progress where it was
        }
    }

    void turn(int dx, int dy) {
        if (state_ == Over || state_ == Paused) return;
        if (dx == -dx_ && dy == -dy_) return;      // no reversing into the body
        ndx_ = dx;
        ndy_ = dy;
        if (state_ == Ready) start_running();
        invalidate();
    }

    void step() {
        dx_ = ndx_;
        dy_ = ndy_;
        int nx = bx_[0] + dx_, ny = by_[0] + dy_;
        if (nx < 0 || ny < 0 || nx >= kCols || ny >= kRows) return die();
        bool grow = nx == fx_ && ny == fy_;
        // the tail vacates its cell on this step unless the snake grows
        if (on_snake(nx, ny, grow ? len_ : len_ - 1)) return die();

        for (int i = 0; i < len_; i++) {
            px_[i] = bx_[i];
            py_[i] = by_[i];
        }
        if (grow && len_ < kMaxLen) {
            bx_[len_] = px_[len_] = bx_[len_ - 1];
            by_[len_] = py_[len_] = by_[len_ - 1];
            len_++;
        }
        for (int i = len_ - 1; i > 0; i--) {
            bx_[i] = bx_[i - 1];
            by_[i] = by_[i - 1];
        }
        bx_[0] = nx;
        by_[0] = ny;
        last_step_ = now_;

        if (grow) {
            score_++;
            if (score_ > best_) best_ = score_;
            speaker_beep(880, 25);
            spawn_food();
        }
    }

    void die() {
        state_ = Over;
        die_at_ = now_;
        speaker_beep(160, 60);
        save_best(best_);
        invalidate();
    }

    // Progress of the current step, 0..1 (frozen while paused).
    float progress() const {
        if (state_ != Running && state_ != Paused) return 1.0f;
        uint64_t ref = state_ == Paused ? paused_at_ : now_;
        float p = ref > last_step_ ? (float)(ref - last_step_) / (float)interval() : 0.0f;
        return p > 1.0f ? 1.0f : p;
    }

    void seg_center(int i, float p, float& x, float& y) const {
        float ex = px_[i] + (bx_[i] - px_[i]) * p;
        float ey = py_[i] + (by_[i] - py_[i]) * p;
        x = kGX + (ex + 0.5f) * kCell;
        y = kGY + (ey + 0.5f) * kCell;
    }

    void draw_status(Canvas& c) {
        char a[16], b[16], l[16];
        snprintf(a, sizeof(a), "%d", score_);
        snprintf(b, sizeof(b), "%d", best_);
        snprintf(l, sizeof(l), "%d", level());
        pill(c, Rect(10, 12, 120, 32), "Score", a);
        pill(c, Rect(140, 12, 120, 32), "Best", b);
        pill(c, Rect(270, 12, 110, 32), "Level", l);
    }

    void draw_board(Canvas& c) {
        Rect panel(kPanelX, kPanelY, kPanelW, kPanelH);
        c.draw_shadow(panel, 16, 14, rgba(0, 0, 0, 70), 0, 6);
        c.fill_gradient(panel, hex(0x173A30), hex(0x0B1915), 16);
        c.stroke_round_rect(panel, 16, rgba(255, 255, 255, 30), 1);

        for (int y = 0; y < kRows; y++)
            for (int x = 0; x < kCols; x++)
                c.fill_circle(kGX + x * kCell + kCell / 2.0f, kGY + y * kCell + kCell / 2.0f, 1.1f,
                              rgba(255, 255, 255, 22));

        float p = progress();
        draw_food(c);
        draw_snake(c, p);

        if (state_ == Over) {
            uint64_t t = since(die_at_);
            if (t < 400)
                c.fill_round_rect(panel, 16, rgba(255, 60, 60, (int)(120 * (1.0f - t / 400.0f))));
            draw_card(c, "Game over", "Score", "Space or New Game to play again", hex(0xFF8A8A));
        } else if (state_ == Paused) {
            draw_pill(c, "Paused  -  press Space to keep slithering", hex(0xFFFFFF));
        } else if (state_ == Ready) {
            draw_pill(c, "Press an arrow key to start", hex(0x9CF7B4));
        }
    }

    void draw_food(Canvas& c) {
        float x = kGX + (fx_ + 0.5f) * kCell, y = kGY + (fy_ + 0.5f) * kCell;
        float pulse = 0.5f + 0.5f * sinf((float)now_ / 160.0f);
        float r = kCell * 0.34f + pulse * 1.2f;
        c.fill_circle(x, y, r + 4.0f, rgba(255, 90, 95, 45));
        c.fill_circle(x, y, r, kFoodCol);
        c.fill_circle(x - r * 0.35f, y - r * 0.35f, r * 0.28f, rgba(255, 255, 255, 170));
        c.fill_ellipse(x + r * 0.45f, y - r * 1.05f, r * 0.42f, r * 0.2f, hex(0x46D98A));
    }

    void draw_snake(Canvas& c, float p) {
        float xs[kMaxLen], ys[kMaxLen];
        int n = len_;
        for (int i = 0; i < n; i++) seg_center(i, p, xs[i], ys[i]);
        auto radius = [n](int i) {
            float u = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
            return kCell * (0.46f - 0.16f * u);
        };
        auto color = [n](int i) {
            float u = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
            return mix(kHeadCol, kTailCol, (int)(u * 255.0f));
        };
        // joints first, so the rounded caps of the circles cover their ends
        for (int i = n - 1; i >= 1; i--)
            c.draw_line(xs[i], ys[i], xs[i - 1], ys[i - 1], color(i), radius(i) * 2.0f);
        for (int i = n - 1; i >= 0; i--) {
            float r = radius(i);
            Color col = color(i);
            c.fill_circle(xs[i], ys[i], r, col);
            c.fill_circle(xs[i] - r * 0.28f, ys[i] - r * 0.32f, r * 0.36f, rgba(255, 255, 255, 46));
        }
        // eyes on the head, looking the way it travels
        float r = radius(0);
        float fxv = (float)dx_, fyv = (float)dy_;
        float px_v = -fyv, py_v = fxv;
        float hx = xs[0] + fxv * r * 0.22f, hy = ys[0] + fyv * r * 0.22f;
        for (int s = -1; s <= 1; s += 2) {
            float ex = hx + px_v * r * 0.46f * s, ey = hy + py_v * r * 0.46f * s;
            c.fill_circle(ex, ey, r * 0.27f, hex(0xFFFFFF));
            c.fill_circle(ex + fxv * r * 0.1f, ey + fyv * r * 0.1f, r * 0.14f, hex(0x0B2A1E));
        }
    }

    // A slim message along the bottom edge, so the snake stays visible.
    void draw_pill(Canvas& c, const char* text, Color col) {
        Rect panel(kPanelX, kPanelY, kPanelW, kPanelH);
        Rect r(panel.x + (panel.w - 340) / 2, panel.y + panel.h - 52, 340, 36);
        c.fill_round_rect(r, 18, rgba(10, 22, 18, 215));
        c.stroke_round_rect(r, 18, rgba(255, 255, 255, 60), 1);
        c.draw_text_in(Rect(r.x, r.y, r.w, r.h), text, col, 14, Font::Medium, Align::Center);
    }

    void draw_card(Canvas& c, const char* title, const char* sub, const char* hint, Color col) {
        Rect panel(kPanelX, kPanelY, kPanelW, kPanelH);
        int w = 320, h = sub ? 150 : 110;
        Rect card(panel.x + (panel.w - w) / 2, panel.y + (panel.h - h) / 2, w, h);
        c.fill_round_rect(card, 22, rgba(10, 22, 18, 215));
        c.stroke_round_rect(card, 22, rgba(255, 255, 255, 60), 1);
        c.draw_text_in(Rect(card.x, card.y + 20, card.w, 36), title, col, 26, Font::Bold, Align::Center);
        int hy = card.y + 66;
        if (sub) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s %d   ·   Best %d", sub, score_, best_);
            c.draw_text_in(Rect(card.x, card.y + 62, card.w, 22), buf, rgba(255, 255, 255, 230), 15, Font::Medium,
                           Align::Center);
            hy = card.y + 100;
        }
        c.draw_text_in(Rect(card.x, hy, card.w, 20), hint, rgba(255, 255, 255, 160), 13, Font::Regular, Align::Center);
    }

    int len_ = 4;
    int bx_[kMaxLen] = {}, by_[kMaxLen] = {};     // current cells, head first
    int px_[kMaxLen] = {}, py_[kMaxLen] = {};     // cells before the last step
    int dx_ = 1, dy_ = 0, ndx_ = 1, ndy_ = 0;
    int fx_ = 0, fy_ = 0;
    int score_ = 0, best_ = 0;
    State state_ = Ready;
    uint64_t now_ = 0, last_step_ = 0, paused_at_ = 0, die_at_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x6CD98A), hex(0x1C7A4D));
    const int N = 12;
    for (int i = 0; i < N; i++) {                       // tail (i=0) to head (i=N-1)
        float t = (float)i / (N - 1);
        float x = r.x + r.w * (0.2f + 0.52f * t);
        float y = r.y + r.h * (0.52f + 0.17f * sinf(t * 6.2832f * 0.8f));
        float rad = r.w * (0.05f + 0.07f * t);
        c.fill_circle(x, y, rad, mix(hex(0x0E4B30), hex(0xF2FFF2), (int)(t * 255)));
    }
    float ax = r.x + r.w * 0.82f, ay = r.y + r.h * 0.2f, ar = r.w * 0.1f;
    c.fill_circle(ax, ay, ar, hex(0xFF5A5F));
    c.fill_circle(ax - ar * 0.35f, ay - ar * 0.35f, ar * 0.3f, rgba(255, 255, 255, 170));
}

Window* create() { return new SerpentWindow(); }

AppRegistrar reg({"serpent", "Serpent", icon, create, "Games", true, false, 121, "Grow the snake and avoid the walls"});

}  // namespace
