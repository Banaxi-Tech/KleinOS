# Writing KleinOS apps

KleinOS is a freestanding C++20 kernel. Each app is a **.dl** dynamic library:
everything in `apps/<Name>/*.cpp` is compiled with `-fPIC` and linked into
`/System/Apps/<Name>.dl`. At boot, the kernel loads every .dl. Its static
constructors run then, and your `AppRegistrar` adds the app to the Dock,
Launchpad and Spotlight.

## Rules
- **No standard library.** No `std::vector`, `std::string`, `<cmath>`, `printf`, exceptions, RTTI or `dynamic_cast`.
  Use `Vector<T>` (lib/vector.h), `String` (lib/kstring.h), `Func<R(A...)>` (ui/func.h),
  `snprintf` / `strcmp` / ... (lib/string.h), `sqrtf` / `sinf` / ... (lib/math.h), and `kmin` / `kmax` / `kclamp`.
- Put helpers in an anonymous `namespace { }`.
- Only touch files inside your own `apps/<Name>/` directory.
- Single-threaded, event-driven. Never block or busy-wait. Do periodic work in `on_tick` (call `set_ticking(true)`).
- Time: `timer_ms()` (arch/interrupts.h). Wall clock: `rtc_now()` (drivers/rtc.h). Random: `cpu_random64()` (arch/cpu.h).
- Files: `vfs::read_text`, `vfs::write_text`, `vfs::mkdir`, `vfs::lookup` (fs/vfs.h). User home is
  `String("/Users/") + session_user()` (sys/session.h). Store app data in `<home>/Library/<App>/`.
- Beep: `speaker_beep(hz, ms)` (drivers/power.h). Notification: `wm::notify(title, body, "appid")`.

## Skeleton
```cpp
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"

namespace {

class MyWindow : public Window {
public:
    MyWindow() : Window("My App", 480, 320) {           // content size (titlebar is extra)
        auto* b = (Button*)add(new Button(Rect(20, 20, 120, 30), "Click me", Button::Primary));
        b->on_click = [this] { count++; invalidate(); };
    }
    void on_paint(Canvas& c) override {
        Window::on_paint(c);                              // background + widgets
        char buf[32]; snprintf(buf, sizeof(buf), "Clicked %d times", count);
        c.draw_text(20, 70, buf, theme().text, 17, Font::Medium);
    }
    void on_key(const KeyEvent& e) override {
        if (e.key == Key::Char && e.ch == 'r') { count = 0; invalidate(); return; }
        Window::on_key(e);                                // lets focused widgets handle keys
    }
    int count = 0;
};

void icon(Canvas& c, Rect r) {                            // r is a square, any size (32..90 px)
    icon_base(c, r, hex(0x34C759), hex(0x248A3D));        // gradient squircle
    draw_symbol(c, r.inset(r.w / 4), Symbol::Star, hex(0xFFFFFF), r.w / 20.0f);
}
Window* create() { return new MyWindow(); }

// id, name, icon, create, category, single_instance, in_dock, dock_order, description
AppRegistrar reg({"myapp", "My App", icon, create, "Utilities", true, false, 100, "Does things"});

}  // namespace
```

## Drawing (gfx/canvas.h)
Coordinates in `on_paint` are local to the content area: (0,0) is the top-left, and the size is `width()` x `height()`.
- `fill_rect`, `fill_round_rect(r, radius, color)`, `stroke_round_rect`, `fill_gradient(r, top, bottom, radius)`,
  `fill_circle`, `stroke_circle`, `fill_ellipse`, `fill_arc`, `draw_line(x0,y0,x1,y1,color,width)`,
  `fill_polygon(PointF*, n, color)`, `draw_shadow(r, radius, blur, color, dx, dy)`.
