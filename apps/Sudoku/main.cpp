// Sudoku: generated puzzles with a unique solution, pencil marks, conflict
// highlighting, a timer and hints.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"

namespace {

constexpr int kBoardX = 36, kBoardY = 36, kCell = 46;
constexpr int kPanelX = 474, kPanelW = 180;
constexpr int kBoardPx = 9 * kCell;

// ---------------------------------------------------------------- solver
inline int popcount16(uint16_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

// Bit d set = digit d (1..9) is allowed at idx.
uint16_t candidates(const uint8_t* g, int idx) {
    int r = idx / 9, c = idx % 9;
    uint16_t used = 0;
    for (int i = 0; i < 9; i++) {
        used |= 1u << g[r * 9 + i];
        used |= 1u << g[i * 9 + c];
    }
    int br = r / 3 * 3, bc = c / 3 * 3;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) used |= 1u << g[(br + i) * 9 + bc + j];
    return (uint16_t)(0x3FE & ~used);
}

int g_budget = 0;   // solver node budget; negative = exhausted

// Counts solutions up to `limit`. Picks the empty cell with fewest candidates.
int count_solutions(uint8_t* g, int limit) {
    int best = -1, best_n = 10;
    uint16_t best_mask = 0;
    for (int i = 0; i < 81; i++) {
        if (g[i]) continue;
        uint16_t m = candidates(g, i);
        int n = popcount16(m);
        if (n < best_n) {
            best_n = n;
            best = i;
            best_mask = m;
            if (n <= 1) break;
        }
    }
    if (best < 0) return 1;            // full grid
    if (best_n == 0) return 0;         // dead end
    int count = 0;
    for (int d = 1; d <= 9 && count < limit; d++) {
        if (!(best_mask & (1u << d))) continue;
        if (--g_budget < 0) return count;
        g[best] = (uint8_t)d;
        count += count_solutions(g, limit - count);
        g[best] = 0;
    }
    return count;
}

// Fills g completely with a random valid grid. Returns false only on a dead end.
bool fill_random(uint8_t* g) {
    int best = -1, best_n = 10;
    uint16_t best_mask = 0;
    for (int i = 0; i < 81; i++) {
        if (g[i]) continue;
        uint16_t m = candidates(g, i);
        int n = popcount16(m);
        if (n < best_n) { best_n = n; best = i; best_mask = m; }
    }
    if (best < 0) return true;
    if (best_n == 0) return false;
    int ds[9], n = 0;
    for (int d = 1; d <= 9; d++)
        if (best_mask & (1u << d)) ds[n++] = d;
    for (int i = n - 1; i > 0; i--) {       // Fisher-Yates shuffle
        int j = (int)(cpu_random64() % (uint64_t)(i + 1));
        int t = ds[i]; ds[i] = ds[j]; ds[j] = t;
    }
    for (int k = 0; k < n; k++) {
        g[best] = (uint8_t)ds[k];
        if (fill_random(g)) return true;
        g[best] = 0;
    }
    return false;
}

// Difficulty: number of cells removed from a full solution.
int removals_for(int difficulty) { return difficulty == 0 ? 42 : difficulty == 1 ? 50 : 58; }

// ---------------------------------------------------------------- game state
struct Game {
    uint8_t puzzle[81];     // givens (0 = empty)
    uint8_t solution[81];
    uint8_t cur[81];        // what the player has entered (givens included)
    uint16_t marks[81];     // pencil marks, bit d = digit d
    bool hinted[81];
};

// Builds a puzzle with a unique solution. Every removal is checked with the solver.
void generate(Game& g, int difficulty) {
    uint8_t full[81] = {0};
    fill_random(full);
    memcpy(g.solution, full, 81);
    memcpy(g.puzzle, full, 81);

    int order[81];
    for (int i = 0; i < 81; i++) order[i] = i;
    for (int i = 80; i > 0; i--) {
        int j = (int)(cpu_random64() % (uint64_t)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }
    int target = removals_for(difficulty), removed = 0;
    for (int k = 0; k < 81 && removed < target; k++) {
        int idx = order[k];
        uint8_t saved = g.puzzle[idx];
        g.puzzle[idx] = 0;
        uint8_t probe[81];
        memcpy(probe, g.puzzle, 81);
        g_budget = 1500;
        int solutions = count_solutions(probe, 2);
        if (g_budget >= 0 && solutions == 1) removed++;
        else g.puzzle[idx] = saved;   // not unique (or unknown): keep the given
    }
    memcpy(g.cur, g.puzzle, 81);
    memset(g.marks, 0, sizeof(g.marks));
    memset(g.hinted, 0, sizeof(g.hinted));
}

bool is_conflict(const uint8_t* v, int idx) {
    if (!v[idx]) return false;
    int r = idx / 9, c = idx % 9;
    int br = r / 3 * 3, bc = c / 3 * 3;
    for (int i = 0; i < 9; i++) {
        int a = r * 9 + i, b = i * 9 + c;
        if (a != idx && v[a] == v[idx]) return true;
        if (b != idx && v[b] == v[idx]) return true;
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int k = (br + i) * 9 + bc + j;
            if (k != idx && v[k] == v[idx]) return true;
        }
    return false;
}

bool is_peer(int a, int b) {
    if (a == b) return false;
    int ra = a / 9, ca = a % 9, rb = b / 9, cb = b % 9;
    return ra == rb || ca == cb || (ra / 3 == rb / 3 && ca / 3 == cb / 3);
}

// ---------------------------------------------------------------- icon
void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x5ED0A8), hex(0x1B7F62));
    float x0 = r.x + r.w * 0.2f, x1 = r.x + r.w * 0.8f;
    float y0 = r.y + r.h * 0.2f, y1 = r.y + r.h * 0.8f;
    float cw = (x1 - x0) / 3.0f, ch = (y1 - y0) / 3.0f;
    Color ln = rgba(255, 255, 255, 150);
    for (int k = 1; k < 3; k++) {
        c.draw_line(x0 + cw * k, y0, x0 + cw * k, y1, ln, 1.2f);
        c.draw_line(x0, y0 + ch * k, x1, y0 + ch * k, ln, 1.2f);
    }
    c.stroke_rect(Rect((int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)), ln, 2);
    c.fill_round_rect(Rect((int)(x0 + cw), (int)(y0 + ch), (int)cw, (int)ch), 3, rgba(255, 255, 255, 60));
    icon_text(c, Rect((int)(x0 + cw), (int)(y0 + ch), (int)cw, (int)ch), "5", hex(0xFFFFFF), 0.62f, Font::Bold);
}

