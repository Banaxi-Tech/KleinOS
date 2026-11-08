// Piano: two-octave keyboard. Mouse (hold to sustain), computer keys, record / play
// back, and a built-in demo of "Ode to Joy" (Beethoven, public domain).
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "drivers/power.h"
#include "arch/interrupts.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

constexpr int kWinW = 760, kWinH = 382;
constexpr int kKeyX = 16, kKeyY = 84, kWhiteW = 52, kWhiteH = 218, kBlackW = 34, kBlackH = 136;
constexpr int kWhites = 14, kKeys = 24;
constexpr uint32_t kUnitMs = 220;     // one eighth note in the demo
constexpr uint32_t kKeyBeepMs = 280;  // computer keys: no release events, so use a fixed length
constexpr uint32_t kStuckMs = 6000;   // release a mouse-held note that never got its mouse-up

struct KeyDef {
    uint32_t hz;
    bool black;
    int pos;        // white key index this key sits on (black: left neighbour)
    char map;       // computer keyboard character (0 = none)
    const char* label;
};

// Index 0..13 are the white keys C4..B5; 14..23 are the black keys.
const KeyDef kKeyDefs[kKeys] = {
    {261, false, 0, 'a', "C4"},  {293, false, 1, 's', "D4"},  {329, false, 2, 'd', "E4"},
    {349, false, 3, 'f', "F4"},  {392, false, 4, 'g', "G4"},  {440, false, 5, 'h', "A4"},
    {493, false, 6, 'j', "B4"},  {523, false, 7, 'k', "C5"},  {587, false, 8, 'l', "D5"},
    {659, false, 9, ';', "E5"},  {698, false, 10, 0, "F5"},   {784, false, 11, 0, "G5"},
    {880, false, 12, 0, "A5"},   {987, false, 13, 0, "B5"},
    {277, true, 0, 'w', "C#4"},  {311, true, 1, 'e', "D#4"},  {370, true, 3, 't', "F#4"},
    {415, true, 4, 'y', "G#4"},  {466, true, 5, 'u', "A#4"},  {554, true, 7, 'o', "C#5"},
    {622, true, 8, 'p', "D#5"},  {740, true, 10, 0, "F#5"},   {831, true, 11, 0, "G#5"},
    {932, true, 12, 0, "A#5"},
};

// Ode to Joy: {white key index, length in eighth notes}
struct Step { int key; int units; };
const Step kOdeToJoy[] = {
    {2, 2}, {2, 2}, {3, 2}, {4, 2}, {4, 2}, {3, 2}, {2, 2}, {1, 2}, {0, 2}, {0, 2}, {1, 2}, {2, 2}, {2, 3}, {1, 1}, {1, 4},
    {2, 2}, {2, 2}, {3, 2}, {4, 2}, {4, 2}, {3, 2}, {2, 2}, {1, 2}, {0, 2}, {0, 2}, {1, 2}, {2, 2}, {1, 3}, {0, 1}, {0, 4},
    {1, 2}, {1, 2}, {2, 2}, {0, 2}, {1, 2}, {2, 1}, {3, 1}, {2, 2}, {0, 2}, {1, 2}, {2, 1}, {3, 1}, {2, 2}, {1, 2},
    {0, 2}, {1, 2}, {0, 4},
};

uint32_t key_hz(int k) { return kKeyDefs[k].hz; }

bool is_black(int k) { return kKeyDefs[k].black; }

Rect white_rect(int pos) { return Rect(kKeyX + pos * kWhiteW, kKeyY, kWhiteW, kWhiteH); }
Rect black_rect(int pos) { return Rect(kKeyX + (pos + 1) * kWhiteW - kBlackW / 2, kKeyY, kBlackW, kBlackH); }
Rect key_rect(int k) { return is_black(k) ? black_rect(kKeyDefs[k].pos) : white_rect(kKeyDefs[k].pos); }

struct Ev {
    int key;
    uint32_t start;
    uint32_t dur;
};

