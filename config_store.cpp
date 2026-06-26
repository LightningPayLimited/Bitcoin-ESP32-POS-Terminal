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

        // Provider defaults to Stacked for devices provisioned before
        // BTCPay support existed (no NVS_KEY_PROVIDER stored).
        String prov = prefs.getString(NVS_KEY_PROVIDER, "stacked");
        _provider  = (prov == "btcpay") ? Provider::BTCPAY : Provider::STACKED;
        _btcpayUrl = prefs.getString(NVS_KEY_BTCPAY_URL, "");
        _storeId   = prefs.getString(NVS_KEY_STORE_ID, "");
        _currency  = prefs.getString(NVS_KEY_CURRENCY, BTCPAY_DEFAULT_CURRENCY);

        // Sanity check — required fields per provider.
        if (_ssid.isEmpty() || _apiKey.isEmpty()) {
            _provisioned = false;
        }
        // BTCPay needs a server URL; the store ID is resolved after the
        // first reboot (the merchant picks from the stores their key can
        // access), so an empty store ID here is expected, not a failure.
        if (_provider == Provider::BTCPAY && _btcpayUrl.isEmpty()) {
            _provisioned = false;
        }
    }

    prefs.end();

    Serial.printf("[CFG] Provisioned: %s\n", _provisioned ? "yes" : "no");
    if (_provisioned) {
        Serial.printf("[CFG] Provider: %s | SSID: %s | key: %.8s...\n",
                      _provider == Provider::BTCPAY ? "btcpay" : "stacked",
                      _ssid.c_str(), _apiKey.c_str());
        if (_provider == Provider::BTCPAY) {
            Serial.printf("[CFG] BTCPay: %s store=%s cur=%s\n",
                          _btcpayUrl.c_str(), _storeId.c_str(), _currency.c_str());
        }
    }

    return _provisioned;
}

bool ConfigStore::saveStacked(const String& ssid, const String& pass,
                              const String& apiKey) {
    prefs.begin(NVS_NAMESPACE, false);  // read-write

    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, pass);
    prefs.putString(NVS_KEY_PROVIDER, "stacked");
    prefs.putString(NVS_KEY_APIKEY, apiKey);
    // Clear any stale BTCPay fields from a previous provisioning.
    prefs.putString(NVS_KEY_BTCPAY_URL, "");
    prefs.putString(NVS_KEY_STORE_ID, "");
    prefs.putString(NVS_KEY_SETUP, "1");

    prefs.end();

    _ssid = ssid;
    _pass = pass;
    _provider = Provider::STACKED;
    _apiKey = apiKey;
    _btcpayUrl = "";
    _storeId = "";
    _provisioned = true;

    Serial.printf("[CFG] Saved (Stacked)! SSID=%s key=%.8s...\n",
                  ssid.c_str(), apiKey.c_str());
    return true;
}

bool ConfigStore::saveBTCPay(const String& ssid, const String& pass,
                             const String& serverUrl, const String& apiKey,
                             const String& storeId, const String& currency) {
    String cur = currency.length() ? currency : String(BTCPAY_DEFAULT_CURRENCY);

    prefs.begin(NVS_NAMESPACE, false);  // read-write

    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, pass);
    prefs.putString(NVS_KEY_PROVIDER, "btcpay");
    prefs.putString(NVS_KEY_APIKEY, apiKey);
    prefs.putString(NVS_KEY_BTCPAY_URL, serverUrl);
    prefs.putString(NVS_KEY_STORE_ID, storeId);
    prefs.putString(NVS_KEY_CURRENCY, cur);
    prefs.putString(NVS_KEY_SETUP, "1");

    prefs.end();

    _ssid = ssid;
    _pass = pass;
    _provider = Provider::BTCPAY;
    _apiKey = apiKey;
    _btcpayUrl = serverUrl;
    _storeId = storeId;
    _currency = cur;
    _provisioned = true;

    Serial.printf("[CFG] Saved (BTCPay)! SSID=%s url=%s store=%s cur=%s\n",
                  ssid.c_str(), serverUrl.c_str(), storeId.c_str(), cur.c_str());
    return true;
}

bool ConfigStore::saveStoreId(const String& storeId) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_STORE_ID, storeId);
    prefs.end();
    _storeId = storeId;
    Serial.printf("[CFG] Store ID saved: %s\n", storeId.c_str());
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
    _provider = Provider::STACKED;
    _apiKey = "";
    _btcpayUrl = "";
    _storeId = "";
    _currency = "";
    _provisioned = false;

    Serial.println("[CFG] Factory reset — config cleared");
    return true;
}
