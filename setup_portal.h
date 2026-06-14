#pragma once
#include <Arduino.h>
#include "config_store.h"

// ============================================================
// Setup Portal
// Two provisioning methods:
//
// 1. Captive Portal (WiFi AP mode)
//    - Device creates "StackedPOS-Setup" WiFi network
//    - Merchant connects and opens 192.168.4.1
//    - Web form: WiFi SSID, WiFi password, API key
//    - Saves to NVS and reboots
//
// 2. Serial Config (Web Serial from Stacked webpage)
//    - Device listens on USB serial for JSON config
//    - Stacked webpage sends: {"ssid":"...","pass":"...","apiKey":"..."}
//    - Saves to NVS and reboots
// ============================================================

class SetupPortal {
public:
    /// The setup AP SSID, with a random 4-char base64 suffix appended so
    /// multiple POS devices being provisioned at once don't collide.
    /// Generated once on first call, then cached for the rest of the boot.
    static String apSSID();

    /// Start the captive portal AP + web server.
    /// Blocks until config is received, then reboots.
    void runCaptivePortal(ConfigStore& store);

    /// Check serial for incoming config JSON.
    /// Call this in loop() — non-blocking.
    /// Returns true if config was received and saved.
    bool checkSerial(ConfigStore& store);
};
