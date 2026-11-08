#pragma once
// Session management: what to show after boot (installer, setup assistant,
// login window, desktop) and who is logged in.
#include "sys/system.h"

void session_start(BootMode mode);
// Called by the login window / OOBE when a user is ready to enter the desktop.
void session_enter_desktop(const char* full_name, const char* login);
void session_logout();
const char* session_user();         // login name ("guest")
const char* session_full_name();
