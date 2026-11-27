// Software mixer for drivers/IntelHDA (see sound.h). The voice table is shared with the
// timer interrupt: the interrupt only reads voices and marks them done, everything
// else changes them with interrupts off and frees buffers from the main loop.
#include "drivers/sound.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "fs/fat32.h"
#include "drivers/rtc.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/math.h"
#include "lib/string.h"
#include "sys/system.h"

namespace {

enum class VState : uint8_t { Free, Playing, Done };

struct Voice {
    volatile VState state;
    uint32_t id;
    const int16_t* data;
    uint32_t frames, pos;
    uint8_t channels;      // 1 or 2
    bool owned;            // free data when done
    int32_t gain;          // Q16 (65536 = full)
    int32_t fade;          // subtracted from gain every frame once fading
};

constexpr int kVoices = 24;
Voice g_voices[kVoices];
uint32_t g_next_id = 1;

const char* g_name = "";
void (*g_pump)();
volatile int32_t g_master;   // Q8, from volume and mute
int g_volume = 70;
bool g_muted;
uint64_t g_save_at;           // remember the volume a moment after it last changed

constexpr char kVolumeFile[] = "/KleinOS/sound.txt";

void update_master() {
    // a squared curve sounds even across the slider
    g_master = g_muted ? 0 : (g_volume * g_volume * 256) / 10000;
}

#if defined(__x86_64__) || defined(__i386__)
uint32_t g_tick;
alignas(16) uint8_t g_fpu[512];
void timer_hook() {
    if (!g_pump || (++g_tick & 3)) return;
    // the interrupted code may be in the middle of SSE/x87 work
    asm volatile("fxsave %0" : "=m"(g_fpu));
    g_pump();
    asm volatile("fxrstor %0" : : "m"(g_fpu));
}
#endif

void load_volume();

void poll() {
    load_volume();
    for (Voice& v : g_voices) {
        if (v.state != VState::Done) continue;
        if (v.owned) kfree((void*)v.data);
        v.data = nullptr;
        v.state = VState::Free;
    }
    if (g_save_at && timer_ms() >= g_save_at && fat32::mounted()) {
        g_save_at = 0;
        char text[32];
        int n = snprintf(text, sizeof(text), "volume=%d\nmuted=%d\n", g_volume, g_muted ? 1 : 0);
        if (fat32::write_file(kVolumeFile, text, (size_t)n, rtc_unix_time())) fat32::flush();
    }
}

// The system partition is mounted after drivers load: read the saved volume once it is
// (a change made before then wins).
bool g_volume_loaded;
void load_volume() {
    if (g_volume_loaded || !fat32::mounted()) return;
    g_volume_loaded = true;
    uint8_t* d = nullptr;
    size_t n = 0;
    if (!fat32::read_file(kVolumeFile, &d, &n)) return;
    char text[64];
    n = n < sizeof(text) - 1 ? n : sizeof(text) - 1;
    memcpy(text, d, n);
    text[n] = 0;
    kfree(d);
    const char* v = strstr(text, "volume=");
    if (v) g_volume = kclamp(atoi(v + 7), 0, 100);
    const char* m = strstr(text, "muted=");
    if (m) g_muted = atoi(m + 6) != 0;
    update_master();
}

uint32_t g_rng = 0x9E3779B9u;
float noise() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float)(int32_t)g_rng / 2147483648.0f;
}

constexpr float kTwoPi = 6.28318530718f;

int16_t* alloc_frames(uint32_t frames) {
    int16_t* p = (int16_t*)kmalloc((size_t)frames * 4);
    if (p) memset(p, 0, (size_t)frames * 4);
    return p;
}

int16_t to_s16(float v) {
    v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    return (int16_t)(v * 32767.0f);
}

// Adds a mono signal into a stereo buffer (both channels).
void add_at(int16_t* buf, uint32_t frames, uint32_t at, uint32_t i, float v) {
    if (at + i >= frames) return;
    int16_t* s = buf + (at + i) * 2;
    int32_t x = s[0] + to_s16(v);
    x = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
    s[0] = s[1] = (int16_t)x;
}

