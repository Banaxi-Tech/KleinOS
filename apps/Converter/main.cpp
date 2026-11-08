// Converter: unit conversion for length, mass, temperature, volume, speed and data.
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "lib/math.h"
#include "lib/string.h"

namespace {

constexpr int kWinW = 700, kWinH = 500;

// Every unit converts to a category base unit as: base = value * factor + offset.
struct Unit {
    const char* name;
    const char* sym;
    double factor;
    double offset;
};

enum Category : int { Length, Mass, Temperature, Volume, Speed, Data, CategoryCount };

const char* kCategoryNames[CategoryCount] = {"Length", "Mass", "Temperature", "Volume", "Speed", "Data"};

const Unit kLength[] = {
    {"Meters", "m", 1.0, 0}, {"Kilometers", "km", 1000.0, 0}, {"Centimeters", "cm", 0.01, 0},
    {"Millimeters", "mm", 0.001, 0}, {"Inches", "in", 0.0254, 0}, {"Feet", "ft", 0.3048, 0},
    {"Yards", "yd", 0.9144, 0}, {"Miles", "mi", 1609.344, 0},
};
const Unit kMass[] = {
    {"Kilograms", "kg", 1.0, 0}, {"Grams", "g", 0.001, 0}, {"Milligrams", "mg", 0.000001, 0},
    {"Pounds", "lb", 0.45359237, 0}, {"Ounces", "oz", 0.028349523125, 0}, {"Stones", "st", 6.35029318, 0},
    {"Metric tons", "t", 1000.0, 0},
};
// Base unit: kelvin.
const Unit kTemperature[] = {
    {"Celsius", "°C", 1.0, 273.15}, {"Fahrenheit", "°F", 5.0 / 9.0, 255.3722222222}, {"Kelvin", "K", 1.0, 0},
    {"Rankine", "°R", 5.0 / 9.0, 0},
};
const Unit kVolume[] = {
    {"Liters", "L", 1.0, 0}, {"Milliliters", "mL", 0.001, 0}, {"Cubic meters", "m³", 1000.0, 0},
    {"US gallons", "gal", 3.785411784, 0}, {"US quarts", "qt", 0.946352946, 0}, {"US cups", "cup", 0.2365882365, 0},
    {"US fl oz", "fl oz", 0.0295735296, 0}, {"US tablespoons", "tbsp", 0.01478676478, 0},
};
const Unit kSpeed[] = {
    {"Meters/second", "m/s", 1.0, 0}, {"Kilometers/hour", "km/h", 1.0 / 3.6, 0}, {"Miles/hour", "mph", 0.44704, 0},
    {"Knots", "kn", 0.514444, 0}, {"Feet/second", "ft/s", 0.3048, 0},
};
const Unit kData[] = {
    {"Bytes", "B", 1.0, 0}, {"Bits", "bit", 0.125, 0}, {"Kilobytes", "KB", 1000.0, 0},
    {"Megabytes", "MB", 1e6, 0}, {"Gigabytes", "GB", 1e9, 0}, {"Terabytes", "TB", 1e12, 0},
    {"Kibibytes", "KiB", 1024.0, 0}, {"Mebibytes", "MiB", 1048576.0, 0}, {"Gibibytes", "GiB", 1073741824.0, 0},
};

struct Table {
    const Unit* units;
    int count;
};

Table table_for(int cat) {
    switch (cat) {
    case Length: return {kLength, (int)(sizeof(kLength) / sizeof(kLength[0]))};
    case Mass: return {kMass, (int)(sizeof(kMass) / sizeof(kMass[0]))};
    case Temperature: return {kTemperature, (int)(sizeof(kTemperature) / sizeof(kTemperature[0]))};
    case Volume: return {kVolume, (int)(sizeof(kVolume) / sizeof(kVolume[0]))};
    case Speed: return {kSpeed, (int)(sizeof(kSpeed) / sizeof(kSpeed[0]))};
    default: return {kData, (int)(sizeof(kData) / sizeof(kData[0]))};
    }
}

double convert(const Unit& from, const Unit& to, double v) {
    double base = v * from.factor + from.offset;
    return (base - to.offset) / to.factor;
}

// Formats with about seven significant digits and no trailing zeros.
void format_number(double v, char* out, size_t n) {
    if (v != v) { snprintf(out, n, "NaN"); return; }
    double a = v < 0 ? -v : v;
    if (a == 0) { snprintf(out, n, "0"); return; }
    if (a >= 1e15) { snprintf(out, n, "%.0f", v); return; }
    int dec;
    if (a >= 1) {
        int digits = 0;
        for (double t = a; t >= 1; t /= 10) digits++;
        dec = 7 - digits;
    } else {
        int zeros = 0;
        for (double t = a; t < 0.1 && zeros < 12; t *= 10) zeros++;
        dec = 7 + zeros;
    }
    if (dec < 0) dec = 0;
    if (dec > 12) dec = 12;
    char raw[64];
    snprintf(raw, sizeof(raw), "%.12f", v);
    // keep `dec` decimals, then strip trailing zeros
    char* dot = strchr(raw, '.');
    if (dot) {
        int keep = (int)(dot - raw) + 1 + dec;
        raw[keep] = 0;
        int len = (int)strlen(raw);
        while (len > 0 && raw[len - 1] == '0') raw[--len] = 0;
        if (len > 0 && raw[len - 1] == '.') raw[len - 1] = 0;
    }
    snprintf(out, n, "%s", raw);
}

// Parses a field; false if it does not contain a number.
bool parse_number(const char* s, double& out) {
    while (*s == ' ') s++;
    if (!*s) return false;
    char* end = nullptr;
    out = strtod(s, &end);
    if (end == s) return false;
    return true;
}

class ConverterWindow : public Window {
public:
    ConverterWindow() : Window("Converter", kWinW, kWinH, WF_NO_ZOOM) {
        seg_ = (SegmentedControl*)add(new SegmentedControl(Rect(30, 20, 640, 30), {"Length", "Mass", "Temperature", "Volume", "Speed", "Data"}));
        seg_->selected = 0;
        seg_->on_change = [this](int i) { set_category(i); };

        from_ = (TextField*)add(new TextField(Rect(52, 100, 330, 46), "0"));
        to_ = (TextField*)add(new TextField(Rect(52, 196, 330, 46), ""));
        from_->font_size = 22;
        to_->font_size = 22;
        from_->accept_char = accept_number;
        to_->accept_char = accept_number;
        from_->on_change = [this] { from_left_ = true; recalc(); };
        to_->on_change = [this] { from_left_ = false; recalc(); };

        from_unit_ = (Dropdown*)add(new Dropdown(Rect(398, 105, 240, 36)));
        to_unit_ = (Dropdown*)add(new Dropdown(Rect(398, 201, 240, 36)));
        from_unit_->on_change = [this](int) { recalc(); };
        to_unit_->on_change = [this](int) { recalc(); };

        swap_ = (Button*)add(new Button(Rect(332, 154, 36, 36), ""));
        swap_->icon = [](Canvas& c, Rect r) { draw_symbol(c, r, Symbol::Refresh, theme().accent, 2.0f); };
        swap_->on_click = [this] { swap(); };

        set_category(0);
        from_->set_text("1");
        recalc();
    }

