// KleinOS Setup — the installer shown when booting the ISO ("Install KleinOS").
// Language -> Install now -> Product key -> Edition -> License -> Install type ->
// Disk -> Installing (writes system.img to the disk) -> Restart.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/wm.h"
#include "boot/multiboot2.h"
#include "drivers/ata.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "drivers/power.h"
#include "sys/productkey.h"
#include "sys/sysconfig.h"
#include "sys/system.h"
#include "arch/interrupts.h"
#include "lib/math.h"
#include "lib/log.h"

namespace {

enum Page { PLanguage, PInstallNow, PKey, PEdition, PLicense, PType, PDisk, PInstalling, PRestart };

const char* kLicense =
    "KLEINOS SOFTWARE LICENSE TERMS\n\n"
    "These license terms are an agreement between you and the KleinOS project. Please read them. "
    "They apply to the software named above, including any media on which you received it.\n\n"
    "1. INSTALLATION AND USE RIGHTS. You may install and use one copy of the software on your device.\n\n"
    "2. PRODUCT KEY. A product key identifies your edition. Installing without a key installs an "
    "unactivated copy, which works the same but shows an activation reminder in Settings.\n\n"
    "3. NO WARRANTY. The software is provided \"as is\", without warranty of any kind. Installing it "
    "erases the selected disk.\n\n"
    "4. PRIVACY. KleinOS does not collect personal data. Network features connect only to the sites you visit.\n\n"
    "5. THIRD-PARTY COMPONENTS. KleinOS includes GRUB (GPL), QuickJS (MIT), stb (public domain), "
    "Noto Sans and DejaVu fonts (OFL / free license).\n\n"
    "By accepting, you agree to these terms.";

struct Region { const char* name; int offset; };
const Region kRegions[] = {{"English (United States)", -300}, {"English (United Kingdom)", 0}, {"Deutsch (Deutschland)", 60},
                           {"Deutsch (Österreich)", 60}, {"Français (France)", 60}, {"Español (España)", 60},
                           {"Italiano (Italia)", 60}, {"Nederlands", 60}, {"日本語 (Japan)", 540}};

class SetupWindow : public Window {
public:
    SetupWindow() : Window("KleinOS Setup", 0, 0, WF_FULLSCREEN) {
        wm::set_shell_visible(false);
        sys_ = boot_module("system.img");
        dlg_ = Rect((width() - 720) / 2, (height() - 500) / 2, 720, 500);
        set_ticking(true);
        char a[16];
        if (boot_arg("autoinstall", a, sizeof(a))) {
            strlcpy(key_, productkey::generic_key(productkey::Edition::Pro), sizeof(key_));
            edition_ = productkey::Edition::Pro;
            drive_ = first_disk();
            if (drive_ >= 0) start_install();
        }
        build_page();
    }

    int first_disk() {
        for (int i = 0; i < ata_drive_count(); i++) {
            AtaDrive* d = ata_drive(i);
            if (d && d->present && !d->atapi && d->sectors * 512 >= (sys_ ? sys_->size : 0) + (1 << 20)) return i;
        }
        return -1;
    }

    // ---------------------------------------------------------------- pages
    void clear_widgets() {
        while (!widgets().empty()) remove(widgets().back());
    }

    Rect inner() const { return Rect(dlg_.x + 40, dlg_.y + 70, dlg_.w - 80, dlg_.h - 140); }

    Button* add_next(const char* label, Func<void()> fn, bool enabled = true) {
        auto* b = (Button*)add(new Button(Rect(dlg_.right() - 150, dlg_.bottom() - 56, 120, 34), label, Button::Primary));
        b->on_click = fn;
        b->enabled = enabled;
        return b;
    }
    void add_back(Page to) {
        auto* b = (Button*)add(new Button(Rect(dlg_.right() - 280, dlg_.bottom() - 56, 120, 34), "Back"));
        b->on_click = [this, to] { go(to); };
    }

    void go(Page p) { page_ = p; build_page(); invalidate(); }