- Text: `draw_text(x, y_top, utf8, color, size, Font::Regular|Medium|Bold|Mono)` returns the width.
  Also `draw_text_in(rect, text, color, size, font, Align::Left|Center|Right)` (single line, ellipsized),
  `draw_text_wrapped(rect, ...)`, `text_width(s, size, font)` and `line_height(size, font)`.
- Images: `Canvas* img = image_decode(bytes, len)` (PNG/JPEG/GIF/BMP), then `c.draw_scaled(*img, dstRect)` or `c.draw(*img, x, y)`.
- State: `c.save(); c.clip_to(r); c.translate(dx, dy); ... c.restore();`
- Colors: `hex(0xRRGGBB)`, `rgb(r,g,b)`, `rgba(r,g,b,a)`, `with_alpha(c, a)`, `mix(a, b, t0_255)`. Always take UI colors
  from `theme()`: `text`, `text_secondary`, `window_bg`, `accent`, `separator`, `control_bg`, `field_bg`, `selection`, `dark`, etc.
- Symbols: `draw_symbol(c, rect, Symbol::Gear, color, stroke)`. See ui/app.h for the list.

## Window (ui/window.h)
Override `on_paint`, `on_mouse_down/up/move`, `on_double_click`, `on_scroll` (delta > 0 means down),
`on_key` (presses only; `e.key`, `e.ch`, `e.cmd()`, `e.shift()`), `on_tick(now_ms)`, `on_resize`, `on_close`,
and `build_menus(Vector<Menu>&)` for menubar menus.
Call `invalidate()` after state changes. Flags: `WF_RESIZABLE` (default), `WF_GLASS`
(transparent content over frosted glass), `WF_NO_ZOOM`, `WF_DIALOG`.
`set_sidebar_width(px)` gives you a frosted glass sidebar on the left. Paint it with transparent or translucent colors.

## Widgets (ui/widgets.h)
`Label`, `Button` (Normal/Primary/Destructive/Borderless/Glass), `TextField` (single line, `on_submit`, `password`),
`TextArea` (multi-line editor with `highlighter` and `line_numbers`), `Checkbox`, `Toggle`, `Slider`, `ProgressBar`,
`SegmentedControl`, `ListView` (`add(text, detail)`, `on_select`, `on_activate`, `icon`), `Dropdown`.
Create them with `new`, pass to `add(...)`, and lay them out in content coordinates. Move widgets in `on_resize` if the window is resizable.

## Opening files
`register_open_handler("myapp", [](const char* path) { ... })`. Register it in your window constructor or in a
static constructor. Other apps call `app_open_file(path)`.

## Build, check and screenshot (use YOUR OWN build dir)
```
make BUILD=build-<you> -j8 check F=apps/<Name>/main.cpp       # fast syntax check
make BUILD=build-<you> -j8 shot APP=<appid> SHOT_DELAY=5       # boots QEMU headless, opens your app
```
Then look at `build-<you>/shot.png` with the Read tool. `build-<you>/serial.log` has the kernel log.
If the build breaks in a file that isn't yours (someone else is mid-edit), wait 30 s and retry, or add
`SKIP=apps/TheirApp/main.cpp`.

## Drivers (.dri)

Hardware drivers live in `drivers/<Name>/*.cpp` and build to `/System/Drivers/<Name>.dri`, the same ELF format as a
.dl. They load at boot, before the network starts. Export one `klein_driver`; `probe` runs for each matching PCI device.
Drivers run in the kernel with full hardware access, and a crash in one takes the system down.

```cpp
#include "sys/driver.h"
#include "net/net.h"

static const DriverPciId ids[] = {{0x11AB, kPciAnyDevice}};
static bool probe(const PciDevice* d) {
    pci_enable(d);
    // set up the card; for a network card fill a static net::NicDriver and call net::register_nic(&nic)
    return true;
}
extern "C" { extern const DriverInfo klein_driver; const DriverInfo klein_driver = {KLEIN_DRIVER_ABI, "My Card", ids, 1, probe}; }
```