class PianoWindow : public Window {
public:
    PianoWindow() : Window("Piano", kWinW, kWinH, WF_NO_ZOOM) {
        rec_btn_ = (Button*)add(new Button(Rect(16, 14, 116, 32), "Record"));
        rec_btn_->icon = [](Canvas& c, Rect r) { c.fill_circle(r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w / 2.5f, hex(0xFF3B30)); };
        play_btn_ = (Button*)add(new Button(Rect(140, 14, 96, 32), "Play"));
        demo_btn_ = (Button*)add(new Button(Rect(244, 14, 96, 32), "Demo", Button::Primary));
        clear_btn_ = (Button*)add(new Button(Rect(348, 14, 84, 32), "Clear"));

        rec_btn_->on_click = [this] { toggle_record(); };
        play_btn_->on_click = [this] { toggle_play(&rec_); };
        demo_btn_->on_click = [this] { toggle_play(&demo_); };
        clear_btn_->on_click = [this] { if (!playing_) { rec_.clear(); invalidate(); } };

        for (const Step& s : kOdeToJoy) {
            demo_.push(Ev{s.key, demo_len_, (uint32_t)(s.units * kUnitMs * 92 / 100)});
            demo_len_ += s.units * kUnitMs;
        }
        set_ticking(true);
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        const Theme& t = theme();
        uint32_t now = timer_ms();

        // status line
        char status[96];
        if (recording_) snprintf(status, sizeof(status), "Recording  %u notes", (unsigned)rec_.size());
        else if (playing_) snprintf(status, sizeof(status), "Playing...");
        else if (rec_.size()) snprintf(status, sizeof(status), "%u notes recorded", (unsigned)rec_.size());
        else snprintf(status, sizeof(status), "Click keys or use the keyboard");
        c.draw_text_in(Rect(460, 14, kWinW - 476, 32), status, recording_ ? hex(0xFF3B30) : t.text_secondary, 13,
                       recording_ ? Font::Medium : Font::Regular, Align::Right);

        // case
        c.draw_shadow(Rect(8, kKeyY - 10, kWinW - 16, kWhiteH + 20), 14, 10, rgba(0, 0, 0, 90), 0, 4);
        c.fill_gradient(Rect(8, kKeyY - 10, kWinW - 16, kWhiteH + 20), hex(0x2A2A2E), hex(0x111114), 14);
        c.stroke_round_rect(Rect(8, kKeyY - 10, kWinW - 16, kWhiteH + 20), 14, rgba(255, 255, 255, 25), 1);

        // white keys
        for (int k = 0; k < kWhites; k++) draw_white(c, k, is_lit(k, now));
        // black keys on top
        for (int k = kWhites; k < kKeys; k++) draw_black(c, k, is_lit(k, now));

        c.draw_text_in(Rect(16, 320, kWinW - 32, 18),
                       "Keys:  A S D F G H J K L ;  for white,   W E T Y U O P  for black", t.text_secondary, 12,
                       Font::Regular, Align::Center);
        c.draw_text_in(Rect(16, 344, kWinW - 32, 18),
                       "Hold the mouse on a key to sustain it, drag to glide between keys.", t.text_tertiary, 12,
                       Font::Regular, Align::Center);
    }

    void on_mouse_down(int x, int y, int button) override {
        if (button == MB_LEFT && y >= kKeyY && y < kKeyY + kWhiteH) {
            int k = key_at(x, y);
            if (k >= 0) {
                focus_widget(nullptr);
                press(k, true);
                invalidate();
                return;
            }
        }
        Window::on_mouse_down(x, y, button);
    }

    void on_mouse_move(int x, int y, int buttons) override {
        if (mouse_key_ >= 0) {
            if (!(buttons & MB_LEFT)) { release_mouse(); invalidate(); return; }
            int k = key_at(x, y);
            if (k >= 0 && k != mouse_key_) { release_mouse(); press(k, true); invalidate(); }
            return;
        }
        Window::on_mouse_move(x, y, buttons);
    }