// ---------------------------------------------------------------- window
const char* kDigits[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};

class SudokuWindow : public Window {
public:
    SudokuWindow() : Window("Sudoku", 680, 500, WF_NO_ZOOM) {
        difficulty = (SegmentedControl*)add(new SegmentedControl(Rect(kPanelX, 36, kPanelW, 28), {"Easy", "Medium", "Hard"}));
        difficulty->selected = 0;
        new_btn = (Button*)add(new Button(Rect(kPanelX, 74, kPanelW, 30), "New Game", Button::Primary));
        new_btn->on_click = [this] { new_game(difficulty->selected); };

        mode = (SegmentedControl*)add(new SegmentedControl(Rect(kPanelX, 116, kPanelW, 28), {"Pen", "Pencil"}));
        mode->selected = 0;
        mode->on_change = [this](int i) { pencil = i == 1; update_widgets(); };

        for (int i = 0; i < 9; i++) {
            int col = i % 3, row = i / 3;
            Button* b = (Button*)add(new Button(Rect(kPanelX + col * 64, 160 + row * 46, 52, 40), kDigits[i]));
            b->font_size = 18;
            b->on_click = [this, i] { enter(i + 1); };
            pad[i] = b;
        }
        erase_btn = (Button*)add(new Button(Rect(kPanelX, 300, kPanelW, 30), "Erase"));
        erase_btn->on_click = [this] { enter(0); };
        hint_btn = (Button*)add(new Button(Rect(kPanelX, 338, kPanelW, 30), "Hint"));
        hint_btn->on_click = [this] { hint(); };

        status = (Label*)add(new Label(Rect(kBoardX, kBoardY + kBoardPx + 12, kBoardPx, 20), "", 13, Font::Medium));
        conflict_lbl = (Label*)add(new Label(Rect(kPanelX, 430, kPanelW, 18), "", 12, Font::Regular));

        new_game(0);
        set_ticking(true);
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        paint_board(c);
        paint_panel(c);
    }

