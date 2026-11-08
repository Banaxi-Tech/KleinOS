#pragma once
#include <stdint.h>

[[noreturn]] void power_reboot();
[[noreturn]] void power_shutdown();

// PC speaker
void speaker_tone(uint32_t hz);   // 0 = off
void speaker_beep(uint32_t hz, uint32_t ms);   // non-blocking; stops after ms (needs speaker_tick)
void speaker_tick();                           // called from main loop