    void build_page() {
        clear_widgets();
        Rect in = inner();
        switch (page_) {
        case PLanguage: {
            const char* labels[] = {"Language to install", "Time and currency format", "Keyboard or input method"};
            for (int i = 0; i < 3; i++)
                add(new Label(Rect(in.x + 60, in.y + 70 + i * 62, 240, 30), labels[i], 15));
            auto* lang = (Dropdown*)add(new Dropdown(Rect(in.x + 300, in.y + 70, 300, 32)));
            for (const Region& r : kRegions) lang->options.push(String(r.name).substr(0, String(r.name).find(' ') > 0 ? String(r.name).find(' ') : String(r.name).size()));
            lang->selected = lang_;
            lang->on_change = [this](int i) { lang_ = i; };
            auto* region = (Dropdown*)add(new Dropdown(Rect(in.x + 300, in.y + 132, 300, 32)));
            for (const Region& r : kRegions) region->options.push(String(r.name));
            region->selected = region_;
            region->on_change = [this](int i) { region_ = i; rtc_set_utc_offset_minutes(kRegions[i].offset); };
            auto* kb = (Dropdown*)add(new Dropdown(Rect(in.x + 300, in.y + 194, 300, 32), {"US", "United Kingdom", "German", "French"}));
            kb->selected = kbd_;
            kb->on_change = [this](int i) { kbd_ = i; input_set_layout((KeyboardLayout)i); };
            auto* l = (Label*)add(new Label(Rect(in.x + 60, in.y + 270, 540, 40), "Enter your language and other preferences and click \"Next\" to continue.", 13));
            l->color = theme().text_secondary;
            add_next("Next", [this] { go(PInstallNow); });
            break;
        }
        case PInstallNow: {
            auto* b = (Button*)add(new Button(Rect(dlg_.x + (dlg_.w - 200) / 2, in.y + 120, 200, 52), "Install now", Button::Primary));
            b->font_size = 17;
            b->on_click = [this] { go(PKey); };
            auto* r = (Button*)add(new Button(Rect(in.x, dlg_.bottom() - 60, 200, 30), "Repair your computer", Button::Borderless));
            r->on_click = [this] { status_ = "No KleinOS installation was found to repair."; invalidate(); };
            break;
        }
        case PKey: {
            auto* t = (Label*)add(new Label(Rect(in.x, in.y, in.w, 30), "Activate KleinOS", 22, Font::Bold));
            (void)t;
            auto* d = (Label*)add(new Label(Rect(in.x, in.y + 40, in.w, 60),
                "If this is the first time you're installing KleinOS on this PC, enter a valid KleinOS product key. "
                "Your product key looks like this: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX", 14));
            d->wrap = true;
            key_field_ = (TextField*)add(new TextField(Rect(in.x, in.y + 130, 420, 36), "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX"));
            key_field_->font_size = 17;
            key_field_->max_length = 29;
            key_field_->set_text(key_);
            key_field_->on_change = [this] {
                char out[32];
                productkey::format_partial(key_field_->text.c_str(), out);
                if (strcmp(out, key_field_->text.c_str()) != 0) key_field_->set_text(out);
                strlcpy(key_, out, sizeof(key_));
                key_error_ = false;
                invalidate();
            };
            key_field_->on_submit = [this] { submit_key(); };
            focus_widget(key_field_);
            auto* nok = (Button*)add(new Button(Rect(in.x - 6, dlg_.bottom() - 60, 240, 30), "I don't have a product key", Button::Borderless));
            nok->on_click = [this] { key_[0] = 0; activated_ = false; go(PEdition); };
            add_next("Next", [this] { submit_key(); });
            add_back(PInstallNow);
            break;
        }
        case PEdition: {
            add(new Label(Rect(in.x, in.y, in.w, 30), "Select the edition you want to install", 20, Font::Bold));
            auto* list = (ListView*)add(new ListView(Rect(in.x, in.y + 50, in.w, 200)));
            list->row_height = 40;
            const char* names[] = {"KleinOS Home", "KleinOS Pro", "KleinOS Enterprise", "KleinOS Education"};
            for (int i = 0; i < 4; i++) list->add(names[i], "x64");
            list->selected = (int)edition_ - 1;
            list->on_select = [this](int i) { edition_ = (productkey::Edition)(i + 1); };
            add_next("Next", [this] { go(PLicense); });
            add_back(PKey);
            break;
        }
        case PLicense: {
            add(new Label(Rect(in.x, in.y - 10, in.w, 30), "Applicable notices and license terms", 20, Font::Bold));
            auto* ta = (TextArea*)add(new TextArea(Rect(in.x, in.y + 30, in.w, 230)));
            ta->set_text(kLicense);
            ta->readonly = true;
            ta->font_size = 13;
            auto* cb = (Checkbox*)add(new Checkbox(Rect(in.x, in.y + 272, 400, 26), "I accept the license terms", accepted_));
            next_btn_ = add_next("Next", [this] { go(PType); }, accepted_);
            cb->on_change = [this](bool v) { accepted_ = v; next_btn_->enabled = v; invalidate(); };
            add_back(activated_ ? PKey : PEdition);
            break;
        }
        case PType: {
            add(new Label(Rect(in.x, in.y, in.w, 30), "Which type of installation do you want?", 20, Font::Bold));
            auto* up = (Button*)add(new Button(Rect(in.x, in.y + 60, in.w, 70), "Upgrade: Install KleinOS and keep files, settings and apps"));
            up->enabled = false;
            auto* cu = (Button*)add(new Button(Rect(in.x, in.y + 150, in.w, 70), "Custom: Install KleinOS only (advanced)"));
            cu->on_click = [this] { go(PDisk); };
            add_back(PLicense);
            break;
        }
        case PDisk: {
            add(new Label(Rect(in.x, in.y - 10, in.w, 30), "Where do you want to install KleinOS?", 20, Font::Bold));
            disk_list_ = (ListView*)add(new ListView(Rect(in.x, in.y + 30, in.w, 190)));
            disk_list_->row_height = 44;
            disk_ids_.clear();
            for (int i = 0; i < ata_drive_count(); i++) {
                AtaDrive* d = ata_drive(i);
                if (!d || !d->present || d->atapi) continue;
                char name[96], size[32];
                snprintf(name, sizeof(name), "Drive %d  —  %s", i, d->model[0] ? d->model : "Disk");
                double gb = d->sectors / 2097152.0;
                snprintf(size, sizeof(size), gb >= 1 ? "%.1f GB" : "%.0f MB", gb >= 1 ? gb : gb * 1024);
                ListView::Item& it = disk_list_->add(name, size);
                it.icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Display, theme().text_secondary, 1.5f); };
                disk_ids_.push(i);
            }
            if (!disk_ids_.empty()) { disk_list_->selected = 0; drive_ = disk_ids_[0]; }
            disk_list_->on_select = [this](int i) { drive_ = disk_ids_[i]; status_.clear(); invalidate(); };
            const char* actions[] = {"Refresh", "Delete", "Format", "New"};
            for (int i = 0; i < 4; i++) {
                auto* b = (Button*)add(new Button(Rect(in.x + i * 100, in.y + 232, 90, 30), actions[i], Button::Borderless));
                b->on_click = [this, i] { if (i == 0) build_page(); else status_ = "All data on the selected drive will be erased during installation."; invalidate(); };
            }
            next_btn_ = add_next("Next", [this] {
                AtaDrive* d = ata_drive(drive_);
                if (!sys_) { status_ = "Setup files are missing (system.img). Boot \"Install KleinOS\" from the ISO."; invalidate(); return; }
                if (!d || d->sectors * 512 < sys_->size + (1 << 20)) { status_ = "This drive is too small. KleinOS needs at least 64 MB."; invalidate(); return; }
                start_install();
            }, !disk_ids_.empty());
            if (disk_ids_.empty()) status_ = "We couldn't find any drives. Add a hard disk to this computer and click Refresh.";
            add_back(PType);
            break;
        }
        case PInstalling:
        case PRestart:
            if (page_ == PRestart) {
                auto* b = (Button*)add(new Button(Rect(dlg_.right() - 170, dlg_.bottom() - 56, 140, 34), "Restart now", Button::Primary));
                b->on_click = [] { power_reboot(); };
            }
            break;
        }
    }

    void submit_key() {
        productkey::KeyInfo info;
        if (!productkey::validate(key_, &info)) { key_error_ = true; invalidate(); return; }
        edition_ = info.edition;
        activated_ = true;
        go(PLicense);
    }

    // ---------------------------------------------------------------- installation
    void start_install() {
        page_ = PInstalling;
        stage_ = 0;
        copied_ = 0;
        stage_start_ = timer_ms();
        klog("setup: installing to ata%d (%zu bytes), edition %d, key '%s'", drive_, sys_ ? sys_->size : 0, (int)edition_, key_);
        build_page();
        invalidate();
    }

    static bool all_zero(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i += 8) if (*(const uint64_t*)(p + i)) return false;
        return true;
    }

    void step_install() {
        uint64_t now = timer_ms();
        if (stage_ == 0) {
            // Copying files: write system.img in 1 MiB chunks (skip empty chunks past the first 4 MiB)
            size_t total = sys_->size;
            size_t chunk = 128 << 10;
            uint64_t budget_end = timer_ms() + 8;   // keep the UI responsive: ~8 ms of disk I/O per frame
            while (copied_ < total && timer_ms() < budget_end) {
                size_t n = total - copied_ < chunk ? total - copied_ : chunk;
                const uint8_t* src = sys_->data + copied_;
                if (copied_ < (4u << 20) || !all_zero(src, n)) {
                    uint32_t sectors = (uint32_t)((n + 511) / 512);
                    if (!ata_write(drive_, copied_ / 512, sectors, src)) { fail("Error 0x80070570: Setup could not write to the selected drive."); return; }
                }
                copied_ += n;
            }
            if (copied_ >= total) next_stage();
        } else if (stage_ == 1) {
            // Getting files ready: write the configuration
            if (now - stage_start_ > 400) {
                SysConfig cfg;
                memset(&cfg, 0, sizeof(cfg));
                strlcpy(cfg.product_key, key_, sizeof(cfg.product_key));
                cfg.edition = (uint32_t)edition_;
                cfg.activated = activated_;
                cfg.install_time = rtc_unix_time();
                strlcpy(cfg.computer_name, "KleinOS-PC", sizeof(cfg.computer_name));
                strlcpy(cfg.language, lang_ == 2 || lang_ == 3 ? "de-DE" : lang_ == 4 ? "fr-FR" : "en-US", sizeof(cfg.language));
                cfg.keyboard_layout = kbd_;
                cfg.utc_offset_min = kRegions[region_].offset;
                cfg.accent = kAccentBlue;
                if (!sysconfig_init_on_drive(drive_, cfg)) { fail("Setup could not write the system configuration."); return; }
                next_stage();
            }
        } else if (stage_ < 5) {
            // Installing features / updates / finishing up
            if (now - stage_start_ > 900) next_stage();
        } else if (stage_ == 5) {
            ata_flush(drive_);
            page_ = PRestart;
            restart_at_ = now + 10000;
            build_page();
        }
        // repaint only when something visible changed (percentage, stage, spinner at ~15 fps)
        int pct = (int)(copied_ * 100 / kmax<size_t>(1, sys_->size));
        if (pct != last_pct_ || stage_ != last_stage_ || now - last_paint_ > 66) {
            last_pct_ = pct;
            last_stage_ = stage_;
            last_paint_ = now;
            invalidate();
        }
    }
    int last_pct_ = -1, last_stage_ = -1;
    uint64_t last_paint_ = 0;
    Canvas* bg_ = nullptr;

    void next_stage() { stage_++; stage_start_ = timer_ms(); }
    void fail(const char* msg) { status_ = msg; page_ = PDisk; build_page(); invalidate(); }

    void on_tick(uint64_t now) override {
        if (page_ == PInstalling) step_install();
        else if (page_ == PRestart) {
            if (now >= restart_at_) power_reboot();
            static uint64_t last;
            if (now / 1000 != last) { last = now / 1000; invalidate(); }
        }
    }

    // ---------------------------------------------------------------- painting
    void on_paint(Canvas& c) override {
        // background: deep blue gradient with soft light, rendered once and cached
        if (!bg_) {
            bg_ = new Canvas(width(), height());
            bg_->fill_gradient(Rect(0, 0, width(), height()), hex(0x0B3D91), hex(0x061A40));
            bg_->fill_circle(width() * 0.75f, height() * 0.2f, 420, rgba(80, 160, 255, 26));
            bg_->fill_circle(width() * 0.2f, height() * 0.9f, 360, rgba(120, 90, 255, 22));
        }
        c.blit(*bg_, 0, 0);
        // dialog
        c.draw_shadow(dlg_, 10, 24, rgba(0, 0, 0, 120), 0, 10);
        c.fill_round_rect(dlg_, 10, hex(0xFFFFFF));
        Rect tb(dlg_.x, dlg_.y, dlg_.w, 44);
        c.fill_gradient(tb, hex(0xF3F5F9), hex(0xE7EBF2), 0);
        c.fill_round_rect(Rect(tb.x, tb.y, tb.w, 12), 10, hex(0xF3F5F9));
        c.fill_rect(Rect(tb.x, tb.bottom() - 1, tb.w, 1), hex(0xD6DCE6));
        draw_mark(c, Rect(tb.x + 14, tb.y + 10, 24, 24));
        c.draw_text_in(Rect(tb.x + 48, tb.y, 300, tb.h), page_ == PInstalling || page_ == PRestart ? "Installing KleinOS" : "KleinOS Setup", hex(0x1C1C1E), 14, Font::Medium);
        Rect in = inner();
        Color tx = hex(0x1C1C1E), t2 = hex(0x5A6170);
        switch (page_) {
        case PLanguage:
            draw_mark(c, Rect(dlg_.x + (dlg_.w - 64) / 2, in.y - 10, 64, 64));
            c.draw_text_in(Rect(dlg_.x, in.y + 30, dlg_.w, 40), "", tx, 20);
            break;
        case PInstallNow:
            draw_mark(c, Rect(dlg_.x + (dlg_.w - 96) / 2, in.y - 20, 96, 96));
            c.draw_text_in(Rect(dlg_.x, in.y + 80, dlg_.w, 30), "KleinOS", tx, 26, Font::Bold, Align::Center);
            break;
        case PKey:
            if (key_error_) c.draw_text(in.x, in.y + 176, "This product key didn't work. Check it and try again, or try a different key.", hex(0xD12F2F), 13);
            else if (strlen(key_) == 29) {
                productkey::KeyInfo info;
                if (productkey::validate(key_, &info)) c.draw_text(in.x, in.y + 176, productkey::edition_name(info.edition), hex(0x1A7F37), 13, Font::Medium);
            }
            break;
        case PInstalling:
        case PRestart: {
            c.draw_text(in.x, in.y - 6, page_ == PRestart ? "KleinOS needs to restart to continue" : "Status", tx, 20, Font::Bold);
            const char* stages[] = {"Copying KleinOS files", "Getting files ready for installation", "Installing features",
                                    "Installing updates", "Finishing up"};
            for (int i = 0; i < 5; i++) {
                int y = in.y + 44 + i * 36;
                bool done = stage_ > i || page_ == PRestart;
                bool cur = stage_ == i && page_ == PInstalling;
                Color col = done ? hex(0x1A7F37) : cur ? tx : hex(0xA0A6B2);
                if (done) draw_symbol(c, Rect(in.x, y + 3, 18, 18), Symbol::Check, col, 2.2f);
                else if (cur) {
                    float a = (timer_ms() % 1000) / 1000.0f * 6.283f;
                    c.fill_arc(in.x + 9, y + 12, 9, 6, a, a + 4.2f, theme().accent);
                }
                char line[96];
                if (i == 0 && cur) snprintf(line, sizeof(line), "%s (%d%%)", stages[i], (int)(copied_ * 100 / kmax<size_t>(1, sys_->size)));
                else strlcpy(line, stages[i], sizeof(line));
                c.draw_text(in.x + 30, y, line, col, 15, cur ? Font::Medium : Font::Regular);
            }
            if (page_ == PInstalling) {
                float total = (stage_ + (stage_ == 0 ? (float)copied_ / kmax<size_t>(1, sys_->size) : 0.5f)) / 5.0f;
                Rect bar(in.x, in.bottom() - 10, in.w, 6);
                c.fill_round_rect(bar, 3, hex(0xE3E7EE));
                c.fill_round_rect(Rect(bar.x, bar.y, (int)(bar.w * kclamp(total, 0.02f, 1.0f)), bar.h), 3, theme().accent);
            } else {
                int secs = (int)((restart_at_ - kmin(restart_at_, timer_ms()) + 999) / 1000);
                char msg[96];
                snprintf(msg, sizeof(msg), "Restarting in %d seconds", secs);
                c.draw_text(in.x, in.bottom() - 30, msg, t2, 15);
            }
            break;
        }
        default: break;
        }
        if (!status_.empty()) c.draw_text_wrapped(Rect(in.x, dlg_.bottom() - 100, in.w - 170, 40), status_.c_str(), hex(0xB4232C), 13);
        // footer
        c.draw_text(dlg_.x + 24, dlg_.bottom() + 16, system_version_string(), rgba(255, 255, 255, 140), 12);
        paint_widgets(c);
    }

    static void draw_mark(Canvas& c, Rect r) {
        float rad = r.w * 0.24f;
        c.fill_gradient(r, hex(0x5E8BFF), hex(0x7A3CFF), rad);
        c.fill_gradient(Rect(r.x + 1, r.y + 1, r.w - 2, r.h / 2), rgba(255, 255, 255, 80), rgba(255, 255, 255, 0), rad - 1);
        float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, u = r.w / 96.0f;
        c.draw_line(cx - 15 * u, cy - 24 * u, cx - 15 * u, cy + 24 * u, hex(0xFFFFFF), 8 * u);
        c.draw_line(cx - 11 * u, cy + 3 * u, cx + 17 * u, cy - 24 * u, hex(0xFFFFFF), 8 * u);
        c.draw_line(cx - 4 * u, cy - 3 * u, cx + 18 * u, cy + 24 * u, hex(0xFFFFFF), 8 * u);
    }

    bool on_close() override { return false; }

    // Shift+F10 opens a command prompt (Terminal) on top of Setup, like Windows Setup.
    void on_key(const KeyEvent& e) override {
        if (e.key == Key::F10 && e.shift()) {
            Window* t = app_launch("terminal");
            if (t) wm::focus(t);
            return;
        }
        Window::on_key(e);
    }

    const BootModule* sys_;
    Rect dlg_;
    Page page_ = PLanguage;
    int lang_ = 0, region_ = 0, kbd_ = 0;
    char key_[32] = "";
    bool key_error_ = false, activated_ = false, accepted_ = false;
    productkey::Edition edition_ = productkey::Edition::Pro;
    TextField* key_field_ = nullptr;
    Button* next_btn_ = nullptr;
    ListView* disk_list_ = nullptr;
    Vector<int> disk_ids_;
    int drive_ = -1;
    int stage_ = 0;
    size_t copied_ = 0;
    uint64_t stage_start_ = 0, restart_at_ = 0;
    String status_;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x2F6FE4), hex(0x0B3D91));
    draw_symbol(c, r.inset(r.w / 4), Symbol::Download, hex(0xFFFFFF), r.w / 20.0f);
}

Window* create() { return new SetupWindow(); }

AppRegistrar reg({"setup", "KleinOS Setup", icon, create, "System", true, false, 100, "Install KleinOS on this computer"});

}  // namespace
