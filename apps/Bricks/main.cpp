// Bricks: a ball-and-paddle brick breaker.
// Move the paddle with the mouse or the arrow keys (A/D too). Click, Space or
// Up launches the ball. The bounce angle depends on where the ball hits the paddle.
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

constexpr int kWinW = 520, kWinH = 600;
constexpr int kFieldX = 10, kFieldY = 56, kFieldW = 500, kFieldH = 534;
constexpr int kCols = 10, kRows = 5;
constexpr int kBrickW = 44, kBrickH = 22, kBrickStepX = 48, kBrickStepY = 28;
constexpr int kBrickX0 = 22, kBrickY0 = 84;
constexpr int kPaddleW = 116, kPaddleH = 12, kPaddleY = 566;
constexpr float kBallR = 7.0f;
constexpr int kParticles = 80;

const Color kRowCol[kRows] = {hex(0xFF6B8B), hex(0xFFA94D), hex(0xFFD43B), hex(0x69DB7C), hex(0x4DABF7)};

// ---- best score: <home>/Library/Games/bricks.txt
String best_path() {
    String dir = String("/Users/") + session_user() + "/Library/Games";
    vfs::mkdir(dir.c_str());
    return dir + "/bricks.txt";
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

void draw_brick(Canvas& c, Rect r, Color base, bool tough) {
    float rad = 6.0f;
    c.fill_round_rect(Rect(r.x, r.y + 3, r.w, r.h), rad, darken(base, 90));
    c.fill_gradient(r, lighten(base, 60), base, rad);
    c.fill_round_rect(Rect(r.x + 3, r.y + 2, r.w - 6, r.h / 3), 3, rgba(255, 255, 255, 70));
    if (tough) {
        c.stroke_round_rect(Rect(r.x + 5, r.y + 5, r.w - 10, r.h - 10), 3, rgba(255, 255, 255, 110), 1.5f);
    }
}

class BricksWindow : public Window {
public:
    BricksWindow() : Window("Bricks", kWinW, kWinH, WF_NO_ZOOM) {
        auto* nb = (Button*)add(new Button(Rect(380, 12, 130, 32), "New Game", Button::Primary));
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
        draw_field(c);
    }

    void on_mouse_move(int x, int y, int buttons) override {
        paddle_cx_ = (float)x;
        use_mouse_ = true;
        Window::on_mouse_move(x, y, buttons);
    }

    void on_mouse_down(int x, int y, int button) override {
        if (y >= kFieldY && state_ == Ready) launch();
        Window::on_mouse_down(x, y, button);
        invalidate();
    }

    void on_key(const KeyEvent& e) override {
        int dir = 0;
        bool launch_key = false, pause = false, restart = false;
        if (e.key == Key::Left) dir = -1;
        else if (e.key == Key::Right) dir = 1;
        else if (e.key == Key::Up || e.key == Key::Space) launch_key = true;
        else if (e.key == Key::Char) {
            uint32_t ch = e.ch;
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch == 'a') dir = -1;
            else if (ch == 'd') dir = 1;
            else if (ch == 'p') pause = true;
            else if (ch == 'n' || ch == 'r') restart = true;
        }
        if (restart) {
            new_game();
            return;
        }
        if (pause) {
            if (state_ == Playing) state_ = Paused;
            else if (state_ == Paused) {
                state_ = Playing;
                last_ = timer_ms();
            }
            return;
        }
        if (dir) {
            paddle_x_ += dir * 24.0f;
            held_dir_ = dir;
            held_until_ = timer_ms() + 140;
            use_mouse_ = false;
            clamp_paddle();
            return;
        }
        if (launch_key && state_ == Ready) launch();
    }

    void on_tick(uint64_t now) override {
        uint64_t dt = (last_ == 0 || now < last_) ? 16 : now - last_;
        last_ = now;
        if (dt > 40) dt = 40;
        now_ = now;

        if (state_ == Over) {
            update_particles(dt);
            if (particles_alive()) invalidate();
            return;
        }
        if (state_ == Paused) return;

        if (now < held_until_) {
            paddle_x_ += held_dir_ * 520.0f * (float)dt / 1000.0f;
            use_mouse_ = false;
        }
        clamp_paddle();
        if (state_ == Ready) {
            ball_x_ = paddle_x_ + kPaddleW / 2.0f;
            ball_y_ = kPaddleY - kBallR - 1;
        }
        if (state_ == Playing) {
            int steps = (int)((dt + 7) / 8);
            float h = (float)dt / 1000.0f / (float)steps;
            for (int s = 0; s < steps && state_ == Playing; s++) physics(h);
        }
        update_particles(dt);
        invalidate();
    }

private:
    enum State : uint8_t { Ready, Playing, Paused, Over };

    static constexpr float kFieldL = kFieldX, kFieldR = kFieldX + kFieldW;
    static constexpr float kFieldT = kFieldY;
    static constexpr float kFieldB = kFieldY + kFieldH;

    void new_game() {
        score_ = 0;
        level_ = 1;
        lives_ = 3;
        state_ = Ready;
        paddle_x_ = (kWinW - kPaddleW) / 2.0f;
        paddle_cx_ = kWinW / 2.0f;
        use_mouse_ = false;
        build_bricks();
        reset_ball();
        for (int i = 0; i < kParticles; i++) particle_life_[i] = 0;
        banner_until_ = 0;
        invalidate();
    }

    void build_bricks() {
        bricks_left_ = 0;
        for (int r = 0; r < kRows; r++)
            for (int c = 0; c < kCols; c++) {
                int i = r * kCols + c;
                // from level 2 the top rows take two hits, and more rows each level after that
                int hp = r < level_ - 1 ? 2 : 1;
                if (hp > 2) hp = 2;
                brick_hp_[i] = (uint8_t)hp;
                if (hp) bricks_left_++;
            }
    }

    Rect brick_rect(int i) const {
        int r = i / kCols, c = i % kCols;
        return Rect(kBrickX0 + c * kBrickStepX, kBrickY0 + r * kBrickStepY, kBrickW, kBrickH);
    }

    float speed() const {
        float s = 300.0f + (level_ - 1) * 42.0f;
        return s > 560.0f ? 560.0f : s;
    }

    void clamp_paddle() {
        float lo = kFieldL + 6, hi = kFieldR - 6 - kPaddleW;
        // while the mouse drives the paddle it sits under the pointer
        if (use_mouse_) paddle_x_ = paddle_cx_ - kPaddleW / 2.0f;
        if (paddle_x_ < lo) paddle_x_ = lo;
        if (paddle_x_ > hi) paddle_x_ = hi;
    }

    void reset_ball() {
        state_ = Ready;
        ball_x_ = paddle_x_ + kPaddleW / 2.0f;
        ball_y_ = kPaddleY - kBallR - 1;
        ball_vx_ = ball_vy_ = 0;
    }

    void launch() {
        float a = (float)((int)(cpu_random64() % 1000) - 500) / 1000.0f * 0.5f;
        ball_vx_ = sinf(a) * speed();
        ball_vy_ = -cosf(a) * speed();
        state_ = Playing;
        last_ = timer_ms();
    }

    void physics(float h) {
        ball_x_ += ball_vx_ * h;
        ball_y_ += ball_vy_ * h;

        // side and top walls
        if (ball_x_ - kBallR < kFieldL) {
            ball_x_ = kFieldL + kBallR;
            ball_vx_ = -ball_vx_;
        }
        if (ball_x_ + kBallR > kFieldR) {
            ball_x_ = kFieldR - kBallR;
            ball_vx_ = -ball_vx_;
        }
        if (ball_y_ - kBallR < kFieldT) {
            ball_y_ = kFieldT + kBallR;
            ball_vy_ = -ball_vy_;
        }

        // paddle: the bounce angle depends on where the ball lands
        if (ball_vy_ > 0 && ball_y_ + kBallR >= kPaddleY && ball_y_ + kBallR <= kPaddleY + kPaddleH + 8 &&
            ball_x_ >= paddle_x_ - kBallR && ball_x_ <= paddle_x_ + kPaddleW + kBallR) {
            float u = (ball_x_ - (paddle_x_ + kPaddleW / 2.0f)) / (kPaddleW / 2.0f);
            u = kclamp(u, -1.0f, 1.0f);
            float ang = u * 1.05f;   // up to about 60 degrees from vertical
            float sp = speed();
            ball_vx_ = sinf(ang) * sp;
            ball_vy_ = -cosf(ang) * sp;
            ball_y_ = kPaddleY - kBallR - 0.5f;
        }

        // bricks: one hit per substep
        for (int i = 0; i < kRows * kCols; i++) {
            if (!brick_hp_[i]) continue;
            Rect b = brick_rect(i);
            float bl = (float)b.x, br = (float)b.right(), bt = (float)b.y, bb = (float)b.bottom();
            float cx = kclamp(ball_x_, bl, br), cy = kclamp(ball_y_, bt, bb);
            float dx = ball_x_ - cx, dy = ball_y_ - cy;
            if (dx * dx + dy * dy > kBallR * kBallR) continue;
            float dl = ball_x_ + kBallR - bl, dr = br - (ball_x_ - kBallR);
            float dtop = ball_y_ + kBallR - bt, dbot = bb - (ball_y_ - kBallR);
            float mh = dl < dr ? dl : dr, mv = dtop < dbot ? dtop : dbot;
            if (mh < mv) ball_vx_ = -ball_vx_;
            else ball_vy_ = -ball_vy_;
            hit_brick(i);
            break;
        }

        // ball lost through the bottom
        if (ball_y_ - kBallR > kFieldB) {
            lives_--;
            if (lives_ <= 0) game_over();
            else reset_ball();
            return;
        }
        if (bricks_left_ == 0) next_level();
    }

    void hit_brick(int i) {
        int row = i / kCols;
        if (brick_hp_[i] > 1) {
            brick_hp_[i]--;
            score_ += 5;
            speaker_beep(520, 20);
            return;
        }
        brick_hp_[i] = 0;
        bricks_left_--;
        score_ += 10 * (kRows - row);
        speaker_beep(780 + row * 60, 20);
        Rect b = brick_rect(i);
        spawn_burst((float)(b.x + b.w / 2), (float)(b.y + b.h / 2), kRowCol[row]);
    }

    void next_level() {
        level_++;
        score_ += 100 * level_;
        build_bricks();
        reset_ball();
        banner_until_ = now_ + 1400;
    }

    void game_over() {
        state_ = Over;
        if (score_ > best_) best_ = score_;
        save_best(best_);
        speaker_beep(180, 60);
        for (int k = 0; k < kParticles; k++) particle_life_[k] = 0;
        invalidate();
    }

    // ---- particles
    void spawn_burst(float x, float y, Color col) {
        int made = 0;
        for (int k = 0; k < kParticles && made < 8; k++) {
            if (particle_life_[k] > 0) continue;
            float a = (float)(cpu_random64() % 6283) / 1000.0f;
            float sp = 80.0f + (float)(cpu_random64() % 160);
            part_x_[k] = x;
            part_y_[k] = y;
            part_vx_[k] = cosf(a) * sp;
            part_vy_[k] = sinf(a) * sp - 60.0f;
            particle_life_[k] = 1;
            part_col_[k] = col;
            made++;
        }
    }

    void update_particles(uint64_t dt) {
        float h = (float)dt / 1000.0f;
        for (int k = 0; k < kParticles; k++) {
            if (particle_life_[k] <= 0) continue;
            part_x_[k] += part_vx_[k] * h;
            part_y_[k] += part_vy_[k] * h;
            part_vy_[k] += 420.0f * h;
            particle_life_[k] -= h * 2.2f;
        }
    }

    bool particles_alive() const {
        for (int k = 0; k < kParticles; k++)
            if (particle_life_[k] > 0) return true;
        return false;
    }

    // ---- painting
    Rect cell_pill(int x, int y, int w) const { return Rect(x, y, w, 32); }

    void draw_status(Canvas& c) {
        const Theme& t = theme();
        Color pill = t.dark ? rgba(255, 255, 255, 26) : rgba(0, 0, 0, 14);
        char buf[24];
        Rect sc = cell_pill(10, 12, 150), lv = cell_pill(170, 12, 100);
        c.fill_round_rect(sc, 16, pill);
        c.fill_round_rect(lv, 16, pill);
        c.draw_text_in(Rect(sc.x + 14, sc.y, 60, sc.h), "Score", t.text_secondary, 12, Font::Medium);
        snprintf(buf, sizeof(buf), "%d", score_);
        c.draw_text_in(Rect(sc.x + 60, sc.y, sc.w - 74, sc.h), buf, t.text, 15, Font::Mono, Align::Right);
        c.draw_text_in(Rect(lv.x + 14, lv.y, 50, lv.h), "Level", t.text_secondary, 12, Font::Medium);
        snprintf(buf, sizeof(buf), "%d", level_);
        c.draw_text_in(Rect(lv.x + 56, lv.y, lv.w - 70, lv.h), buf, t.text, 15, Font::Mono, Align::Right);

        // lives as small balls
        for (int i = 0; i < 3; i++) {
            float x = 294.0f + i * 22.0f, y = 28.0f;
            if (i < lives_) {
                c.fill_circle(x, y, 7.0f, hex(0xFFFFFF));
                c.fill_circle(x - 2.0f, y - 2.0f, 2.0f, rgba(255, 255, 255, 200));
            } else {
                c.stroke_circle(x, y, 6.0f, rgba(255, 255, 255, 70), 1.5f);
            }
        }
    }

    void draw_field(Canvas& c) {
        Rect panel(kFieldX, kFieldY, kFieldW, kFieldH);
        c.draw_shadow(panel, 18, 14, rgba(0, 0, 0, 80), 0, 6);
        c.fill_gradient(panel, hex(0x1F2448), hex(0x0D1026), 18);
        c.stroke_round_rect(panel, 18, rgba(255, 255, 255, 30), 1);

        for (int i = 0; i < kRows * kCols; i++) {
            if (!brick_hp_[i]) continue;
            int row = i / kCols;
            draw_brick(c, brick_rect(i), kRowCol[row], brick_hp_[i] > 1);
        }

        // particles
        for (int k = 0; k < kParticles; k++) {
            if (particle_life_[k] <= 0) continue;
            int a = (int)(255.0f * (particle_life_[k] > 1 ? 1 : particle_life_[k]));
            c.fill_circle(part_x_[k], part_y_[k], 2.6f, with_alpha(part_col_[k], a));
        }

        // paddle
        Rect pad((int)paddle_x_, kPaddleY, kPaddleW, kPaddleH);
        c.fill_round_rect(Rect(pad.x, pad.y + 3, pad.w, pad.h), 6, rgba(0, 0, 0, 70));
        c.fill_gradient(pad, hex(0xF4F6FF), hex(0x8E9CC6), 6);
        c.fill_round_rect(Rect(pad.x + 6, pad.y + 2, pad.w - 12, 3), 1.5f, rgba(255, 255, 255, 150));

        // ball with a soft glow
        if (state_ != Over) {
            c.fill_circle(ball_x_, ball_y_, kBallR + 5.0f, rgba(255, 255, 255, 36));
            c.fill_circle(ball_x_, ball_y_, kBallR, hex(0xFFFFFF));
            c.fill_circle(ball_x_ - 2.2f, ball_y_ - 2.2f, 2.4f, rgba(255, 255, 255, 230));
        }

        uint64_t banner_left = banner_until_ > now_ ? banner_until_ - now_ : 0;
        if (banner_left > 0 && state_ != Over) {
            char buf[24];
            snprintf(buf, sizeof(buf), "Level %d", level_);
            int a = banner_left < 400 ? (int)(255.0f * banner_left / 400.0f) : 255;
            c.draw_text_in(Rect(kFieldX, 250, kFieldW, 40), buf, rgba(255, 255, 255, a), 30, Font::Bold, Align::Center);
        }

        if (state_ == Ready) draw_card(c, "Ready", "Click, Space or Up to launch", hex(0xFFFFFF));
        else if (state_ == Paused) draw_card(c, "Paused", "Press P to continue", hex(0xFFFFFF));
        else if (state_ == Over) draw_card(c, "Game over", "Press N or New Game to retry", hex(0xFF8A8A));
    }

    void draw_card(Canvas& c, const char* title, const char* hint, Color col) {
        Rect card(kFieldX + 60, kFieldY + 300, kFieldW - 120, 120);
        c.fill_round_rect(card, 22, rgba(12, 14, 34, 215));
        c.stroke_round_rect(card, 22, rgba(255, 255, 255, 60), 1);
        c.draw_text_in(Rect(card.x, card.y + 16, card.w, 36), title, col, 26, Font::Bold, Align::Center);
        char buf[48];
        snprintf(buf, sizeof(buf), "Score %d   ·   Best %d", score_, best_);
        c.draw_text_in(Rect(card.x, card.y + 56, card.w, 20), buf, rgba(255, 255, 255, 230), 14, Font::Medium,
                       Align::Center);
        c.draw_text_in(Rect(card.x, card.y + 86, card.w, 18), hint, rgba(255, 255, 255, 160), 12, Font::Regular,
                       Align::Center);
    }

    State state_ = Ready;
    int score_ = 0, best_ = 0, level_ = 1, lives_ = 3;
    int bricks_left_ = 0;
    uint8_t brick_hp_[kRows * kCols] = {};
    float paddle_x_ = 0, paddle_cx_ = kWinW / 2.0f;
    float ball_x_ = 0, ball_y_ = 0, ball_vx_ = 0, ball_vy_ = 0;
    int held_dir_ = 0;
    bool use_mouse_ = false;
    uint64_t held_until_ = 0;
    uint64_t last_ = 0, now_ = 0, banner_until_ = 0;
    float part_x_[kParticles] = {}, part_y_[kParticles] = {};
    float part_vx_[kParticles] = {}, part_vy_[kParticles] = {};
    float particle_life_[kParticles] = {};
    Color part_col_[kParticles] = {};
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0xB36BFF), hex(0x5A2A9C));
    // a few colored bricks across the top, a paddle and a ball
    int bw = r.w / 4, bh = r.h / 9, gap = r.w / 40;
    int x0 = r.x + (r.w - (3 * bw + 2 * gap)) / 2;
    const Color cols[3] = {hex(0xFF6B8B), hex(0xFFD43B), hex(0x69DB7C)};
    for (int row = 0; row < 2; row++)
        for (int k = 0; k < 3; k++) {
            Rect b(x0 + k * (bw + gap), r.y + r.h / 5 + row * (bh + gap), bw, bh);
            draw_brick(c, b, cols[(k + row) % 3], false);
        }
    int pw = r.w / 2, ph = r.h / 14;
    Rect pad(r.x + (r.w - pw) / 2, r.y + r.h * 0.8f, pw, ph);
    c.fill_gradient(pad, hex(0xF4F6FF), hex(0x9AA6CC), ph / 2);
    c.fill_circle(r.x + r.w / 2.0f + r.w * 0.12f, r.y + r.h * 0.6f, r.w * 0.06f, hex(0xFFFFFF));
}

Window* create() { return new BricksWindow(); }

AppRegistrar reg({"bricks", "Bricks", icon, create, "Games", true, false, 124, "Break every brick with the ball"});

}  // namespace
