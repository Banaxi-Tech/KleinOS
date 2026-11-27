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
bool install();                  // download and install the available version
// Install a full .kupd already in memory (kmalloc); signature checks are the same. Takes the buffer on true.
bool install_package(uint8_t* data, size_t size);
const char* latest_version();    // "1.0.170" once known, else ""
const char* error();             // last failure, for display
int progress();                  // 0..100 for the current download or install
char active_slot();              // 'a', 'b', or 0 when not installed

}  // namespace update
