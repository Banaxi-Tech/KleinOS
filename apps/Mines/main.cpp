// Mines: a minesweeper with rounded tiles and soft colors.
// Left click reveals (first click is always safe), right click flags.
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

constexpr int kMaxCells = 30 * 16;
constexpr int kSparks = 22;
constexpr int kPanelX = 20, kPanelY = 110, kPanelW = 600, kPanelH = 470;

struct Level {
    int cols, rows, mines;
    const char* name;
    const char* key;   // key in mines.txt
};
const Level kLevels[3] = {
    {9, 9, 10, "Easy", "easy"},
    {16, 16, 40, "Medium", "medium"},
    {30, 16, 99, "Hard", "hard"},
};

// Digit colors for 1..8 (index 0 unused).
const Color kNumColor[9] = {
    0, hex(0x2F7DE1), hex(0x23A06B), hex(0xE5484D), hex(0x7B4FD6),
    hex(0xB4235A), hex(0x0F8B9A), hex(0x2B2F3A), hex(0x7A8294),
};

int iabs(int v) { return v < 0 ? -v : v; }

// ---- best times (ms): <home>/Library/Games/mines.txt, one "key=ms" line per level
String best_path() {
    String dir = String("/Users/") + session_user() + "/Library/Games";
    vfs::mkdir(dir.c_str());
    return dir + "/mines.txt";
}
void load_bests(uint64_t* out) {
    for (int i = 0; i < 3; i++) out[i] = 0;
    String s;
    if (!vfs::read_text(best_path().c_str(), s)) return;
    for (int i = 0; i < 3; i++) {
        String key = String(kLevels[i].key) + "=";
        long p = s.find(key.c_str());
        if (p >= 0) out[i] = (uint64_t)strtoul(s.c_str() + p + key.size(), nullptr, 10);
    }
}
void save_bests(const uint64_t* v) {
    char buf[128];
    snprintf(buf, sizeof(buf), "easy=%llu\nmedium=%llu\nhard=%llu\n", (unsigned long long)v[0],
             (unsigned long long)v[1], (unsigned long long)v[2]);
    vfs::write_text(best_path().c_str(), buf);
}

