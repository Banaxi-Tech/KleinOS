#pragma once
// Wi-Fi interface between the menu bar panel and a wireless driver. A driver
// registers itself with wifi_register; until one does, wifi_driver() is null.
#include <stdint.h>

namespace net {

struct WifiNetwork {
    char ssid[33];
    uint8_t signal;      // 0..4 bars
    bool secured;
};

enum class WifiStatus { Idle, Connecting, Connected, WrongPassword, Failed, Unsupported };

struct WifiDriver {
    void (*scan)();                                     // start a new scan
    bool (*scanning)();
    int (*results)(WifiNetwork* out, int max);          // strongest first
    void (*connect)(const char* ssid, const char* password);   // "" for open networks; 64 hex digits are the raw WPA key
    void (*disconnect)();
    WifiStatus (*status)();
    const char* (*connected_ssid)();                    // "" when not connected
};

void wifi_register(const WifiDriver* driver);
const WifiDriver* wifi_driver();

// Joins and remembers the network once it connects. Stores the derived WPA key, never the password.
void wifi_connect(const char* ssid, const char* password);
bool wifi_connect_saved(const char* ssid);              // false if this network isn't remembered
void wifi_forget(const char* ssid);
void wifi_set_panel_open(bool open);                    // no background joining while the user is choosing
void wifi_autoconnect_init();                           // reconnect to remembered networks; call after drivers load
// Fake networks for trying the panel without hardware (boot argument wifidemo=1).
void wifi_init_demo();

}  // namespace net