    void on_paint(Canvas& c) override {
        const Theme& t = theme();
        Window::on_paint(c);
        const Table tb = table_for(category_);

        // conversion card
        draw_group_box(c, Rect(30, 66, 640, 230));
        c.draw_text(52, 80, "From", t.text_secondary, 12, Font::Medium);
        c.draw_text(52, 176, "To", t.text_secondary, 12, Font::Medium);
        char line[160];
        char b[48];
        double one_to = convert(tb.units[from_unit_->selected], tb.units[to_unit_->selected], 1.0);
        format_number(one_to, b, sizeof(b));
        snprintf(line, sizeof(line), "1 %s = %s %s", tb.units[from_unit_->selected].sym, b, tb.units[to_unit_->selected].sym);
        c.draw_text_in(Rect(52, 256, 586, 20), line, t.text_secondary, 14, Font::Regular, Align::Center);

        // reference card
        draw_group_box(c, Rect(30, 312, 640, 170));
        char head[96];
        snprintf(head, sizeof(head), "Common %s conversions from 1 %s", kCategoryNames[category_],
                 tb.units[from_unit_->selected].name);
        c.draw_text(52, 326, head, t.text, 13, Font::Medium);
        int col = 0, row = 0;
        for (int i = 0; i < tb.count && row < 4; i++) {
            if (i == from_unit_->selected) continue;
            char v[48];
            format_number(convert(tb.units[from_unit_->selected], tb.units[i], 1.0), v, sizeof(v));
            int x = 52 + col * 290, y = 358 + row * 26;
            c.draw_text_in(Rect(x, y, 140, 22), tb.units[i].name, t.text, 13, Font::Regular);
            String value = String(v) + " " + tb.units[i].sym;
            c.draw_text_in(Rect(x + 130, y, 120, 22), value.c_str(), t.text_secondary, 13, Font::Medium, Align::Right);
            if (++col == 2) { col = 0; row++; }
        }

        // the cards were drawn over the controls: repaint them on top
        for (Widget* w : widgets()) w->paint(c);
    }

private:
    static bool accept_number(uint32_t ch) {
        return (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E';
    }

    void set_category(int cat) {
        category_ = cat;
        Table tb = table_for(cat);
        from_unit_->options.clear();
        to_unit_->options.clear();
        for (int i = 0; i < tb.count; i++) {
            String label = String(tb.units[i].name) + " (" + tb.units[i].sym + ")";
            from_unit_->options.push(label);
            to_unit_->options.push(label);
        }
        from_unit_->selected = 0;
        to_unit_->selected = kDefaultTo[cat] < tb.count ? kDefaultTo[cat] : 1;
        recalc();
        invalidate();
    }

    // Recomputes the side that was not edited last.
    void recalc() {
        if (updating_) return;
        updating_ = true;
        Table tb = table_for(category_);
        int fi = from_unit_->selected, ti = to_unit_->selected;
        if (fi < 0 || ti < 0 || fi >= tb.count || ti >= tb.count) { updating_ = false; return; }
        if (from_left_) {
            double v;
            if (parse_number(from_->get_text(), v)) {
                char buf[48];
                format_number(convert(tb.units[fi], tb.units[ti], v), buf, sizeof(buf));
                to_->set_text(buf);
            } else {
                to_->set_text("");
            }
        } else {
            double v;
            if (parse_number(to_->get_text(), v)) {
                char buf[48];
                format_number(convert(tb.units[ti], tb.units[fi], v), buf, sizeof(buf));
                from_->set_text(buf);
            } else {
                from_->set_text("");
            }
        }
        updating_ = false;
        invalidate();
    }

    void swap() {
        int fi = from_unit_->selected;
        from_unit_->selected = to_unit_->selected;
        to_unit_->selected = fi;
        String a = from_->text, b = to_->text;
        from_->set_text(b.c_str());
        to_->set_text(a.c_str());
        from_left_ = true;
        recalc();
    }

    static constexpr int kDefaultTo[CategoryCount] = {5, 3, 1, 4, 2, 2};

    SegmentedControl* seg_ = nullptr;
    TextField* from_ = nullptr;
    TextField* to_ = nullptr;
    Dropdown* from_unit_ = nullptr;
    Dropdown* to_unit_ = nullptr;
    Button* swap_ = nullptr;
    int category_ = Length;
    bool from_left_ = true;
    bool updating_ = false;
};

void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x30D158), hex(0x0E8A3A));
    float u = r.w / 100.0f;
    // two opposing arrows
    c.draw_line(22 * u + r.x, 38 * u + r.y, 74 * u + r.x, 38 * u + r.y, hex(0xFFFFFF), 6 * u);
    PointF head1[3] = {{r.x + 80 * u, r.y + 38 * u}, {r.x + 64 * u, r.y + 26 * u}, {r.x + 64 * u, r.y + 50 * u}};
    c.fill_polygon(head1, 3, hex(0xFFFFFF));
    c.draw_line(26 * u + r.x, 64 * u + r.y, 78 * u + r.x, 64 * u + r.y, hex(0xFFFFFF), 6 * u);
    PointF head2[3] = {{r.x + 20 * u, r.y + 64 * u}, {r.x + 36 * u, r.y + 52 * u}, {r.x + 36 * u, r.y + 76 * u}};
    c.fill_polygon(head2, 3, hex(0xFFFFFF));
}

Window* create() { return new ConverterWindow(); }

AppRegistrar reg({"converter", "Converter", icon, create, "Utilities", true, false, 100, "Convert length, mass, temperature and more"});

}  // namespace