    void on_mouse_down(int x, int y, int button) override {
        int idx = cell_at(x, y);
        if (idx < 0) {
            Window::on_mouse_down(x, y, button);
            return;
        }
        sel = idx;
        update_widgets();
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Char && e.ch >= '1' && e.ch <= '9' && !e.cmd()) {
            enter((int)(e.ch - '0'));
            return;
        }
        if (e.key == Key::Char && e.ch == '0' && !e.cmd()) { enter(0); return; }
        if (e.key == Key::Backspace || e.key == Key::Delete) { enter(0); return; }
        if (e.key == Key::Char && (e.ch == 'p' || e.ch == 'P') && !e.cmd()) {
            mode->selected = pencil ? 0 : 1;
            pencil = !pencil;
            update_widgets();
            return;
        }
        if (e.key == Key::Up || e.key == Key::Down || e.key == Key::Left || e.key == Key::Right) {
            if (sel < 0) sel = 40;
            int r = sel / 9, c = sel % 9;
            if (e.key == Key::Up && r > 0) r--;
            if (e.key == Key::Down && r < 8) r++;
            if (e.key == Key::Left && c > 0) c--;
            if (e.key == Key::Right && c < 8) c++;
            sel = r * 9 + c;
            update_widgets();
            return;
        }
        Window::on_key(e);
    }

    void on_tick(uint64_t) override {
        uint64_t s = elapsed_ms() / 1000;
        if (s != last_second) {
            last_second = s;
            update_widgets();
        }
    }

