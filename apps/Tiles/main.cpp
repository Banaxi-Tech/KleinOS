// Tiles: slide and merge numbered tiles on a 4x4 board until one reaches 2048.
// Slides and merges animate over 120 ms; new tiles pop in after the slide.
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

constexpr int kN = 4;
constexpr int kBoardX = 10, kBoardY = 92, kBoardW = 460, kBoardH = 460;
constexpr int kPad = 12, kGap = 12;
constexpr int kCell = (kBoardW - 2 * kPad - (kN - 1) * kGap) / kN;   // 100
constexpr uint64_t kSlideMs = 120;
constexpr int kWinTile = 2048;

// Tile palette, indexed by log2(value) - 1 (2, 4, 8, ... 4096+).
const Color kFill[12] = {
    hex(0xEEF8F4), hex(0xD4F0E4), hex(0x9EE3C9), hex(0x6CCFF6), hex(0x4AA8F0), hex(0x6F7CF5),
    hex(0xA36BF2), hex(0xE870C4), hex(0xFF7A59), hex(0xFFB347), hex(0xFFD93D), hex(0x22C3A8),
};

// ---- best score: <home>/Library/Games/tiles.txt
String best_path() {
    String dir = String("/Users/") + session_user() + "/Library/Games";
    vfs::mkdir(dir.c_str());
    return dir + "/tiles.txt";
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

int tile_index(int v) {
    int k = 0;
    while (v > 2) {
        v >>= 1;
        k++;
    }
    return k > 11 ? 11 : k;
}

void draw_tile(Canvas& c, Rect cell, int v, float scale) {
    if (scale <= 0.0f) return;
    float cx = cell.x + cell.w / 2.0f, cy = cell.y + cell.h / 2.0f;
    float hw = cell.w * scale / 2.0f, hh = cell.h * scale / 2.0f;
    Rect t((int)(cx - hw), (int)(cy - hh), (int)(hw * 2), (int)(hh * 2));
    float rad = t.w * 0.18f;
    int idx = tile_index(v);
    Color fill = kFill[idx];
    Color text = idx <= 2 ? hex(0x34554B) : (idx == 10 ? hex(0x5C4300) : hex(0xFFFFFF));
    if (v >= kWinTile) c.draw_shadow(t, rad, 16, rgba(255, 217, 61, 130), 0, 0);
    c.fill_round_rect(Rect(t.x, t.y + 5, t.w, t.h), rad, darken(fill, 75));
    c.fill_gradient(t, lighten(fill, 45), fill, rad);
    c.stroke_round_rect(t, rad, rgba(255, 255, 255, 110), 1);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d", v);
    int size = v < 100 ? 46 : v < 1000 ? 38 : v < 10000 ? 30 : 24;
    c.draw_text_in(t, buf, text, (int)(size * scale), Font::Bold, Align::Center);
}

class TilesWindow : public Window {
public:
    TilesWindow() : Window("Tiles", 480, 580, WF_NO_ZOOM) {
        auto* nb = (Button*)add(new Button(Rect(340, 28, 120, 32), "New Game", Button::Primary));
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
        draw_header(c);
        draw_board(c);
        c.draw_text_in(Rect(0, 556, width(), 20), "Arrow keys or WASD to slide", theme().text_secondary, 13,
                       Font::Regular, Align::Center);
    }

    void on_key(const KeyEvent& e) override {
        int dir = -1;
        if (e.key == Key::Up) dir = 0;
        else if (e.key == Key::Down) dir = 1;
        else if (e.key == Key::Left) dir = 2;
        else if (e.key == Key::Right) dir = 3;
        else if (e.key == Key::Space) {
            if (won_ && !keep_going_) {
                keep_going_ = true;
                invalidate();
            }
            return;
        } else if (e.key == Key::Char) {
            uint32_t ch = e.ch;
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch == 'w') dir = 0;
            else if (ch == 's') dir = 1;
            else if (ch == 'a') dir = 2;
            else if (ch == 'd') dir = 3;
            else if (ch == 'n') {
                new_game();
                invalidate();
                return;
            }
        }
        if (dir < 0 || game_over_ || (won_ && !keep_going_)) return;

        animating_ = false;           // a new move cuts the previous animation short
        int before = score_;
        if (!slide(dir)) return;
        if (score_ > before) speaker_beep(740, 20);
        spawn_tile();
        start_anim();
        check_state();
        invalidate();
    }

    void on_tick(uint64_t now) override {
        now_ = now;
        if (animating_) {
            if (since(anim_start_) >= kSlideMs) animating_ = false;
            invalidate();
        }
    }

private:
    struct Move {
        int src, dst, val;
        bool merge;
    };

    uint64_t since(uint64_t t0) const { return now_ > t0 ? now_ - t0 : 0; }

    void new_game() {
        for (int i = 0; i < kN * kN; i++) {
            board_[i] = 0;
            incoming_[i] = merged_[i] = false;
        }
        score_ = 0;
        won_ = keep_going_ = game_over_ = false;
        animating_ = false;
        nmoves_ = 0;
        spawn_idx_ = -1;
        spawn_tile();
        spawn_tile();
        spawn_idx_ = -1;
    }

    void spawn_tile() {
        int empty[kN * kN], n = 0;
        for (int i = 0; i < kN * kN; i++)
            if (!board_[i]) empty[n++] = i;
        if (!n) return;
        int at = empty[cpu_random64() % (uint64_t)n];
        board_[at] = cpu_random64() % 10 == 0 ? 4 : 2;
        spawn_idx_ = at;
    }

    void start_anim() {
        anim_start_ = timer_ms();
        now_ = anim_start_;
        animating_ = true;
    }

    void add_move(int s, int d, int v, bool merge) {
        if (nmoves_ < 16) moves_[nmoves_++] = Move{s, d, v, merge};
    }

    // Slides the board in direction dir (0 up, 1 down, 2 left, 3 right).
    bool slide(int dir) {
        int nb[kN * kN] = {};
        bool mg[kN * kN] = {};
        bool changed = false;
        int gained = 0;
        nmoves_ = 0;
        for (int i = 0; i < kN * kN; i++) incoming_[i] = merged_[i] = false;

        for (int line = 0; line < kN; line++) {
            int idx[kN];
            for (int k = 0; k < kN; k++) {
                switch (dir) {
                case 0: idx[k] = k * kN + line; break;
                case 1: idx[k] = (kN - 1 - k) * kN + line; break;
                case 2: idx[k] = line * kN + k; break;
                default: idx[k] = line * kN + (kN - 1 - k); break;
                }
            }
            int placed = 0;
            for (int j = 0; j < kN; j++) {
                int s = idx[j], v = board_[s];
                if (!v) continue;
                if (placed > 0 && nb[idx[placed - 1]] == v && !mg[idx[placed - 1]]) {
                    int d = idx[placed - 1];
                    nb[d] = v * 2;
                    mg[d] = true;
                    merged_[d] = true;
                    gained += v * 2;
                    add_move(s, d, v, true);
                    changed = true;
                } else {
                    int d = idx[placed];
                    nb[d] = v;
                    if (d != s) {
                        add_move(s, d, v, false);
                        incoming_[d] = true;
                        changed = true;
                    }
                    placed++;
                }
            }
        }
        if (!changed) return false;
        for (int i = 0; i < kN * kN; i++) board_[i] = nb[i];
        score_ += gained;
        if (score_ > best_) {
            best_ = score_;
            save_best(best_);
        }
        return true;
    }

    bool has_moves() const {
        for (int r = 0; r < kN; r++)
            for (int c = 0; c < kN; c++) {
                int v = board_[r * kN + c];
                if (!v) return true;
                if (c + 1 < kN && board_[r * kN + c + 1] == v) return true;
                if (r + 1 < kN && board_[(r + 1) * kN + c] == v) return true;
            }
        return false;
    }

    void check_state() {
        for (int i = 0; i < kN * kN; i++)
            if (board_[i] >= kWinTile && !won_) {
                won_ = true;
                break;
            }
        if (!has_moves()) game_over_ = true;
        if (score_ >= best_) best_ = score_;
    }

    // ---- painting
    void draw_header(Canvas& c) {
        const Theme& t = theme();
        Color pill = t.dark ? rgba(255, 255, 255, 26) : rgba(0, 0, 0, 14);
        char buf[24];
        Rect sc(20, 14, 140, 56), bs(170, 14, 140, 56);
        c.fill_round_rect(sc, 16, pill);
        c.fill_round_rect(bs, 16, pill);
        c.draw_text_in(Rect(sc.x, sc.y + 6, sc.w, 16), "SCORE", t.text_secondary, 11, Font::Bold, Align::Center);
        snprintf(buf, sizeof(buf), "%d", score_);
        c.draw_text_in(Rect(sc.x, sc.y + 22, sc.w, 28), buf, t.text, 22, Font::Bold, Align::Center);
        c.draw_text_in(Rect(bs.x, bs.y + 6, bs.w, 16), "BEST", t.text_secondary, 11, Font::Bold, Align::Center);
        snprintf(buf, sizeof(buf), "%d", best_);
        c.draw_text_in(Rect(bs.x, bs.y + 22, bs.w, 28), buf, t.text, 22, Font::Bold, Align::Center);
    }

    void draw_board(Canvas& c) {
        Rect panel(kBoardX, kBoardY, kBoardW, kBoardH);
        c.draw_shadow(panel, 20, 16, rgba(0, 0, 0, 70), 0, 8);
        c.fill_gradient(panel, hex(0x2C3D63), hex(0x17223D), 20);
        c.stroke_round_rect(panel, 20, rgba(255, 255, 255, 40), 1);

        auto cell_rect = [](int i) {
            int r = i / kN, col = i % kN;
            return Rect(kBoardX + kPad + col * (kCell + kGap), kBoardY + kPad + r * (kCell + kGap), kCell, kCell);
        };
        for (int i = 0; i < kN * kN; i++) {
            Rect r = cell_rect(i);
            c.fill_round_rect(r, r.w * 0.16f, rgba(255, 255, 255, 22));
        }

        float p = 1.0f;
        if (animating_) {
            p = (float)since(anim_start_) / (float)kSlideMs;
            if (p > 1.0f) p = 1.0f;
        }
        float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);   // ease out cubic

        // sliding tiles (drawn under the static ones)
        if (animating_) {
            for (int m = 0; m < nmoves_; m++) {
                Rect a = cell_rect(moves_[m].src), b = cell_rect(moves_[m].dst);
                Rect at(a.x + (int)((b.x - a.x) * e), a.y + (int)((b.y - a.y) * e), a.w, a.h);
                draw_tile(c, at, moves_[m].val, 1.0f);
            }
        }

        for (int i = 0; i < kN * kN; i++) {
            if (!board_[i]) continue;
            if (incoming_[i] && !merged_[i]) continue;   // still sliding in
            Rect r = cell_rect(i);
            float scale = 1.0f;
            if (animating_) {
                if (i == spawn_idx_) {
                    float q = (p - 0.45f) / 0.55f;
                    if (q < 0) continue;
                    scale = 0.3f + 0.7f * smoothstepf(0.0f, 1.0f, q);
                } else if (merged_[i]) {
                    scale = 1.0f + 0.12f * sinf(p * 3.14159f);
                }
            }
            draw_tile(c, r, board_[i], scale);
        }

        if (won_ && !keep_going_) {
            draw_card(c, "You made 2048!", "Press Space to keep going", hex(0xFFD93D));
        } else if (game_over_) {
            draw_card(c, "No moves left", "Press New Game to try again", hex(0xFFFFFF));
        }
    }

    void draw_card(Canvas& c, const char* title, const char* hint, Color col) {
        Rect panel(kBoardX, kBoardY, kBoardW, kBoardH);
        Rect card(panel.x + 50, panel.y + (panel.h - 170) / 2, panel.w - 100, 170);
        c.fill_round_rect(card, 22, rgba(14, 20, 36, 215));
        c.stroke_round_rect(card, 22, rgba(255, 255, 255, 60), 1);
        c.draw_text_in(Rect(card.x, card.y + 30, card.w, 40), title, col, 28, Font::Bold, Align::Center);
        char buf[48];
        snprintf(buf, sizeof(buf), "Score %d   ·   Best %d", score_, best_);
        c.draw_text_in(Rect(card.x, card.y + 82, card.w, 22), buf, rgba(255, 255, 255, 230), 15, Font::Medium,
                       Align::Center);
        c.draw_text_in(Rect(card.x, card.y + 120, card.w, 20), hint, rgba(255, 255, 255, 170), 13, Font::Regular,
                       Align::Center);
    }

    int board_[kN * kN] = {};
    bool incoming_[kN * kN] = {};
    bool merged_[kN * kN] = {};
    Move moves_[16] = {};
    int nmoves_ = 0;
    int spawn_idx_ = -1;
    int score_ = 0, best_ = 0;
    bool won_ = false, keep_going_ = false, game_over_ = false;
    bool animating_ = false;
    uint64_t now_ = 0, anim_start_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x4A5C8C), hex(0x1A2448));
    const Color cols[4] = {hex(0x9EE3C9), hex(0x6CCFF6), hex(0xFFD93D), hex(0xFF7A59)};
    int pad = r.w / 6, gap = r.w / 14;
    int cw = (r.w - 2 * pad - gap) / 2;
    for (int k = 0; k < 4; k++) {
        int x = r.x + pad + (k % 2) * (cw + gap), y = r.y + pad + (k / 2) * (cw + gap);
        Rect t(x, y, cw, cw);
        float rad = cw * 0.22f;
        c.fill_round_rect(Rect(t.x, t.y + 2, t.w, t.h), rad, darken(cols[k], 90));
        c.fill_gradient(t, lighten(cols[k], 50), cols[k], rad);
    }
}

Window* create() { return new TilesWindow(); }

AppRegistrar reg({"tiles", "Tiles", icon, create, "Games", true, false, 122, "Slide and merge numbers to reach 2048"});

}  // namespace
