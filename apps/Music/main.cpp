// Music: plays the WAV files in ~/Music (and any opened from Files), with a built-in
// piano sample so there is something to hear on a new computer. Glass sidebar with
// the library, a now-playing pane with artwork, seek bar, transport and volume.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "fs/vfs.h"
#include "sys/session.h"
#include "drivers/sound.h"
#include "arch/interrupts.h"
#include "lib/heap.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

constexpr int kSidebarW = 230;

struct Track {
    String title, path;      // path empty = the built-in sample
    uint32_t seconds = 0;    // 0 = unknown
};

String music_dir() { return String::format("/Users/%s/Music", session_user()); }

// Length of a WAV from its header, without decoding it.
uint32_t wav_seconds(const uint8_t* d, size_t size) {
    if (size < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    uint32_t rate = 0, align = 0, data = 0;
    for (size_t o = 12; o + 8 <= size;) {
        uint32_t len = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (uint32_t)d[o + 7] << 24;
        if (!memcmp(d + o, "fmt ", 4) && o + 24 <= size) {
            rate = d[o + 12] | d[o + 13] << 8 | d[o + 14] << 16 | (uint32_t)d[o + 15] << 24;
            align = d[o + 20] | d[o + 21] << 8;
        } else if (!memcmp(d + o, "data", 4)) {
            data = len;
        }
        o += 8 + len + (len & 1);
    }
    return rate && align ? (uint32_t)((uint64_t)data / align / rate) : 0;
}

String clock_text(uint32_t s) { return String::format("%u:%02u", s / 60, s % 60); }

// ---- the built-in sample: an original little piano piece (I-V-vi-IV in C)
struct NoteEv { float beat; int midi; float len, vel; };

bool compose_sample(int16_t** out, uint32_t* out_frames) {
    const float kBeat = 0.3f;   // seconds per eighth note (100 bpm)
    static const int chords[4][5] = {{48, 60, 64, 67, 72}, {43, 59, 62, 67, 71}, {45, 60, 64, 69, 72}, {41, 60, 65, 69, 72}};
    static const int melody[3][8] = {{76, 79, 74, 79, 76, 72, 77, 81}, {79, 76, 74, 71, 72, 76, 77, 74}, {76, 79, 83, 79, 81, 76, 77, 72}};
    static const int arp[8] = {1, 2, 3, 4, 3, 2, 3, 2};
    Vector<NoteEv> evs;
    for (int bar = 0; bar < 16; bar++) {
        const int* ch = chords[bar % 4];
        float b0 = bar * 8.0f;
        evs.push(NoteEv{b0, ch[0], 8, 0.42f});
        evs.push(NoteEv{b0 + 4, ch[0] + 12, 4, 0.30f});
        for (int i = 0; i < 8; i++) evs.push(NoteEv{b0 + i, ch[arp[i]], 2, i == 0 ? 0.30f : 0.22f});
        if (bar >= 4) {
            int pass = bar / 4 - 1, k = (bar % 4) * 2;
            evs.push(NoteEv{b0, melody[pass][k], 4, 0.5f});
            evs.push(NoteEv{b0 + 4, melody[pass][k + 1], 4, 0.45f});
        }
    }
    for (int m : {48, 55, 60, 64, 67, 72, 76}) evs.push(NoteEv{128, m, 16, m == 76 ? 0.45f : 0.3f});   // last chord

    uint32_t note_frames = kSoundRate * 2;
    uint32_t frames = (uint32_t)((128 * kBeat + 4.0f) * kSoundRate);
    int32_t* mix = (int32_t*)kmalloc((size_t)frames * 2 * sizeof(int32_t));
    if (!mix) return false;
    memset(mix, 0, (size_t)frames * 2 * sizeof(int32_t));
    int16_t* cache[128] = {};
    for (const NoteEv& e : evs) {
        if (!cache[e.midi]) cache[e.midi] = sound_render_piano(440.0f * powf(2.0f, (e.midi - 69) / 12.0f), note_frames, 0.8f);
        const int16_t* n = cache[e.midi];
        if (!n) continue;
        uint32_t at = (uint32_t)(e.beat * kBeat * kSoundRate);
        uint32_t len = kmin((uint32_t)(e.len * kBeat * kSoundRate) + kSoundRate / 3, note_frames);   // ring a little past the note
        int vel = (int)(e.vel * 256);
        int pan = (e.midi - 60) * 3;   // low notes a little left, high notes right
        int left = kclamp(256 - pan, 128, 320), right = kclamp(256 + pan, 128, 320);
        for (uint32_t i = 0; i < len && at + i < frames; i++) {
            int32_t v = n[i] * vel >> 8;
            if (i + 2400 > len) v = v * (int32_t)(len - i) / 2400;   // damper: 50 ms fade
            mix[(at + i) * 2] += v * left >> 8;
            mix[(at + i) * 2 + 1] += v * right >> 8;
        }
    }
    for (int16_t* c : cache) kfree(c);
    int16_t* buf = (int16_t*)kmalloc((size_t)frames * 4);
    if (!buf) { kfree(mix); return false; }
    for (uint32_t i = 0; i < frames * 2; i++) buf[i] = (int16_t)kclamp(mix[i] * 5 / 8, -32768, 32767);
    kfree(mix);
    *out = buf;
    *out_frames = frames;
    return true;
}

class MusicWindow;
MusicWindow* g_win;

class MusicWindow : public Window {
public:
    MusicWindow() : Window("Music", 780, 470) {
        g_win = this;
        set_app_id("music");
        set_min_size(620, 420);
        set_sidebar_width(kSidebarW);
        set_ticking(true);
        list_ = (ListView*)add(new ListView(Rect(8, 44, kSidebarW - 16, height() - 52)));
        list_->transparent = true;
        list_->row_height = 44;
        list_->on_activate = [this](int i) { select(i, true); };
        list_->on_select = [this](int i) { if (i != current_) select(i, false); };
        volume_ = (Slider*)add(new Slider(Rect(0, 0, 140, 22), sound_volume() / 100.0f));
        volume_->on_change = [](float v) { sound_set_volume((int)(v * 100 + 0.5f)); };
        scan();
        layout();
    }

    ~MusicWindow() override {
        stop_voice();
        kfree(buf_);
        if (g_win == this) g_win = nullptr;
    }

    void open_path(const char* path) {
        for (int i = 0; i < (int)tracks_.size(); i++)
            if (tracks_[i].path == path) { select(i, true); return; }
        add_track(path);
        refresh_list();
        select((int)tracks_.size() - 1, true);
    }

    void on_resize() override { layout(); }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        const Theme& t = theme();
        c.draw_text(20, 16, "Library", t.text_secondary, 12, Font::Bold);
        Rect pane = pane_rect();
        if (current_ < 0) {
            c.draw_text_in(pane, "Choose something to play", t.text_tertiary, 15, Font::Medium, Align::Center);
            return;
        }
        const Track& tr = tracks_[current_];
        // artwork: a soft gradient picked from the title, with a note on it
        Rect art = art_rect();
        uint32_t h = 2166136261u;
        for (const char* p = tr.title.c_str(); *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        static const uint32_t pal[][2] = {{0xFF7A59, 0xC2185B}, {0x5AC8FA, 0x5856D6}, {0x34C759, 0x0A84FF},
                                          {0xFFCC00, 0xFF2D55}, {0xBF5AF2, 0x5E5CE6}, {0x64D2FF, 0x30B0C7}};
        const uint32_t* pc = pal[h % 6];
        c.draw_shadow(art, 18, 22, rgba(0, 0, 0, 70), 0, 10);
        c.fill_gradient(art, hex(pc[0]), hex(pc[1]), 18);
        int sym = art.w * 2 / 5;
        draw_symbol(c, Rect(art.x + (art.w - sym) / 2, art.y + (art.h - sym) / 2, sym, sym), Symbol::Music, rgba(255, 255, 255, 230), 3.0f);

        int y = art.bottom() + 22;
        c.draw_text_in(Rect(pane.x + 20, y, pane.w - 40, 26), tr.title.c_str(), t.text, 20, Font::Bold, Align::Center);
        const char* sub = tr.path.empty() ? "KleinOS Sample  \xC2\xB7  Piano" : "WAV audio";
        c.draw_text_in(Rect(pane.x + 20, y + 28, pane.w - 40, 18), sub, t.text_secondary, 13, Font::Regular, Align::Center);

        if (!error_.empty()) {
            c.draw_text_in(Rect(pane.x + 20, y + 64, pane.w - 40, 18), error_.c_str(), hex(0xFF3B30), 13, Font::Medium, Align::Center);
            return;
        }
        if (!sound_available()) {
            c.draw_text_in(Rect(pane.x + 20, y + 64, pane.w - 40, 18), "This computer has no sound device KleinOS can use.",
                           t.text_tertiary, 13, Font::Medium, Align::Center);
            return;
        }
        // seek bar
        Rect bar = seek_rect();
        uint32_t total = total_frames(), pos = position();
        float f = total ? (float)pos / total : 0.0f;
        c.fill_round_rect(Rect(bar.x, bar.y + 7, bar.w, 6), 3, t.dark ? rgba(255, 255, 255, 40) : rgba(0, 0, 0, 22));
        int fw = (int)(bar.w * f);
        if (fw > 0) c.fill_round_rect(Rect(bar.x, bar.y + 7, fw, 6), 3, t.accent);
        c.fill_circle(bar.x + fw, bar.y + 10, seeking_ ? 8 : 6, t.dark ? hex(0xFFFFFF) : hex(0xFFFFFF));
        c.stroke_circle(bar.x + fw, bar.y + 10, seeking_ ? 8 : 6, rgba(0, 0, 0, 50), 1);
        c.draw_text(bar.x, bar.y + 22, clock_text(pos / kSoundRate).c_str(), t.text_secondary, 12);
        String rest = String("-") + clock_text((total - kmin(pos, total)) / kSoundRate);
        c.draw_text_in(Rect(bar.x, bar.y + 22, bar.w, 16), rest.c_str(), t.text_secondary, 12, Font::Regular, Align::Right);

        // transport
        Rect pr = button_rect(0), pl = button_rect(1), nx = button_rect(2);
        c.fill_circle(pl.x + pl.w / 2, pl.y + pl.h / 2, pl.w / 2, t.accent);
        draw_symbol(c, pl.inset(16), playing_ ? Symbol::Pause : Symbol::Play, hex(0xFFFFFF), 2.4f);
        draw_symbol(c, pr.inset(8), Symbol::SkipBack, t.text, 2.0f);
        draw_symbol(c, nx.inset(8), Symbol::SkipForward, t.text, 2.0f);
        Rect vr = volume_->bounds;
        draw_symbol(c, Rect(vr.x - 26, vr.y + 2, 18, 18), Symbol::Volume, t.text_secondary, 1.6f);
    }

    void on_mouse_down(int x, int y, int button) override {
        if (button == MB_LEFT && current_ >= 0 && sound_available() && error_.empty()) {
            if (seek_rect().expand(6).contains(x, y)) { seeking_ = true; seek_to_x(x, false); return; }
            for (int i = 0; i < 3; i++)
                if (button_rect(i).contains(x, y)) {
                    if (i == 0) previous();
                    else if (i == 1) toggle();
                    else next(false);
                    return;
                }
        }
        Window::on_mouse_down(x, y, button);
    }
    void on_mouse_move(int x, int y, int buttons) override {
        if (seeking_) { if (buttons & MB_LEFT) seek_to_x(x, false); else { seeking_ = false; seek_to_x(x, true); } return; }
        Window::on_mouse_move(x, y, buttons);
    }
    void on_mouse_up(int x, int y, int button) override {
        if (seeking_) { seeking_ = false; seek_to_x(x, true); return; }
        Window::on_mouse_up(x, y, button);
    }

    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Space) { toggle(); return; }
        if ((e.key == Key::Left || e.key == Key::Right) && current_ >= 0 && loaded_ == current_) {
            int32_t step = (e.key == Key::Right ? 5 : -5) * kSoundRate;
            int64_t to = (int64_t)position() + step;
            seek((uint32_t)kclamp(to, (int64_t)0, (int64_t)total_frames()), true);
            return;
        }
        Window::on_key(e);
    }

    void on_tick(uint64_t) override {
        if (playing_ && !sound_playing(voice_)) {   // reached the end
            playing_ = false;
            voice_ = 0;
            off_ = total_frames();
            next(true);
        }
        // repaint when the seek knob moves a pixel or the time changes a second
        uint32_t pos = position(), total = total_frames();
        int px = total ? (int)((uint64_t)pos * seek_rect().w / total) : 0;
        uint32_t key = (uint32_t)px << 16 | (pos / kSoundRate & 0xFFFF);
        if (key != last_key_) { last_key_ = key; invalidate(); }
        float v = sound_muted() ? 0.0f : sound_volume() / 100.0f;
        if (!volume_->dragging && fabsf(volume_->value - v) > 0.001f) { volume_->value = v; volume_->invalidate(); }
    }

    void build_menus(Vector<Menu>& menus) override {
        Menu& m = menus.emplace();
        m.title = "Controls";
        m.add(playing_ ? "Pause" : "Play", [this] { toggle(); }, "Space");
        m.add("Next", [this] { next(false); });
        m.add("Previous", [this] { previous(); });
        m.separator();
        m.add("Show Music Folder", [] { app_open_file(music_dir().c_str()); });
    }

