#pragma once
// The resolution is requested from the firmware at boot (see boot.S). Virtual adapters
// and the N220's Intel chip can then move to their native size here.

// Returns true if the mode changed; framebuffer() then describes the new mode.
bool display_set_native_mode();
