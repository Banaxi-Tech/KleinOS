#pragma once
// Software updates: signed .kupd packages from GitHub Releases, installed into
// the inactive A/B boot slot. GRUB tries a new slot once; if that boot never
// confirms itself, the next boot returns to the old slot.
#include <stddef.h>
#include <stdint.h>

namespace update {

enum class State { Unsupported, Idle, Checking, UpToDate, Available, Downloading, Verifying, Installing, Ready, Failed };

void init();                     // after the desktop has started
State state();
bool check();                    // ask the server; false if busy or unsupported
void confirm_by_user();          // call before a restart/shutdown the user chose: keeps a new version
bool install();                  // download and install the available version
// Install a full .kupd already in memory (kmalloc); signature checks are the same. Takes the buffer on true.
bool install_package(uint8_t* data, size_t size);
const char* latest_version();    // "1.0.170" once known, else ""
const char* error();             // last failure, for display
int progress();                  // 0..100 for the current download or install
char active_slot();              // 'a', 'b', or 0 when not installed

// Recovery: GRUB starts it (boot arg recovery=1) after 3 failed starts in a row.
bool recovery_boot();
bool previous_version_available();          // the other slot holds a complete system
// Clear the failed-start count (and switch to the other slot); restart afterwards.
bool recovery_finish(bool previous_version);
// Shift+Restart: the next start opens Recovery. False when this install can't
// (a live USB, or a boot config from before Recovery).
bool request_recovery();
bool recovery_requested();                  // this Recovery start was asked for, not caused by failed starts
// Restart or Shut Down chosen in the UI: keeps a version on trial; Restart with
// Shift held goes to Recovery.
[[noreturn]] void user_restart();
[[noreturn]] void user_shutdown();

}  // namespace update
