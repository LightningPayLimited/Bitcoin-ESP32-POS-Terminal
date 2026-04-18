#pragma once
#include <Arduino.h>

// ============================================================
// Config Store — NVS-backed persistent configuration
// Stores: WiFi SSID, WiFi password, Stacked API key
// ============================================================

class ConfigStore {
public:
    /// Load config from NVS. Returns true if provisioned.
    bool begin();

    /// Check if device has been provisioned
    bool isProvisioned() const { return _provisioned; }

    /// Save all config to NVS and mark as provisioned
    bool save(const String& ssid, const String& pass, const String& apiKey);

    /// Clear all config (factory reset)
    bool clear();

    /// Flip the "provisioned" flag off without wiping creds.
    /// Used when WiFi fails to connect — keeps SSID/pass around
    /// so the portal can pre-fill them for the user to fix.
    bool markUnprovisioned();

    String ssid()   const { return _ssid; }
    String pass()   const { return _pass; }
    String apiKey() const { return _apiKey; }

private:
    bool   _provisioned = false;
    String _ssid;
    String _pass;
    String _apiKey;
};