void fmt_time(char* buf, size_t n, uint64_t ms) {
    uint64_t s = ms / 1000;
    if (s > 5999) s = 5999;
    snprintf(buf, n, "%02llu:%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
}

struct Cell {
    uint8_t mine = 0, revealed = 0, flagged = 0, adj = 0;
};

// ---- drawing helpers
void hidden_tile(Canvas& c, Rect r, float rad) {
    c.fill_round_rect(Rect(r.x, r.y + 2, r.w, r.h), rad, hex(0xA9B6CB));
    c.fill_gradient(r, hex(0xFFFFFF), hex(0xE1E8F3), rad);
    c.stroke_round_rect(r, rad, rgba(255, 255, 255, 200), 1);
}

void plate(Canvas& c, Rect r, float rad, Color fill) {
    c.fill_round_rect(r, rad, fill);
    c.stroke_round_rect(r, rad, rgba(120, 135, 160, 40), 1);
}

void draw_mine(Canvas& c, Rect r, Color body) {
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    float R = r.w * 0.2f;
    float w = r.w * 0.08f;
    for (int k = 0; k < 8; k++) {
        float a = k * 0.7853982f;
        c.draw_line(cx, cy, cx + cosf(a) * R * 1.55f, cy + sinf(a) * R * 1.55f, body, w);
    }
    c.fill_circle(cx, cy, R, body);
    c.fill_circle(cx - R * 0.32f, cy - R * 0.32f, R * 0.3f, rgba(255, 255, 255, 150));
}

void draw_flag(Canvas& c, Rect r, Color col) {
    int x = r.x + (int)(r.w * 0.36f);
    int top = r.y + (int)(r.h * 0.2f), bot = r.y + (int)(r.h * 0.8f);
    c.draw_line((float)x, (float)top, (float)x, (float)bot, hex(0x3A4150), r.w * 0.07f);
    PointF pts[3] = {
        {(float)x, (float)top},
        {x + r.w * 0.36f, top + r.h * 0.15f},
        {(float)x, top + r.h * 0.3f},
    };
    c.fill_polygon(pts, 3, col);
    c.fill_round_rect(Rect(x - r.w / 6, bot - 2, r.w / 3, 3), 1.5f, hex(0x3A4150));
}

void draw_cross(Canvas& c, Rect r, Color col) {
    float ins = r.w * 0.28f;
    float x0 = r.x + ins, y0 = r.y + ins, x1 = r.x + r.w - ins, y1 = r.y + r.h - ins;
    c.draw_line(x0, y0, x1, y1, col, r.w * 0.09f);
    c.draw_line(x1, y0, x0, y1, col, r.w * 0.09f);
}

class MinesWindow : public Window {
public:
    MinesWindow() : Window("Mines", 640, 600, WF_NO_ZOOM) {
        seg_ = (SegmentedControl*)add(new SegmentedControl(Rect(140, 14, 360, 30), {"Easy", "Medium", "Hard"}));
        seg_->selected = level_;
        seg_->on_change = [this](int i) {
            level_ = i;
            new_game();
            invalidate();
        };
        auto* nb = (Button*)add(new Button(Rect(490, 61, 130, 32), "New Game", Button::Primary));
        nb->on_click = [this] {
            new_game();
            invalidate();
        };
        load_bests(best_);
        new_game();
        set_ticking(true);
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        draw_status(c);
        draw_board(c);
    }

    void on_mouse_down(int x, int y, int button) override {
        const Level& L = kLevels[level_];
        int cell = cell_size();
        int gx = grid_x(cell), gy = grid_y(cell);
        if (x >= gx && y >= gy && x < gx + L.cols * cell && y < gy + L.rows * cell) {
            int col = (x - gx) / cell, row = (y - gy) / cell;
            int i = row * L.cols + col;
            if (button == MB_RIGHT) toggle_flag(i);
            else if (button == MB_LEFT) click(i);
            invalidate();
            return;
        }
        Window::on_mouse_down(x, y, button);
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Char && (e.ch == 'n' || e.ch == 'N')) {
            new_game();
            invalidate();
            return;
        }
        Window::on_key(e);
    }

    void on_tick(uint64_t now) override {
        now_ = now;
        if (state_ == Playing) {
            uint64_t s = elapsed() / 1000;
            if (s != last_sec_) {
                last_sec_ = s;
                invalidate();
            }
        } else if (state_ == Lost && since(lost_at_) < 2000) {
            invalidate();
        }
    }

private:
    enum State : uint8_t { Ready, Playing, Won, Lost };

    uint64_t since(uint64_t t0) const { return now_ > t0 ? now_ - t0 : 0; }
    uint64_t elapsed() const { return state_ == Playing ? since(start_) : frozen_; }

    int cell_size() const {
        const Level& L = kLevels[level_];
        int cw = (kPanelW - 32) / L.cols;
        int ch = (kPanelH - 32) / L.rows;
        int s = cw < ch ? cw : ch;
        return s > 40 ? 40 : s;
    }
    int grid_x(int cell) const { return kPanelX + (kPanelW - kLevels[level_].cols * cell) / 2; }
    int grid_y(int cell) const { return kPanelY + (kPanelH - kLevels[level_].rows * cell) / 2; }

    int neighbors(int i, int* out) const {
        const Level& L = kLevels[level_];
        int r = i / L.cols, c = i % L.cols, k = 0;
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) {
                if (!dr && !dc) continue;
                int rr = r + dr, cc = c + dc;
                if (rr < 0 || cc < 0 || rr >= L.rows || cc >= L.cols) continue;
                out[k++] = rr * L.cols + cc;
            }
        return k;
    }

    int flags_placed() const {
        int n = kLevels[level_].cols * kLevels[level_].rows, f = 0;
        for (int i = 0; i < n; i++) f += cells_[i].flagged;
        return f;
    }

    void new_game() {
        for (int i = 0; i < kMaxCells; i++) cells_[i] = Cell{};
        state_ = Ready;
        revealed_ = 0;
        start_ = frozen_ = lost_at_ = last_sec_ = 0;
        lost_idx_ = -1;
    }

    void place_mines(int safe) {
        const Level& L = kLevels[level_];
        int n = L.cols * L.rows;
        int sr = safe / L.cols, sc = safe % L.cols;
        int placed = 0;
        while (placed < L.mines) {
            int i = (int)(cpu_random64() % (uint64_t)n);
            if (cells_[i].mine) continue;
            int r = i / L.cols, c = i % L.cols;
            if (iabs(r - sr) <= 1 && iabs(c - sc) <= 1) continue;   // keep the first click's neighborhood clear
            cells_[i].mine = 1;
            placed++;
        }
        int nb[8];
        for (int i = 0; i < n; i++) {
            int k = neighbors(i, nb), a = 0;
            for (int j = 0; j < k; j++) a += cells_[nb[j]].mine;
            cells_[i].adj = (uint8_t)a;
        }
    }

    void click(int i) {
        if (state_ == Won || state_ == Lost) return;
        Cell& c = cells_[i];
        if (c.revealed) {
            // chord: a satisfied number opens its unflagged neighbors
            if (!c.adj) return;
            int nb[8];
            int k = neighbors(i, nb), f = 0;
            for (int j = 0; j < k; j++) f += cells_[nb[j]].flagged;
            if (f != c.adj) return;
            for (int j = 0; j < k && state_ != Lost; j++) {
                Cell& n = cells_[nb[j]];
                if (!n.flagged && !n.revealed) reveal(nb[j]);
            }
            return;
        }
        if (c.flagged) return;
        reveal(i);
    }

    void reveal(int i) {
        if (state_ == Won || state_ == Lost) return;
        if (state_ == Ready) {
            place_mines(i);
            state_ = Playing;
            start_ = timer_ms();
            now_ = start_;
            last_sec_ = 0;
        }
        Cell& c = cells_[i];
        if (c.revealed || c.flagged) return;
        if (c.mine) {
            lose(i);
            return;
        }
        flood(i);
        const Level& L = kLevels[level_];
        if (revealed_ == L.cols * L.rows - L.mines) win();
        else speaker_beep(660, 20);
    }

    void flood(int start) {
        int q[kMaxCells];
        int h = 0, t = 0;
        cells_[start].revealed = 1;
        revealed_++;
        q[t++] = start;
        while (h < t) {
            int i = q[h++];
            if (cells_[i].adj) continue;
            int nb[8];
            int k = neighbors(i, nb);
            for (int j = 0; j < k; j++) {
                Cell& cj = cells_[nb[j]];
                if (cj.revealed || cj.flagged || cj.mine) continue;
                cj.revealed = 1;
                revealed_++;
                q[t++] = nb[j];
            }
        }
    }

    void toggle_flag(int i) {
        if (state_ != Ready && state_ != Playing) return;
        Cell& c = cells_[i];
        if (c.revealed) return;
        c.flagged = !c.flagged;
    }

    void lose(int i) {
        state_ = Lost;
        lost_idx_ = i;
        lost_at_ = timer_ms();
        now_ = lost_at_;
        frozen_ = lost_at_ - start_;
        cells_[i].revealed = 1;
        speaker_beep(90, 60);
        for (int k = 0; k < kSparks; k++) {
            float a = (float)(cpu_random64() % 6283) / 1000.0f;
            float sp = 150.0f + (float)(cpu_random64() % 230);
            spark_vx_[k] = cosf(a) * sp;
            spark_vy_[k] = sinf(a) * sp - 140.0f;
        }
    }

    void win() {
        now_ = timer_ms();
        state_ = Won;
        frozen_ = since(start_);
        speaker_beep(988, 50);
        const Level& L = kLevels[level_];
        for (int i = 0; i < L.cols * L.rows; i++)
            if (cells_[i].mine) cells_[i].flagged = 1;
        if (best_[level_] == 0 || frozen_ < best_[level_]) {
            best_[level_] = frozen_;
            save_bests(best_);
        }
    }

    uint64_t mine_delay(int i) const {
        const Level& L = kLevels[level_];
        int dr = iabs(i / L.cols - lost_idx_ / L.cols);
        int dc = iabs(i % L.cols - lost_idx_ % L.cols);
        return (uint64_t)(dr > dc ? dr : dc) * 45;
    }

    // ---- painting
    void draw_status(Canvas& c) {
        const Theme& t = theme();
        Color pill = t.dark ? rgba(255, 255, 255, 26) : rgba(0, 0, 0, 14);
        char buf[32];

        Rect timer(20, 61, 130, 32);
        c.fill_round_rect(timer, 16, pill);
        fmt_time(buf, sizeof(buf), elapsed());
        c.draw_text_in(Rect(timer.x + 14, timer.y, 50, timer.h), "Time", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(timer.x + 56, timer.y, timer.w - 70, timer.h), buf, t.text, 15, Font::Mono, Align::Right);

        Rect mines(160, 61, 130, 32);
        c.fill_round_rect(mines, 16, pill);
        int left = kLevels[level_].mines - flags_placed();
        snprintf(buf, sizeof(buf), "%d", left);
        c.draw_text_in(Rect(mines.x + 14, mines.y, 60, mines.h), "Mines", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(mines.x + 56, mines.y, mines.w - 70, mines.h), buf,
                       left < 0 ? t.destructive : t.text, 15, Font::Mono, Align::Right);

        Rect best(300, 61, 170, 32);
        c.fill_round_rect(best, 16, pill);
        char bt[16];
        if (best_[level_]) fmt_time(bt, sizeof(bt), best_[level_]);
        else snprintf(bt, sizeof(bt), "--:--");
        c.draw_text_in(Rect(best.x + 14, best.y, 90, best.h), "Best time", t.text_secondary, 12, Font::Medium);
        c.draw_text_in(Rect(best.x + 96, best.y, best.w - 110, best.h), bt, t.text, 15, Font::Mono, Align::Right);
    }

    void draw_cell(Canvas& c, Rect r, int i, uint64_t t_lost) {
        const Cell& cl = cells_[i];
        float rad = r.w * 0.26f;
        if (cl.revealed) {
            if (cl.mine) {
                plate(c, r, rad, i == lost_idx_ ? hex(0xFF6B6B) : hex(0xE3E9F2));
                draw_mine(c, r, hex(0x2B2F3A));
            } else {
                plate(c, r, rad, hex(0xF2F5FA));
                if (cl.adj) {
                    char buf[2] = {(char)('0' + cl.adj), 0};
                    c.draw_text_in(r, buf, kNumColor[cl.adj], (int)(r.h * 0.6f), Font::Bold, Align::Center);
                }
            }
            return;
        }
        if (cl.flagged) {
            hidden_tile(c, r, rad);
            if (state_ == Lost && !cl.mine) {
                draw_cross(c, r, hex(0xE5484D));
            } else {
                draw_flag(c, r, hex(0xFF5A5F));
            }
            return;
        }
        if (state_ == Lost && cl.mine && t_lost >= mine_delay(i)) {
            plate(c, r, rad, hex(0xE3E9F2));
            draw_mine(c, r, hex(0x2B2F3A));
            return;
        }
        hidden_tile(c, r, rad);
    }

    void draw_blast(Canvas& c, int cell, int gx, int gy, uint64_t t) {
        if (state_ != Lost || lost_idx_ < 0) return;
        const Level& L = kLevels[level_];
        int r = lost_idx_ / L.cols, col = lost_idx_ % L.cols;
        float cx = gx + col * cell + cell / 2.0f, cy = gy + r * cell + cell / 2.0f;
        if (t < 260) {
            float k = t / 260.0f;
            c.fill_circle(cx, cy, cell * (0.6f + k * 1.2f), rgba(255, 215, 150, (int)(200 * (1.0f - k))));
        }
        if (t < 700) {
            float k = t / 700.0f;
            c.stroke_circle(cx, cy, cell * 0.6f + k * cell * 4.5f, rgba(255, 150, 90, (int)(220 * (1.0f - k))), 3.0f);
        }
        if (t < 900) {
            float tf = t / 1000.0f;
            int a = (int)(255 * (1.0f - t / 900.0f));
            for (int k = 0; k < kSparks; k++) {
                float x = cx + spark_vx_[k] * tf;
                float y = cy + spark_vy_[k] * tf + 0.5f * 420.0f * tf * tf;
                c.fill_circle(x, y, 2.6f, rgba(255, 185, 110, a));
            }
        }
    }

    void draw_board(Canvas& c) {
        const Level& L = kLevels[level_];
        Rect panel(kPanelX, kPanelY, kPanelW, kPanelH);
        c.draw_shadow(panel, 20, 16, rgba(0, 0, 0, 70), 0, 8);
        c.fill_gradient(panel, hex(0xE9EEF6), hex(0xCDD7E6), 20);
        c.stroke_round_rect(panel, 20, rgba(255, 255, 255, 150), 1);

        int cell = cell_size();
        int gx = grid_x(cell), gy = grid_y(cell);
        uint64_t t = state_ == Lost ? since(lost_at_) : 0;
        int shake = 0;
        if (state_ == Lost && t < 420)
            shake = (int)(sinf((float)t * 0.09f) * 7.0f * (1.0f - (float)t / 420.0f));

        c.save();
        c.translate(shake, 0);
        int gap = cell >= 24 ? 3 : 1;
        for (int i = 0; i < L.cols * L.rows; i++) {
            int r = i / L.cols, col = i % L.cols;
            Rect tile(gx + col * cell + gap, gy + r * cell + gap, cell - 2 * gap, cell - 2 * gap);
            draw_cell(c, tile, i, t);
        }
        draw_blast(c, cell, gx, gy, t);
        c.restore();

        if (state_ == Won) draw_overlay(c, true);
        else if (state_ == Lost && t > 1100) draw_overlay(c, false);
    }

    void draw_overlay(Canvas& c, bool won) {
        Rect card(kPanelX + (kPanelW - 320) / 2, kPanelY + (kPanelH - 160) / 2, 320, 160);
        c.fill_round_rect(card, 22, rgba(16, 20, 30, 205));
        c.stroke_round_rect(card, 22, rgba(255, 255, 255, 70), 1);
        c.draw_text_in(Rect(card.x, card.y + 24, card.w, 36), won ? "Field cleared!" : "Boom!",
                       won ? hex(0x7CE9B0) : hex(0xFF8080), 28, Font::Bold, Align::Center);
        char a[16], b[16], line[64];
        fmt_time(a, sizeof(a), frozen_);
        if (best_[level_]) fmt_time(b, sizeof(b), best_[level_]);
        else snprintf(b, sizeof(b), "--:--");
        snprintf(line, sizeof(line), "Time %s     Best %s", a, b);
        c.draw_text_in(Rect(card.x, card.y + 74, card.w, 22), line, rgba(255, 255, 255, 230), 15, Font::Mono,
                       Align::Center);
        c.draw_text_in(Rect(card.x, card.y + 108, card.w, 20),
                       won ? "Press New Game for another round" : "Press New Game to try again",
                       rgba(255, 255, 255, 160), 13, Font::Regular, Align::Center);
    }

    int level_ = 0;
    State state_ = Ready;
    Cell cells_[kMaxCells] = {};
    int revealed_ = 0;
    int lost_idx_ = -1;
    uint64_t now_ = 0, start_ = 0, frozen_ = 0, lost_at_ = 0, last_sec_ = 0;
    float spark_vx_[kSparks] = {};
    float spark_vy_[kSparks] = {};
    uint64_t best_[3] = {};
    SegmentedControl* seg_ = nullptr;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x5A6B8C), hex(0x1D2638));
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.56f, R = r.w * 0.2f;
    for (int k = 0; k < 8; k++) {
        float a = k * 0.7853982f;
        c.draw_line(cx, cy, cx + cosf(a) * R * 1.6f, cy + sinf(a) * R * 1.6f, hex(0x1A1F2C), r.w * 0.065f);
    }
    c.fill_circle(cx, cy, R, hex(0x1A1F2C));
    c.fill_circle(cx - R * 0.33f, cy - R * 0.33f, R * 0.28f, rgba(255, 255, 255, 170));
    // a red flag on a shallow tile in the corner
    int fx = r.x + (int)(r.w * 0.2f), fy = r.y + (int)(r.h * 0.2f);
    c.draw_line((float)fx, (float)fy, (float)fx, (float)(fy + r.h * 0.34f), rgba(255, 255, 255, 230), r.w * 0.03f);
    PointF pts[3] = {
        {(float)fx, (float)fy},
        {fx + r.w * 0.2f, fy + r.h * 0.07f},
        {(float)fx, fy + r.h * 0.14f},
    };
    c.fill_polygon(pts, 3, hex(0xFF5A5F));
}

Window* create() { return new MinesWindow(); }

AppRegistrar reg({"mines", "Mines", icon, create, "Games", true, false, 120, "Classic mine-sweeping puzzle"});

}  // namespace