// One note of a timbre, mixed into buf starting at frame `at`.
void render_note(int16_t* buf, uint32_t frames, uint32_t at, float hz, uint32_t len, float amp, Timbre t) {
    uint32_t attack = kSoundRate / 400;   // 2.5 ms: no click at the start
    uint32_t release = kSoundRate / 100;  // 10 ms: none at the end
    if (len < attack + release) len = attack + release;
    float w = kTwoPi * hz / kSoundRate;
    for (uint32_t i = 0; i < len && at + i < frames; i++) {
        float tsec = (float)i / kSoundRate;
        float env = i < attack ? (float)i / attack : 1.0f;
        if (i + release > len) env *= (float)(len - i) / release;
        float ph = w * (float)i, v = 0;
        switch (t) {
            case Timbre::Sine: v = sinf(ph); break;
            case Timbre::Square:   // odd harmonics only: softer than a real square wave
                v = (sinf(ph) + sinf(3 * ph) / 3 + sinf(5 * ph) / 5 + sinf(7 * ph) / 7) * 0.8f;
                break;
            case Timbre::Bell:
                v = (sinf(ph) + 0.5f * sinf(2.76f * ph) * expf(-tsec * 6) + 0.25f * sinf(5.4f * ph) * expf(-tsec * 10)) * 0.6f;
                env *= expf(-tsec * 4.0f);
                break;
            case Timbre::Piano:   // sound_tone renders pianos with sound_render_piano
            case Timbre::Pluck:
                v = (sinf(ph) + 0.4f * sinf(2 * ph) + 0.2f * sinf(3 * ph)) * 0.6f;
                env *= expf(-tsec * 9.0f);
                break;
        }
        add_at(buf, frames, at, i, v * env * amp);
    }
}

void render_noise(int16_t* buf, uint32_t frames, uint32_t at, uint32_t len, float amp, float decay, float smooth) {
    float lp = 0;
    for (uint32_t i = 0; i < len; i++) {
        float tsec = (float)i / kSoundRate;
        lp += (noise() - lp) * smooth;   // one-pole low-pass: smaller = darker
        add_at(buf, frames, at, i, lp * amp * expf(-tsec * decay));
    }
}

uint32_t ms_frames(int ms) { return (uint32_t)((uint64_t)ms * kSoundRate / 1000); }

}  // namespace

void sound_attach(const char* name, void (*pump)()) {
    g_name = name ? name : "Sound";
    load_volume();
    update_master();
    static bool hooked;
    if (!hooked) {
        hooked = true;
        system_add_poll_hook(poll);
#if defined(__x86_64__) || defined(__i386__)
        timer_set_irq_hook(timer_hook);
#endif
    }
    g_pump = pump;
    klog("sound: %s, volume %d%s", g_name, g_volume, g_muted ? " (muted)" : "");
}

