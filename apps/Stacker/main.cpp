// Stacker: falling tetromino blocks in a 10x20 well.
// Rotation with wall kicks, soft and hard drop, a ghost piece, next-piece
// preview, flashing line clears and speed-ups every 10 lines.
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

constexpr int kW = 10, kH = 20, kCell = 22;
constexpr int kBoardX = 20, kBoardY = 20;
constexpr uint64_t kFlashMs = 260;

// Piece colors: I, O, T, S, Z, J, L.
const Color kPieceCol[7] = {
    hex(0x3DD9EB), hex(0xF7C948), hex(0xB175FF), hex(0x62D68A), hex(0xF0707E), hex(0x5B86F5), hex(0xF59A52),
};

// Base cells in a 4x4 box for rotation 0.
const int8_t kBase[7][4][2] = {
    {{0, 1}, {1, 1}, {2, 1}, {3, 1}},   // I
    {{1, 0}, {2, 0}, {1, 1}, {2, 1}},   // O
    {{1, 0}, {0, 1}, {1, 1}, {2, 1}},   // T
    {{1, 0}, {2, 0}, {0, 1}, {1, 1}},   // S
    {{0, 0}, {1, 0}, {1, 1}, {2, 1}},   // Z
    {{0, 0}, {0, 1}, {1, 1}, {2, 1}},   // J
    {{2, 0}, {0, 1}, {1, 1}, {2, 1}},   // L
};

// Cells of a piece in a given rotation (0..3), relative to its top-left corner.
// Rotation is done on doubled coordinates around the piece pivot.
void piece_cells(int type, int rot, int out[4][2]) {
    int pc = type == 0 ? 3 : 2;   // I pivots on a cell corner, the others on a cell center
    for (int k = 0; k < 4; k++) {
        int x = kBase[type][k][0] * 2, y = kBase[type][k][1] * 2;
        if (type != 1)
            for (int r = 0; r < rot; r++) {
                int nx = pc - (y - pc), ny = pc + (x - pc);
                x = nx;
                y = ny;
            }
        out[k][0] = x / 2;
        out[k][1] = y / 2;
    }
}

