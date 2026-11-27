#include "drivers/battery.h"

static BatteryStatus g_status;
static uint32_t g_generation;

void battery_report(const BatteryStatus& s) {
    if (s.present == g_status.present && s.percent == g_status.percent && s.charging == g_status.charging && s.on_ac == g_status.on_ac) return;
    g_status = s;
    g_generation++;
}

const BatteryStatus& battery_status() { return g_status; }
uint32_t battery_generation() { return g_generation; }