void sound_mix(int16_t* out, uint32_t frames) {
    static int32_t acc[2 * 4096];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, frames * 2 * sizeof(int32_t));
    for (Voice& v : g_voices) {
        if (v.state != VState::Playing) continue;
        uint32_t n = frames;
        for (uint32_t i = 0; i < n; i++) {
            if (v.pos >= v.frames || v.gain <= 0) { v.state = VState::Done; break; }
            int32_t g = v.gain >> 8;   // Q8
            const int16_t* s = v.data + v.pos * v.channels;
            acc[i * 2] += (s[0] * g) >> 8;
            acc[i * 2 + 1] += (s[v.channels - 1] * g) >> 8;
            v.pos++;
            if (v.fade) v.gain -= v.fade;
        }
        if (v.pos >= v.frames) v.state = VState::Done;
    }
    int32_t m = g_master;
    for (uint32_t i = 0; i < frames * 2; i++) {
        int32_t x = (acc[i] * m) >> 8;
        out[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
}

bool sound_available() { return g_pump != nullptr; }
const char* sound_device_name() { return g_name; }

namespace {
uint32_t start_voice(const int16_t* data, uint32_t frames, int channels, int volume, bool owned) {
    if (!data) return 0;
    if (!g_pump || !frames) { if (owned) kfree((void*)data); return 0; }
    IrqGuard g;
    Voice* best = nullptr;
    for (Voice& v : g_voices) if (v.state == VState::Free) { best = &v; break; }
    if (!best) {   // all busy: replace the one closest to its end
        for (Voice& v : g_voices)
            if (v.state == VState::Playing && (!best || v.frames - v.pos < best->frames - best->pos)) best = &v;
        if (!best) { if (owned) kfree((void*)data); return 0; }
        if (best->owned) kfree((void*)best->data);
    }
    best->id = g_next_id++;
    best->data = data;
    best->channels = (uint8_t)(channels == 1 ? 1 : 2);
    best->owned = owned;
    best->frames = frames;
    best->pos = 0;
    best->gain = kclamp(volume, 0, 100) * 65536 / 100;
    best->fade = 0;
    best->state = VState::Playing;
    return best->id;
}
}  // namespace

uint32_t sound_play(int16_t* stereo, uint32_t frames, int volume) { return start_voice(stereo, frames, 2, volume, true); }

uint32_t sound_play_static(const int16_t* data, uint32_t frames, int channels, int volume) {
    return start_voice(data, frames, channels, volume, false);
}

int16_t* sound_render_piano(float hz, uint32_t frames, float amp) {
    int16_t* out = (int16_t*)kmalloc((size_t)frames * 2);
    if (!out) return nullptr;
    // each harmonic is a rotating phasor with its own decay: no sin/exp per sample, so a
    // note renders in a few ms even on an Atom
    constexpr int kH = 8;
    float c[kH], sn[kH], rc[kH], rs[kH], a[kH], d[kH];
    int n = 0;
    for (int k = 1; k <= kH; k++) {
        float f = hz * (float)k * (1.0f + 0.0004f * k * k);
        if (f > 16000) break;
        float w = kTwoPi * f / kSoundRate;
        c[n] = 1; sn[n] = 0; rc[n] = cosf(w); rs[n] = sinf(w);
        a[n] = amp * 0.55f / (k > 1 ? k * 1.1f : 1.0f);
        d[n] = expf(-(1.2f + 0.9f * k) / kSoundRate);
        n++;
    }
    float hammer = 0.25f, hd = expf(-40.0f / kSoundRate);
    uint32_t attack = kSoundRate / 400, release = kSoundRate / 50;
    for (uint32_t i = 0; i < frames; i++) {
        float v = 0;
        for (int k = 0; k < n; k++) {
            v += sn[k] * a[k];
            float nc = c[k] * rc[k] - sn[k] * rs[k];
            sn[k] = sn[k] * rc[k] + c[k] * rs[k];
            c[k] = nc;
            a[k] *= d[k];
        }
        if ((i & 1023) == 0)   // keep the phasors on the unit circle
            for (int k = 0; k < n; k++) { float m = 1.0f / sqrtf(c[k] * c[k] + sn[k] * sn[k]); c[k] *= m; sn[k] *= m; }
        float env = 1.0f - hammer;
        hammer *= hd;
        if (i < attack) env *= (float)i / attack;
        if (i + release > frames) env *= (float)(frames - i) / release;
        out[i] = to_s16(v * env);
    }
    return out;
}

void sound_stop(uint32_t id, int fade_ms) {
    IrqGuard g;
    for (Voice& v : g_voices) {
        if (v.state != VState::Playing || v.id != id) continue;
        uint32_t n = ms_frames(fade_ms > 0 ? fade_ms : 1);
        v.fade = v.gain / (int32_t)(n ? n : 1) + 1;
    }
}

bool sound_playing(uint32_t id) {
    for (Voice& v : g_voices) if (v.id == id && v.state == VState::Playing) return true;
    return false;
}

void sound_stop_all() {
    IrqGuard g;
    for (Voice& v : g_voices) if (v.state == VState::Playing) v.fade = v.gain / (int32_t)ms_frames(30) + 1;
}

uint32_t sound_tone(float hz, int ms, int volume, Timbre t) {
    if (!g_pump || hz <= 0 || ms <= 0) return 0;
    uint32_t len = ms_frames(ms);
    if (t == Timbre::Piano) {
        len += ms_frames(1200);   // let it ring
        int16_t* mono = sound_render_piano(hz, len, 0.8f);
        if (!mono) return 0;
        int16_t* st = alloc_frames(len);
        if (!st) { kfree(mono); return 0; }
        for (uint32_t i = 0; i < len; i++) st[i * 2] = st[i * 2 + 1] = mono[i];
        kfree(mono);
        return sound_play(st, len, volume);
    }
    uint32_t tail = (t == Timbre::Sine || t == Timbre::Square) ? 0 : len;   // decaying timbres ring on
    int16_t* buf = alloc_frames(len + tail);
    if (!buf) return 0;
    render_note(buf, len + tail, 0, hz, len + tail, 0.8f, t);
    return sound_play(buf, len + tail, volume);
}

bool sound_decode_wav(const uint8_t* d, size_t size, int16_t** out, uint32_t* out_frames) {
    auto u16 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (size < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    uint32_t fmt = 0, channels = 0, rate = 0, bits = 0;
    const uint8_t* pcm = nullptr;
    size_t pcm_size = 0;
    for (size_t o = 12; o + 8 <= size;) {
        uint32_t len = u32(o + 4);
        if (!memcmp(d + o, "fmt ", 4) && len >= 16 && o + 24 <= size) {
            fmt = u16(o + 8); channels = u16(o + 10); rate = u32(o + 12); bits = u16(o + 22);
            if (fmt == 0xFFFE && len >= 40 && o + 34 <= size) fmt = u16(o + 32);   // WAVE_FORMAT_EXTENSIBLE
        } else if (!memcmp(d + o, "data", 4)) {
            pcm = d + o + 8;
            pcm_size = len < size - o - 8 ? len : size - o - 8;
        }
        o += 8 + len + (len & 1);
    }
    if (!pcm || !channels || channels > 8 || rate < 4000 || rate > 192000) return false;
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) return false;
    uint32_t bps = bits / 8, in_frames = (uint32_t)(pcm_size / (bps * channels));
    if (!in_frames) return false;
    auto sample = [&](uint32_t f, uint32_t c) -> int32_t {
        const uint8_t* p = pcm + ((size_t)f * channels + c) * bps;
        switch (bits) {
            case 8: return ((int32_t)p[0] - 128) << 8;
            case 16: return (int16_t)(p[0] | p[1] << 8);
            case 24: return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
            default:
                if (fmt == 3) {
                    float v;
                    memcpy(&v, p, 4);
                    return to_s16(v);
                }
                return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) >> 16;
        }
    };
    uint64_t frames64 = (uint64_t)in_frames * kSoundRate / rate;
    if (frames64 > (uint64_t)kSoundRate * 60 * 20) frames64 = (uint64_t)kSoundRate * 60 * 20;   // 20 minutes
    uint32_t frames = (uint32_t)frames64;
    int16_t* buf = (int16_t*)kmalloc((size_t)frames * 4);
    if (!buf) return false;
    uint32_t right = channels > 1 ? 1 : 0;
    for (uint32_t i = 0; i < frames; i++) {
        // linear interpolation between input frames (16.16 position)
        uint64_t p = (uint64_t)i * rate * 65536 / kSoundRate;
        uint32_t a = (uint32_t)(p >> 16), frac = (uint32_t)(p & 0xFFFF);
        uint32_t b = a + 1 < in_frames ? a + 1 : a;
        int32_t l = sample(a, 0) + (int32_t)(((int64_t)(sample(b, 0) - sample(a, 0)) * frac) >> 16);
        int32_t r = sample(a, right) + (int32_t)(((int64_t)(sample(b, right) - sample(a, right)) * frac) >> 16);
        buf[i * 2] = (int16_t)kclamp(l, -32768, 32767);
        buf[i * 2 + 1] = (int16_t)kclamp(r, -32768, 32767);
    }
    *out = buf;
    *out_frames = frames;
    return true;
}

