#include "config_store.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

bool ConfigStore::begin() {
    prefs.begin(NVS_NAMESPACE, true);  // read-only

    String done = prefs.getString(NVS_KEY_SETUP, "");
    _provisioned = (done == "1");

    if (_provisioned) {
        _ssid   = prefs.getString(NVS_KEY_SSID, "");
        _pass   = prefs.getString(NVS_KEY_PASS, "");
        _apiKey = prefs.getString(NVS_KEY_APIKEY, "");

        // Sanity check
        if (_ssid.isEmpty() || _apiKey.isEmpty()) {
            _provisioned = false;
        }
    }

    prefs.end();

    Serial.printf("[CFG] Provisioned: %s\n", _provisioned ? "yes" : "no");
    if (_provisioned) {
        Serial.printf("[CFG] SSID: %s | API key: %.8s...\n",
                      _ssid.c_str(), _apiKey.c_str());
    }

    return _provisioned;
}

bool ConfigStore::save(const String& ssid, const String& pass, const String& apiKey) {
    prefs.begin(NVS_NAMESPACE, false);  // read-write

    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, pass);
    prefs.putString(NVS_KEY_APIKEY, apiKey);
    prefs.putString(NVS_KEY_SETUP, "1");

    prefs.end();

    _ssid = ssid;
    _pass = pass;
    _apiKey = apiKey;
    _provisioned = true;

    Serial.printf("[CFG] Saved! SSID=%s key=%.8s...\n", ssid.c_str(), apiKey.c_str());
    return true;
}

bool ConfigStore::markUnprovisioned() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_SETUP, "0");
    prefs.end();
    _provisioned = false;
    Serial.println("[CFG] Marked unprovisioned (creds retained)");
    return true;
}

bool ConfigStore::clear() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();

    _ssid = "";
    _pass = "";
    _apiKey = "";
    _provisioned = false;

    Serial.println("[CFG] Factory reset — config cleared");
    return true;
}