private:
    Rect pane_rect() const { return Rect(kSidebarW, 0, width() - kSidebarW, height()); }
    Rect art_rect() const {
        Rect p = pane_rect();
        int s = kclamp(height() - 270, 100, 240);   // everything below the artwork takes 270 px
        return Rect(p.x + (p.w - s) / 2, 30, s, s);
    }
    Rect seek_rect() const {
        Rect p = pane_rect();
        int w = kmin(p.w - 80, 420);
        return Rect(p.x + (p.w - w) / 2, art_rect().bottom() + 76, w, 20);
    }
    Rect button_rect(int i) const {   // previous, play, next
        Rect p = pane_rect();
        int cx = p.x + p.w / 2, y = seek_rect().bottom() + 22;
        if (i == 1) return Rect(cx - 28, y, 56, 56);
        return Rect(cx + (i == 0 ? -100 : 64), y + 10, 36, 36);
    }

    void layout() {
        list_->bounds = Rect(8, 36, kSidebarW - 16, height() - 44);
        Rect b = button_rect(1);
        Rect p = pane_rect();
        volume_->bounds = Rect(p.x + (p.w - 150) / 2 + 13, b.bottom() + 14, 150, 22);
        volume_->visible = sound_available();
        invalidate();
    }

    void add_track(const char* path) {
        Track& t = tracks_.emplace();
        t.path = path;
        String name = vfs::basename(path);
        long last = name.rfind('.');
        t.title = last > 0 ? String(name.c_str(), (size_t)last) : name;
        const uint8_t* d;
        size_t size;
        if (vfs::read_file(path, &d, &size)) t.seconds = wav_seconds(d, size);
    }

    void scan() {
        tracks_.clear();
        Track& s = tracks_.emplace();
        s.title = "Klein Morning";
        s.seconds = 42;
        vfs::Node* dir = vfs::lookup(music_dir().c_str());
        if (dir && dir->is_dir)
            for (vfs::Node* n : dir->children) {
                if (n->is_dir || n->name.empty() || n->name[0] == '.') continue;
                String ext = vfs::extension(n->name.c_str());
                if (ext == "wav" || ext == "wave") add_track(vfs::join(music_dir().c_str(), n->name.c_str()).c_str());
            }
        refresh_list();
    }

    void refresh_list() {
        list_->clear();
        for (const Track& t : tracks_) {
            ListView::Item& it = list_->add(t.title.c_str(), t.seconds ? clock_text(t.seconds).c_str() : "");
            it.icon = [](Canvas& c, Rect r) {   // a little artwork tile: readable on the selection too
                c.fill_gradient(r, hex(0xFF6A88), hex(0x8E3BF5), 5);
                draw_symbol(c, r.inset(r.w / 5), Symbol::Music, hex(0xFFFFFF), 1.4f);
            };
        }
        list_->selected = current_;
    }

    uint32_t total_frames() const { return loaded_ == current_ && current_ >= 0 ? buf_frames_ : 0; }
    uint32_t position() const {
        if (!playing_) return off_;
        uint32_t p = off_ + sound_position(voice_);
        return kmin(p, buf_frames_);
    }

    void stop_voice() {
        if (voice_) sound_kill(voice_);
        if (fading_) sound_kill(fading_);
        voice_ = fading_ = 0;
        playing_ = false;
    }

    bool load(int i) {
        if (loaded_ == i) return true;
        stop_voice();
        kfree(buf_);
        buf_ = nullptr;
        buf_frames_ = 0;
        loaded_ = -1;
        error_ = "";
        const Track& t = tracks_[i];
        bool ok;
        if (t.path.empty()) {
            ok = compose_sample(&buf_, &buf_frames_);
        } else {
            const uint8_t* d;
            size_t size;
            ok = vfs::read_file(t.path.c_str(), &d, &size) && sound_decode_wav(d, size, &buf_, &buf_frames_);
            if (!ok) error_ = "KleinOS can play WAV files; this one's format isn't supported.";
        }
        if (ok) loaded_ = i;
        return ok;
    }

    void select(int i, bool play) {
        if (i < 0 || i >= (int)tracks_.size()) return;
        if (i != current_) {
            stop_voice();
            current_ = i;
            off_ = 0;
            error_ = "";
            if (loaded_ != i) { kfree(buf_); buf_ = nullptr; buf_frames_ = 0; loaded_ = -1; }
        }
        list_->selected = i;
        list_->ensure_visible(i);
        if (play) start(off_ >= total_frames() ? 0 : off_);
        invalidate();
    }

    void start(uint32_t from) {
        if (current_ < 0 || !sound_available() || !load(current_)) { invalidate(); return; }
        if (from >= buf_frames_) from = 0;
        stop_voice();
        off_ = from;
        voice_ = sound_play_static(buf_ + (size_t)from * 2, buf_frames_ - from, 2, 100);
        playing_ = voice_ != 0;
        invalidate();
    }

    void pause() {
        if (!playing_) return;
        off_ = position();
        if (fading_) sound_kill(fading_);
        fading_ = voice_;
        sound_stop(voice_, 40);
        voice_ = 0;
        playing_ = false;
        invalidate();
    }

    void toggle() {
        if (current_ < 0) { select(0, true); return; }
        if (playing_) pause();
        else start(off_);
    }

    void seek(uint32_t frame, bool final_) {
        off_ = frame;
        if (playing_ && final_) start(frame);
        else if (playing_) { stop_voice(); playing_ = false; resume_after_seek_ = true; }
        if (final_ && resume_after_seek_) { resume_after_seek_ = false; start(frame); }
        invalidate();
    }

    void seek_to_x(int x, bool final_) {
        if (current_ < 0 || !load(current_)) return;
        Rect bar = seek_rect();
        float f = kclamp((float)(x - bar.x) / bar.w, 0.0f, 1.0f);
        seek((uint32_t)(f * buf_frames_), final_);
    }

    void next(bool automatic) {
        if (tracks_.empty()) return;
        int n = current_ + 1;
        if (n >= (int)tracks_.size()) {
            if (automatic) { off_ = 0; invalidate(); return; }   // the end of the library: stop
            n = 0;
        }
        select(n, true);
    }

    void previous() {
        if (position() > (uint32_t)kSoundRate * 3) { start(0); return; }   // like every player: first back to the start
        select(current_ > 0 ? current_ - 1 : (int)tracks_.size() - 1, true);
    }

    ListView* list_ = nullptr;
    Slider* volume_ = nullptr;
    Vector<Track> tracks_;
    int current_ = -1, loaded_ = -1;
    int16_t* buf_ = nullptr;
    uint32_t buf_frames_ = 0;
    uint32_t voice_ = 0, fading_ = 0;
    uint32_t off_ = 0;               // frame the current voice started at (or the paused position)
    bool playing_ = false, seeking_ = false, resume_after_seek_ = false;
    uint32_t last_key_ = 0;
    String error_;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0xFF6A88), hex(0x8E3BF5));
    draw_symbol(c, r.inset(r.w / 4), Symbol::Music, hex(0xFFFFFF), r.w / 18.0f);
}

Window* create() { return new MusicWindow(); }

void open_music(const char* path) {
    if (g_win) g_win->open_path(path);
}

struct OpenRegistration {
    OpenRegistration() { register_open_handler("music", open_music); }
} g_open_reg;

AppRegistrar reg({"music", "Music", icon, create, "Media", true, true, 46, "Play music and sounds"});

}  // namespace
