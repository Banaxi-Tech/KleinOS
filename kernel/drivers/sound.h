#pragma once
// Sound: a small software mixer that feeds one output device (drivers/IntelHDA).
// Everything plays as 48 kHz 16-bit stereo. The device pulls mixed audio from the
// timer interrupt, so sound keeps playing while the UI is busy.
#include <stddef.h>
#include <stdint.h>

constexpr int kSoundRate = 48000;

// ---- for output drivers
// pump is called about every 4 ms from the timer interrupt (FPU state saved); it calls
// sound_mix to fill the device's ring buffer ahead of the hardware.
void sound_attach(const char* name, void (*pump)());
void sound_mix(int16_t* out, uint32_t frames);           // interrupt context only

// ---- for everyone else (not from interrupts)
bool sound_available();
const char* sound_device_name();                         // "" without a device

// Play stereo frames; the buffer must come from kmalloc and is freed when done.
// volume 0..100 (before the system volume). Returns a voice id, 0 if nothing plays.
uint32_t sound_play(int16_t* stereo, uint32_t frames, int volume = 100);
// Play frames the caller keeps alive for good (a cache): mono or stereo.
uint32_t sound_play_static(const int16_t* data, uint32_t frames, int channels, int volume = 100);
// A piano note as mono frames (kmalloc'd), for caching.
int16_t* sound_render_piano(float hz, uint32_t frames, float amp = 0.8f);
void sound_stop(uint32_t id, int fade_ms = 30);
bool sound_playing(uint32_t id);
uint32_t sound_position(uint32_t id);                    // frames played so far (0 once it ended)
// Stop at once, without a fade: afterwards the mixer no longer reads the voice's buffer.
void sound_kill(uint32_t id);
void sound_stop_all();

enum class Timbre : uint8_t { Sine, Square, Bell, Piano, Pluck };
uint32_t sound_tone(float hz, int ms, int volume = 60, Timbre t = Timbre::Sine);

// WAV files: PCM 8/16/24-bit or float, mono or stereo, any rate (resampled).
uint32_t sound_play_wav(const uint8_t* data, size_t size, int volume = 100);
// Decodes a WAV into 48 kHz stereo (kmalloc'd); false if the format is unsupported.
bool sound_decode_wav(const uint8_t* data, size_t size, int16_t** out, uint32_t* frames);

enum class SoundEffect : uint8_t { Notification, Alert, Screenshot, Trash, Startup, Volume, Pop };
void sound_effect(SoundEffect e);

// System volume 0..100 and mute; remembered across starts.
int sound_volume();
void sound_set_volume(int v, bool feedback = false);
bool sound_muted();
void sound_set_muted(bool m);