    void on_mouse_up(int x, int y, int button) override {
        if (mouse_key_ >= 0 && button == MB_LEFT) { release_mouse(); invalidate(); return; }
        Window::on_mouse_up(x, y, button);
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Char && !e.cmd()) {
            uint32_t ch = e.ch;
            if (ch >= 'A' && ch <= 'Z') ch |= 0x20;
            for (int k = 0; k < kKeys; k++) {
                if (kKeyDefs[k].map && kKeyDefs[k].map == ch) {
                    press(k, false);
                    invalidate();
                    return;
                }
            }
        }
        Window::on_key(e);
    }

    void on_tick(uint64_t) override {
        uint32_t now = timer_ms();
        if (mouse_key_ >= 0 && now - mouse_t0_ > kStuckMs) { release_mouse(); }
        if (playing_) {
            const Vector<Ev>& src = *play_src_;
            uint32_t el = now - play_t0_;
            while (play_idx_ < src.size() && src[play_idx_].start <= el) {
                const Ev& e = src[play_idx_];
                speaker_beep(key_hz(e.key), e.dur);
                lit_until_[e.key] = now + e.dur;
                play_idx_++;
            }
            if (play_idx_ >= src.size() && (src.empty() || el > src.back().start + src.back().dur + 200)) {
                stop_playing();
            }
        }
        uint32_t mask = 0;
        for (int k = 0; k < kKeys; k++) if (is_lit(k, now)) mask |= 1u << k;
        if (mask != last_mask_) { last_mask_ = mask; invalidate(); }
    }