private:
    SegmentedControl* difficulty = nullptr;
    SegmentedControl* mode = nullptr;
    Button* new_btn = nullptr;
    Button* pad[9] = {nullptr};
    Button* erase_btn = nullptr;
    Button* hint_btn = nullptr;
    Label* status = nullptr;
    Label* conflict_lbl = nullptr;

    Game g{};
    int level = 0;
    int sel = -1;
    bool pencil = false;
    bool solved = false;
    uint64_t start_ms = 0;
    uint64_t finished_ms = 0;
    uint64_t last_second = 0;
    int hints = 0;

    uint64_t elapsed_ms() const { return solved ? finished_ms - start_ms : timer_ms() - start_ms; }

    int cell_at(int x, int y) const {
        if (x < kBoardX || y < kBoardY || x >= kBoardX + kBoardPx || y >= kBoardY + kBoardPx) return -1;
        return ((y - kBoardY) / kCell) * 9 + (x - kBoardX) / kCell;
    }

    void new_game(int difficulty_level) {
        level = difficulty_level;
        generate(g, level);
        sel = -1;
        solved = false;
        hints = 0;
        start_ms = timer_ms();
        last_second = 0;
        update_widgets();
    }

    bool given(int idx) const { return g.puzzle[idx] != 0; }

    void enter(int d) {
        if (solved || sel < 0 || given(sel)) return;
        if (pencil && d > 0) {
            if (g.cur[sel] == 0) g.marks[sel] ^= (uint16_t)(1u << d);
            invalidate();
            return;
        }
        g.cur[sel] = (uint8_t)d;
        g.marks[sel] = 0;
        g.hinted[sel] = false;
        if (d > 0) {
            // A placed digit leaves the pencil marks of its peers.
            for (int i = 0; i < 81; i++)
                if (is_peer(i, sel)) g.marks[i] &= (uint16_t)~(1u << d);
        }
        check_solved();
        update_widgets();
    }

    void hint() {
        if (solved) return;
        int target = -1;
        if (sel >= 0 && !given(sel) && g.cur[sel] != g.solution[sel]) target = sel;
        for (int i = 0; target < 0 && i < 81; i++)
            if (g.cur[i] != g.solution[i]) target = i;
        if (target < 0) return;
        g.cur[target] = g.solution[target];
        g.marks[target] = 0;
        g.hinted[target] = true;
        sel = target;
        hints++;
        check_solved();
        update_widgets();
    }

    void check_solved() {
        for (int i = 0; i < 81; i++)
            if (g.cur[i] != g.solution[i]) return;
        solved = true;
        finished_ms = timer_ms();
    }

    int conflict_count() const {
        int n = 0;
        for (int i = 0; i < 81; i++)
            if (is_conflict(g.cur, i)) n++;
        return n;
    }

    void paint_board(Canvas& c) {
        const Theme& t = theme();
        Rect board(kBoardX - 6, kBoardY - 6, kBoardPx + 12, kBoardPx + 12);
        c.draw_shadow(board, 10, 10, rgba(0, 0, 0, 50), 0, 3);
        c.fill_round_rect(board, 10, t.dark ? hex(0x2A2A2F) : hex(0xFFFFFF));
        c.stroke_round_rect(board, 10, t.separator, 1);

        int sel_val = sel >= 0 ? g.cur[sel] : 0;
        for (int i = 0; i < 81; i++) {
            int r = i / 9, col = i % 9;
            Rect cell(kBoardX + col * kCell, kBoardY + r * kCell, kCell, kCell);
            Color bg = 0;
            if (i == sel) bg = with_alpha(t.accent, 110);
            else if (sel >= 0 && is_peer(i, sel)) bg = t.dark ? rgba(255, 255, 255, 14) : rgba(0, 0, 0, 12);
            else if (sel_val && g.cur[i] == sel_val) bg = with_alpha(t.accent, 55);
            if (is_conflict(g.cur, i)) bg = rgba(255, 69, 58, 70);
            if (bg) c.fill_rect(cell, bg);

            if (g.cur[i]) {
                Color fg = t.text;
                Font f = Font::Bold;
                if (is_conflict(g.cur, i)) fg = t.destructive;
                else if (g.hinted[i]) fg = t.success;
                else if (!given(i)) { fg = t.accent; f = Font::Medium; }
                char ch[2] = {(char)('0' + g.cur[i]), 0};
                c.draw_text_in(cell, ch, fg, 24, f, Align::Center);
            } else if (g.marks[i]) {
                int sub = kCell / 3;
                for (int d = 1; d <= 9; d++) {
                    if (!(g.marks[i] & (1u << d))) continue;
                    int sr = (d - 1) / 3, sc = (d - 1) % 3;
                    char ch[2] = {(char)('0' + d), 0};
                    c.draw_text_in(Rect(cell.x + sc * sub, cell.y + sr * sub, sub, sub), ch, t.text_secondary, 10,
                                   Font::Regular, Align::Center);
                }
            }
        }

        // thin lines between cells, heavier lines between 3x3 boxes
        Color thin = t.dark ? rgba(255, 255, 255, 26) : rgba(0, 0, 0, 22);
        Color thick = t.dark ? rgba(255, 255, 255, 90) : rgba(0, 0, 0, 90);
        for (int k = 0; k <= 9; k++) {
            int p = kBoardX + k * kCell;
            int q = kBoardY + k * kCell;
            bool heavy = k % 3 == 0;
            c.fill_rect(Rect(p, kBoardY, heavy ? 2 : 1, kBoardPx), heavy ? thick : thin);
            c.fill_rect(Rect(kBoardX, q, kBoardPx, heavy ? 2 : 1), heavy ? thick : thin);
        }
        if (sel >= 0) c.stroke_rect(Rect(kBoardX + (sel % 9) * kCell, kBoardY + (sel / 9) * kCell, kCell, kCell), t.accent, 2);
    }

    void paint_panel(Canvas& c) {
        const Theme& t = theme();
        uint64_t total = elapsed_ms() / 1000;
        char clock[16];
        snprintf(clock, sizeof(clock), "%02llu:%02llu", (unsigned long long)(total / 60), (unsigned long long)(total % 60));
        c.draw_text_in(Rect(kPanelX, 380, kPanelW, 14), "Elapsed", t.text_secondary, 11, Font::Medium);
        c.draw_text_in(Rect(kPanelX, 394, kPanelW, 30), clock, t.text, 26, Font::Bold);

    }

    // Labels and button enabled states; called after every state change, never from paint.
    void update_widgets() {
        const Theme& t = theme();
        uint64_t total = elapsed_ms() / 1000;
        char clock[16];
        snprintf(clock, sizeof(clock), "%02llu:%02llu", (unsigned long long)(total / 60), (unsigned long long)(total % 60));
        int empty = 0;
        for (int i = 0; i < 81; i++)
            if (!g.cur[i]) empty++;
        char msg[72];
        if (solved) {
            snprintf(msg, sizeof(msg), "Solved in %s. Hints used: %d", clock, hints);
            status->color = t.success;
        } else {
            snprintf(msg, sizeof(msg), "%d empty cell%s  |  %s", empty, empty == 1 ? "" : "s",
                     level == 0 ? "Easy" : level == 1 ? "Medium" : "Hard");
            status->color = t.text_secondary;
        }
        status->set_text(msg);
        int conflicts = conflict_count();
        if (!solved && conflicts) {
            char cmsg[32];
            snprintf(cmsg, sizeof(cmsg), "%d conflicting cell%s", conflicts, conflicts == 1 ? "" : "s");
            conflict_lbl->set_text(cmsg);
            conflict_lbl->color = t.destructive;
        } else {
            conflict_lbl->set_text(pencil ? "Pencil mode on" : "");
            conflict_lbl->color = t.text_secondary;
        }
        bool editable = !solved && sel >= 0 && !given(sel);
        hint_btn->enabled = !solved;
        erase_btn->enabled = editable;
        for (Button* b : pad) b->enabled = editable;
        invalidate();
    }
};

Window* create() { return new SudokuWindow(); }

AppRegistrar reg({"sudoku", "Sudoku", icon, create, "Games", true, false, 100,
                  "Number puzzles with pencil marks and hints"});

}  // namespace
