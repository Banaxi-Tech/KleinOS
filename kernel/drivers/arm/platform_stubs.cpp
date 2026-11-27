// Display mode switching needs a native GPU driver; on ARM the mode is fixed when the framebuffer is set up.
#include "drivers/display_mode.h"

bool display_set_native_mode() { return false; }
