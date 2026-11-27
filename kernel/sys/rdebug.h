#pragma once
// Wi-Fi Debugging: a password-protected debug server for machines on the same
// local network. Off until the owner turns it on in Settings. Someone with the
// password has full control of the kernel, so connections are encrypted, only
// local addresses are accepted and failed logins lock the server out for a while.
// The client is tools/kdbg.py.
#include <stdint.h>

namespace rdebug {

constexpr uint16_t kPort = 5555;
constexpr int kMinPassword = 10;

void init();                         // after the network has started
bool has_password();
bool set_password(const char* password);   // false if too short
bool enabled();
bool set_enabled(bool on);           // false without a password
// Address of the connected debugger, or nullptr.
const char* peer();

}  // namespace rdebug