private:
    bool is_lit(int k, uint32_t now) const {
        return k == mouse_key_ || now < lit_until_[k];
    }

    int key_at(int x, int y) const {
        for (int k = kWhites; k < kKeys; k++)
            if (black_rect(kKeyDefs[k].pos).contains(x, y)) return k;
        for (int k = 0; k < kWhites; k++)
            if (white_rect(kKeyDefs[k].pos).contains(x, y)) return k;
        return -1;
    }

    void draw_white(Canvas& c, int k, bool lit) {
        Rect r = white_rect(kKeyDefs[k].pos);
        if (lit) r.y += 2;
        c.draw_shadow(Rect(r.x, r.y, r.w, r.h - 2), 6, 4, rgba(0, 0, 0, lit ? 40 : 110), 0, lit ? 1 : 3);
        Color top = lit ? hex(0xD9D9DE) : hex(0xFFFFFF);
        Color bot = lit ? hex(0xEFEFF2) : hex(0xE2E2E7);
        c.fill_gradient(Rect(r.x, r.y, r.w, r.h - 2), top, bot, 6);
        c.fill_rect(Rect(r.x + 2, r.y + 2, 1, r.h - 10), rgba(0, 0, 0, 14));
        c.fill_rect(Rect(r.right() - 3, r.y + 2, 1, r.h - 10), rgba(0, 0, 0, 14));
        c.stroke_round_rect(Rect(r.x, r.y, r.w, r.h - 2), 6, rgba(0, 0, 0, 70), 1);
        const char* lbl = kKeyDefs[k].label;
        if (lbl[1] == '4' || lbl[1] == '5') {
            if (lbl[0] == 'C') c.draw_text_in(Rect(r.x, r.bottom() - 46, r.w, 16), lbl, hex(0x6E6E73), 11, Font::Medium, Align::Center);
        }
        if (kKeyDefs[k].map) {
            char letter[2] = {(char)(kKeyDefs[k].map == ';' ? ';' : kKeyDefs[k].map - 32), 0};
            c.draw_text_in(Rect(r.x, r.bottom() - 26, r.w, 16), letter, hex(0x8E8E93), 12, Font::Medium, Align::Center);
        }
    }

    void draw_black(Canvas& c, int k, bool lit) {
        Rect r = black_rect(kKeyDefs[k].pos);
        if (lit) r.y += 1;
        c.draw_shadow(r, 5, 5, rgba(0, 0, 0, 150), 0, 4);
        Color top = lit ? hex(0x3A3A3E) : hex(0x2C2C2E);
        Color bot = lit ? hex(0x111113) : hex(0x000000);
        c.fill_gradient(r, top, bot, 5);
        c.fill_gradient(Rect(r.x + 3, r.y + 2, r.w - 6, r.h / 3), rgba(255, 255, 255, lit ? 22 : 45), rgba(255, 255, 255, 0), 4);
        c.stroke_round_rect(r, 5, rgba(255, 255, 255, 25), 1);
        if (kKeyDefs[k].map) {
            char letter[2] = {(char)(kKeyDefs[k].map == ';' ? ';' : kKeyDefs[k].map - 32), 0};
            c.draw_text_in(Rect(r.x, r.bottom() - 26, r.w, 16), letter, hex(0x8E8E93), 11, Font::Medium, Align::Center);
        }
    }

    void press(int k, bool from_mouse) {
        uint32_t now = timer_ms();
        speaker_tone(0);
        if (from_mouse) {
            speaker_tone(key_hz(k));
            mouse_key_ = k;
            mouse_t0_ = now;
        } else {
            speaker_beep(key_hz(k), kKeyBeepMs);
        }
        if (recording_) {
            Ev e{k, now - rec_t0_, from_mouse ? 0 : kKeyBeepMs};
            rec_.push(e);
            held_ev_ = from_mouse ? (int)rec_.size() - 1 : -1;
        }
        lit_until_[k] = now + 180;
    }

    void release_mouse() {
        if (mouse_key_ < 0) return;
        speaker_tone(0);
        if (recording_ && held_ev_ >= 0 && held_ev_ < (int)rec_.size()) {
            Ev& e = rec_[held_ev_];
            uint32_t len = timer_ms() - rec_t0_ - e.start;
            e.dur = len < 60 ? 60 : len;
        }
        held_ev_ = -1;
        mouse_key_ = -1;
    }

    void toggle_record() {
        if (recording_) {
            recording_ = false;
            rec_btn_->text = "Record";
        } else {
            if (playing_) stop_playing();
            rec_.clear();
            rec_t0_ = timer_ms();
            recording_ = true;
            rec_btn_->text = "Stop";
        }
        invalidate();
    }

    void toggle_play(const Vector<Ev>* src) {
        if (playing_) { stop_playing(); return; }
        if (recording_) toggle_record();
        if (src->empty()) return;
        play_src_ = src;
        play_idx_ = 0;
        play_t0_ = timer_ms();
        playing_ = true;
        (src == &demo_ ? demo_btn_ : play_btn_)->text = "Stop";
        invalidate();
    }

    void stop_playing() {
        playing_ = false;
        speaker_tone(0);
        play_btn_->text = "Play";
        demo_btn_->text = "Demo";
        invalidate();
    }

    Button* rec_btn_ = nullptr;
    Button* play_btn_ = nullptr;
    Button* demo_btn_ = nullptr;
    Button* clear_btn_ = nullptr;

    Vector<Ev> rec_;
    Vector<Ev> demo_;
    uint32_t demo_len_ = 0;
    bool recording_ = false;
    uint32_t rec_t0_ = 0;
    int held_ev_ = -1;

    bool playing_ = false;
    const Vector<Ev>* play_src_ = nullptr;
    size_t play_idx_ = 0;
    uint32_t play_t0_ = 0;

    int mouse_key_ = -1;
    uint32_t mouse_t0_ = 0;
    uint32_t lit_until_[kKeys] = {};
    uint32_t last_mask_ = 0;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x3A3A3E), hex(0x0A0A0C));
    float u = r.w / 100.0f;
    // white keys
    for (int i = 0; i < 4; i++) {
        Rect k(r.x + (int)((18 + i * 17) * u), r.y + (int)(40 * u), (int)(16 * u), (int)(40 * u));
        c.fill_round_rect(k, 2 * u, hex(0xFFFFFF));
    }
    // black keys
    int bx[3] = {27, 44, 61};
    for (int i = 0; i < 3; i++) {
        Rect k(r.x + (int)(bx[i] * u), r.y + (int)(40 * u), (int)(11 * u), (int)(24 * u));
        c.fill_round_rect(k, 2 * u, hex(0x111111));
    }
}

Window* create() { return new PianoWindow(); }

AppRegistrar reg({"piano", "Piano", icon, create, "Media", true, false, 80, "Play notes on a two-octave keyboard"});

}  // namespace
