#pragma once
// Battery state shown by the shell. A .dri driver reports it; nothing reports on desktops.
#include <stdint.h>

struct BatteryStatus {
    bool present = false;
    int percent = 0;           // 0..100
    bool charging = false;
    bool on_ac = false;
};

void battery_report(const BatteryStatus& status);
const BatteryStatus& battery_status();
// Changes whenever the reported state does.
uint32_t battery_generation();