uint32_t sound_play_wav(const uint8_t* data, size_t size, int volume) {
    int16_t* buf;
    uint32_t frames;
    if (!g_pump || !sound_decode_wav(data, size, &buf, &frames)) return 0;
    return sound_play(buf, frames, volume);
}

void sound_effect(SoundEffect e) {
    load_volume();
    if (!g_pump || g_muted) return;
    // an action that makes its own sound and then posts a notification (screenshot,
    // trash, logging in) shouldn't also ding
    static uint64_t last_other;
    uint64_t now = timer_ms();
    if (e == SoundEffect::Notification && last_other && now - last_other < 1000) return;
    if (e != SoundEffect::Notification) last_other = now;
    // rendered once, then played from the cache
    static int16_t* cache[8];
    static uint32_t cache_len[8];
    int slot = (int)e;
    if (slot < 8 && cache[slot]) { sound_play_static(cache[slot], cache_len[slot], 2, 100); return; }
    uint32_t len = 0;
    int16_t* b = nullptr;
    switch (e) {
        case SoundEffect::Notification:   // two soft bell notes going up
            len = ms_frames(900);
            if (!(b = alloc_frames(len))) return;
            render_note(b, len, 0, 1318.5f, ms_frames(700), 0.30f, Timbre::Bell);
            render_note(b, len, ms_frames(110), 1975.5f, ms_frames(780), 0.26f, Timbre::Bell);
            break;
        case SoundEffect::Alert:          // a low, short "bonk"
            len = ms_frames(260);
            if (!(b = alloc_frames(len))) return;
            render_note(b, len, 0, 392.0f, len, 0.45f, Timbre::Pluck);
            render_note(b, len, 0, 196.0f, len, 0.30f, Timbre::Pluck);
            break;
        case SoundEffect::Screenshot:     // a camera shutter: two quick clicks of noise
            len = ms_frames(220);
            if (!(b = alloc_frames(len))) return;
            render_noise(b, len, 0, ms_frames(60), 0.55f, 70, 0.6f);
            render_noise(b, len, ms_frames(90), ms_frames(110), 0.45f, 45, 0.35f);
            break;
        case SoundEffect::Trash:          // paper crumpling into the bin
            len = ms_frames(360);
            if (!(b = alloc_frames(len))) return;
            for (int k = 0; k < 6; k++) render_noise(b, len, ms_frames(k * 45), ms_frames(70), 0.35f - k * 0.03f, 40, 0.25f);
            break;
        case SoundEffect::Startup: {      // a warm chord, played upwards
            len = ms_frames(2600);
            if (!(b = alloc_frames(len))) return;
            static const float notes[] = {261.63f, 329.63f, 392.0f, 523.25f, 659.26f};
            for (int k = 0; k < 5; k++) {
                uint32_t at = ms_frames(k * 70);
                int16_t* note = sound_render_piano(notes[k], len - at, 0.17f);
                if (!note) continue;
                for (uint32_t i = 0; i < len - at; i++) {
                    int32_t x = b[(at + i) * 2] + note[i];
                    b[(at + i) * 2] = b[(at + i) * 2 + 1] = (int16_t)kclamp(x, -32768, 32767);
                }
                kfree(note);
            }
            break;
        }
        case SoundEffect::Volume:         // the short pop when changing the volume
        case SoundEffect::Pop:
            len = ms_frames(90);
            if (!(b = alloc_frames(len))) return;
            render_note(b, len, 0, e == SoundEffect::Volume ? 880.0f : 660.0f, len, 0.35f, Timbre::Pluck);
            break;
    }
    if (slot < 8) { cache[slot] = b; cache_len[slot] = len; sound_play_static(b, len, 2, 100); }
    else sound_play(b, len, 100);
}

int sound_volume() { load_volume(); return g_volume; }
bool sound_muted() { load_volume(); return g_muted; }

void sound_set_volume(int v, bool feedback) {
    g_volume_loaded = true;
    v = kclamp(v, 0, 100);
    bool changed = v != g_volume;
    g_volume = v;
    if (v > 0) g_muted = false;
    update_master();
    g_save_at = timer_ms() + 1500;
    if (feedback && changed) sound_effect(SoundEffect::Volume);
}

void sound_set_muted(bool m) {
    g_volume_loaded = true;
    g_muted = m;
    update_master();
    g_save_at = timer_ms() + 1500;
}
