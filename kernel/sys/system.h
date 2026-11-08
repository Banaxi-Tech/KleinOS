#pragma once
// System-level state: version, boot mode, main loop.
#include <stdint.h>

#define KLEIN_VERSION "1.0"   // main.sub; the full version appends the build: 1.0.144
#define KLEIN_CODENAME "Aurora"

enum class BootMode { Live, Installer, Installed };

BootMode system_boot_mode();
const char* system_boot_mode_name();
int system_build_number();
bool system_is_dev_build();
// "KleinOS 1.0.123 Aurora"
const char* system_version_string();

// Hooks run every main-loop iteration (drivers, network stack...).
using PollHook = void (*)();
void system_add_poll_hook(PollHook hook);

[[noreturn]] void system_main();
// Create /Users/<login> with the standard folders and a welcome note.
void system_create_home(const char* login);