// ---- best score: <home>/Library/Games/stacker.txt
String best_path() {
    String dir = String("/Users/") + session_user() + "/Library/Games";
    vfs::mkdir(dir.c_str());
    return dir + "/stacker.txt";
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

void draw_block(Canvas& c, Rect r, Color base, int alpha = 255) {
    float rad = r.w * 0.22f;
    c.fill_round_rect(Rect(r.x, r.y + 2, r.w, r.h), rad, with_alpha(darken(base, 95), alpha));
    c.fill_gradient(r, with_alpha(lighten(base, 70), alpha), with_alpha(base, alpha), rad);
    c.fill_round_rect(Rect(r.x + 3, r.y + 3, r.w - 6, r.h / 4), rad * 0.6f, rgba(255, 255, 255, alpha / 4));
}

void stat(Canvas& c, Rect r, const char* label, const char* value) {
    const Theme& t = theme();
    c.fill_round_rect(r, r.h / 2.0f, t.dark ? rgba(255, 255, 255, 26) : rgba(0, 0, 0, 14));
    c.draw_text_in(Rect(r.x + 16, r.y, r.w / 2, r.h), label, t.text_secondary, 12, Font::Medium);
    c.draw_text_in(Rect(r.x + r.w / 2, r.y, r.w / 2 - 16, r.h), value, t.text, 17, Font::Mono, Align::Right);
}

class StackerWindow : public Window {
public:
    StackerWindow() : Window("Stacker", 440, 480, WF_NO_ZOOM) {
        auto* nb = (Button*)add(new Button(Rect(260, 330, 160, 32), "New Game", Button::Primary));
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
        draw_board(c);
        draw_side(c);
    }

    void on_key(const KeyEvent& e) override {
        int op = 0;   // 1 left, 2 right, 3 rotate, 4 soft, 5 hard, 6 pause, 7 new
        if (e.key == Key::Left) op = 1;
        else if (e.key == Key::Right) op = 2;
        else if (e.key == Key::Up) op = 3;
        else if (e.key == Key::Down) op = 4;
        else if (e.key == Key::Space) op = 5;
        else if (e.key == Key::Char) {
            uint32_t ch = e.ch;
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch == 'a') op = 1;
            else if (ch == 'd') op = 2;
            else if (ch == 'w' || ch == 'x') op = 3;
            else if (ch == 's') op = 4;
            else if (ch == 'p') op = 6;
            else if (ch == 'n' || ch == 'r') op = 7;
        }
        if (op == 0) return;
        if (op == 7) {
            new_game();
            invalidate();
            return;
        }
        if (op == 6) {
            toggle_pause();
            invalidate();
            return;
        }
        if (state_ != Playing) return;
        switch (op) {
        case 1: move(-1); break;
        case 2: move(1); break;
        case 3: rotate(); break;
        case 4:
            if (fits(type_, rot_, px_, py_ + 1)) {
                py_++;
                score_ += 1;
                last_fall_ = timer_ms();
                invalidate();
            } else {
                lock();
            }
            break;
        case 5: hard_drop(); break;
        }
        invalidate();
    }

    void on_tick(uint64_t now) override {
        now_ = now;
        if (state_ == Playing && since(last_fall_) >= (uint64_t)fall_ms()) {
            last_fall_ = now;
            if (fits(type_, rot_, px_, py_ + 1)) py_++;
            else lock();
            invalidate();
        } else if (state_ == Clearing) {
            if (since(clear_at_) >= kFlashMs) finish_clear();
            invalidate();
        }
    }

private:
    enum State : uint8_t { Playing, Paused, Clearing, Over };

    uint64_t since(uint64_t t0) const { return now_ > t0 ? now_ - t0 : 0; }
    int level() const { return 1 + lines_ / 10; }
    int fall_ms() const {
        int ms = 800 - (level() - 1) * 75;
        return ms < 70 ? 70 : ms;
    }

    int next_piece() {
        if (bag_pos_ >= 7) {
            for (int i = 0; i < 7; i++) bag_[i] = i;
            for (int i = 6; i > 0; i--) {
                int j = (int)(cpu_random64() % (uint64_t)(i + 1));
                int t = bag_[i];
                bag_[i] = bag_[j];
                bag_[j] = t;
            }
            bag_pos_ = 0;
        }
        return bag_[bag_pos_++];
    }

    void new_game() {
        for (int i = 0; i < kW * kH; i++) board_[i] = 0;
        score_ = lines_ = 0;
        bag_pos_ = 7;
        next_type_ = next_piece();
        state_ = Playing;
        nclear_ = 0;
        spawn();
        last_fall_ = timer_ms();
    }

    bool fits(int type, int rot, int px, int py) const {
        int cs[4][2];
        piece_cells(type, rot, cs);
        for (int k = 0; k < 4; k++) {
            int x = px + cs[k][0], y = py + cs[k][1];
            if (x < 0 || x >= kW || y >= kH) return false;
            if (y >= 0 && board_[y * kW + x]) return false;
        }
        return true;
    }

    void spawn() {
        type_ = next_type_;
        next_type_ = next_piece();
        rot_ = 0;
        px_ = 3;
        py_ = 0;
        if (!fits(type_, rot_, px_, py_)) game_over();
    }

    void move(int dx) {
        if (fits(type_, rot_, px_ + dx, py_)) px_ += dx;
    }

    void rotate() {
        if (type_ == 1) return;   // O does not rotate
        int nr = (rot_ + 1) & 3;
        static const int kicks[5] = {0, -1, 1, -2, 2};
        for (int k = 0; k < 5; k++)
            if (fits(type_, nr, px_ + kicks[k], py_)) {
                px_ += kicks[k];
                rot_ = nr;
                return;
            }
    }

    void hard_drop() {
        while (fits(type_, rot_, px_, py_ + 1)) {
            py_++;
            score_ += 2;
        }
        lock();
    }

    void lock() {
        int cs[4][2];
        piece_cells(type_, rot_, cs);
        for (int k = 0; k < 4; k++) {
            int x = px_ + cs[k][0], y = py_ + cs[k][1];
            if (y < 0) {
                game_over();
                return;
            }
            board_[y * kW + x] = (uint8_t)(type_ + 1);
        }
        nclear_ = 0;
        for (int y = 0; y < kH; y++) {
            bool full = true;
            for (int x = 0; x < kW && full; x++) full = board_[y * kW + x] != 0;
            if (full) clear_rows_[nclear_++] = y;
        }
        if (nclear_) {
            state_ = Clearing;
            clear_at_ = now_;
            speaker_beep(1320, 30);
        } else {
            spawn();
            last_fall_ = timer_ms();
        }
    }

    void finish_clear() {
        int wr = kH - 1;
        for (int y = kH - 1; y >= 0; y--) {
            bool full = false;
            for (int i = 0; i < nclear_; i++) full |= clear_rows_[i] == y;
            if (full) continue;
            for (int x = 0; x < kW; x++) board_[wr * kW + x] = board_[y * kW + x];
            wr--;
        }
        for (int y = wr; y >= 0; y--)
            for (int x = 0; x < kW; x++) board_[y * kW + x] = 0;

        static const int kPoints[5] = {0, 100, 300, 500, 800};
        score_ += kPoints[nclear_] * level();
        lines_ += nclear_;
        nclear_ = 0;
        state_ = Playing;
        spawn();
        last_fall_ = timer_ms();
    }

    void game_over() {
        state_ = Over;
        if (score_ > best_) best_ = score_;
        save_best(best_);
        speaker_beep(180, 60);
    }

    void toggle_pause() {
        if (state_ == Playing) state_ = Paused;
        else if (state_ == Paused) {
            state_ = Playing;
            last_fall_ = timer_ms();
        }
    }

    // ---- painting
    Rect cell_rect(int x, int y) const {
        return Rect(kBoardX + x * kCell, kBoardY + y * kCell, kCell, kCell);
    }

    void draw_board(Canvas& c) {
        Rect panel(kBoardX - 8, kBoardY - 8, kW * kCell + 16, kH * kCell + 16);
        c.draw_shadow(panel, 16, 14, rgba(0, 0, 0, 80), 0, 6);
        c.fill_gradient(panel, hex(0x1A2340), hex(0x0B1020), 16);
        c.stroke_round_rect(panel, 16, rgba(255, 255, 255, 34), 1);

        Rect well(kBoardX, kBoardY, kW * kCell, kH * kCell);
        for (int x = 1; x < kW; x++)
            c.fill_rect(Rect(kBoardX + x * kCell, kBoardY, 1, kH * kCell), rgba(255, 255, 255, 9));
        for (int y = 1; y < kH; y++)
            c.fill_rect(Rect(kBoardX, kBoardY + y * kCell, kW * kCell, 1), rgba(255, 255, 255, 9));

        for (int y = 0; y < kH; y++)
            for (int x = 0; x < kW; x++) {
                uint8_t v = board_[y * kW + x];
                if (v) draw_block(c, cell_rect(x, y).inset(1), kPieceCol[v - 1]);
            }

        if (state_ == Clearing) {
            uint64_t t = since(clear_at_);
            float k = (float)t / (float)kFlashMs;
            for (int i = 0; i < nclear_; i++) {
                Rect r(kBoardX, kBoardY + clear_rows_[i] * kCell, kW * kCell, kCell);
                int a = (int)(230.0f * (1.0f - k) * (0.6f + 0.4f * sinf(k * 18.0f)));
                c.fill_rect(r, rgba(255, 255, 255, a < 0 ? 0 : a));
            }
        }

        if (state_ == Playing || state_ == Paused) {
            // ghost piece
            int gy = py_;
            while (fits(type_, rot_, px_, gy + 1)) gy++;
            if (gy != py_) {
                int cs[4][2];
                piece_cells(type_, rot_, cs);
                for (int k = 0; k < 4; k++) {
                    Rect r = cell_rect(px_ + cs[k][0], gy + cs[k][1]).inset(2);
                    c.stroke_round_rect(r, r.w * 0.22f, with_alpha(kPieceCol[type_], 150), 1.5f);
                }
            }
            // falling piece
            int cs[4][2];
            piece_cells(type_, rot_, cs);
            for (int k = 0; k < 4; k++) {
                int x = px_ + cs[k][0], y = py_ + cs[k][1];
                if (y >= 0) draw_block(c, cell_rect(x, y).inset(1), kPieceCol[type_]);
            }
        }

        if (state_ == Paused) draw_card(c, "Paused", "Press P to keep stacking", hex(0xFFFFFF));
        else if (state_ == Over) draw_card(c, "Stack over", "Press N or New Game to retry", hex(0xFF8A8A));
    }

    void draw_card(Canvas& c, const char* title, const char* hint, Color col) {
        Rect panel(kBoardX, kBoardY, kW * kCell, kH * kCell);
        Rect card(panel.x + 12, panel.y + panel.h / 2 - 66, panel.w - 24, 132);
        c.fill_round_rect(card, 20, rgba(10, 14, 28, 220));
        c.stroke_round_rect(card, 20, rgba(255, 255, 255, 60), 1);
        c.draw_text_in(Rect(card.x, card.y + 18, card.w, 34), title, col, 24, Font::Bold, Align::Center);
        char buf[40];
        snprintf(buf, sizeof(buf), "Score %d", score_);
        c.draw_text_in(Rect(card.x, card.y + 56, card.w, 20), buf, rgba(255, 255, 255, 230), 14, Font::Medium,
                       Align::Center);
        c.draw_text_in(Rect(card.x, card.y + 92, card.w, 18), hint, rgba(255, 255, 255, 160), 12, Font::Regular,
                       Align::Center);
    }

    void draw_side(Canvas& c) {
        const Theme& t = theme();
        Rect box(260, 12, 160, 110);
        c.fill_round_rect(box, 16, t.dark ? rgba(255, 255, 255, 18) : rgba(0, 0, 0, 10));
        c.draw_text_in(Rect(box.x + 14, box.y + 8, 100, 16), "NEXT", t.text_secondary, 11, Font::Bold);

        int minx = 9, maxx = -9, miny = 9, maxy = -9;
        int cs[4][2];
        piece_cells(next_type_, 0, cs);
        for (int k = 0; k < 4; k++) {
            if (cs[k][0] < minx) minx = cs[k][0];
            if (cs[k][0] > maxx) maxx = cs[k][0];
            if (cs[k][1] < miny) miny = cs[k][1];
            if (cs[k][1] > maxy) maxy = cs[k][1];
        }
        int ps = 18;
        int w = (maxx - minx + 1) * ps, h = (maxy - miny + 1) * ps;
        int ox = box.x + (box.w - w) / 2, oy = box.y + 26 + (box.h - 26 - h) / 2;
        for (int k = 0; k < 4; k++) {
            Rect r(ox + (cs[k][0] - minx) * ps, oy + (cs[k][1] - miny) * ps, ps, ps);
            draw_block(c, r.inset(1), kPieceCol[next_type_]);
        }

        char buf[24];
        snprintf(buf, sizeof(buf), "%d", score_);
        stat(c, Rect(260, 134, 160, 40), "Score", buf);
        snprintf(buf, sizeof(buf), "%d", lines_);
        stat(c, Rect(260, 182, 160, 40), "Lines", buf);
        snprintf(buf, sizeof(buf), "%d", level());
        stat(c, Rect(260, 230, 160, 40), "Level", buf);
        snprintf(buf, sizeof(buf), "%d", best_);
        stat(c, Rect(260, 278, 160, 40), "Best", buf);

        const char* keys[] = {"Left / Right   move", "Up   rotate", "Down   soft drop", "Space   hard drop", "P   pause"};
        for (int i = 0; i < 5; i++)
            c.draw_text_in(Rect(262, 380 + i * 18, 160, 18), keys[i], t.text_secondary, 12, Font::Regular);
    }

    int board_[kW * kH] = {};
    int type_ = 0, rot_ = 0, px_ = 3, py_ = 0;
    int next_type_ = 0;
    int bag_[7] = {};
    int bag_pos_ = 7;
    int score_ = 0, lines_ = 0, best_ = 0;
    State state_ = Playing;
    int clear_rows_[4] = {};
    int nclear_ = 0;
    uint64_t now_ = 0, last_fall_ = 0, clear_at_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x3B5BA8), hex(0x14203F));
    int s = r.w / 7;
    int ox = r.x + r.w / 2 - s * 2, oy = r.y + r.h / 2 - s * 1;
    // a T piece and an L piece stacked in the well
    struct B { int x, y, col; };
    const B blocks[] = {
        {0, 0, 2}, {1, 0, 2}, {2, 0, 2}, {1, 1, 2},          // T (purple)
        {0, 2, 6}, {0, 3, 6}, {1, 3, 6}, {2, 3, 6},          // L (orange)
        {3, 2, 3}, {4, 2, 3}, {4, 3, 3}, {3, 3, 3},          // O-ish (green)
    };
    for (const B& b : blocks) {
        Rect cell(ox + b.x * s, oy + b.y * s, s, s);
        draw_block(c, cell.inset(s / 12), kPieceCol[b.col]);
    }
}

Window* create() { return new StackerWindow(); }

AppRegistrar reg({"stacker", "Stacker", icon, create, "Games", true, false, 123, "Stack falling blocks and clear lines"});

}  // namespace
