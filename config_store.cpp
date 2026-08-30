#include "config_store.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

const char* ConfigStore::providerName(Provider p) {
    switch (p) {
        case Provider::BTCPAY:    return "btcpay";
        case Provider::LNADDRESS: return "lnaddress";
        default:                  return "stacked";
    }
}

bool ConfigStore::begin() {
    prefs.begin(NVS_NAMESPACE, true);  // read-only

    String done = prefs.getString(NVS_KEY_SETUP, "");
    _provisioned = (done == "1");

    // Fields are loaded whether or not setup is complete: after
    // markUnprovisioned() the captive portal pre-fills them so the
    // merchant only re-types what was wrong.
    _ssid   = prefs.getString(NVS_KEY_SSID, "");
    _pass   = prefs.getString(NVS_KEY_PASS, "");
    _apiKey = prefs.getString(NVS_KEY_APIKEY, "");

    // Provider defaults to Stacked for devices provisioned before
    // BTCPay support existed (no NVS_KEY_PROVIDER stored).
    String prov = prefs.getString(NVS_KEY_PROVIDER, "stacked");
    if      (prov == "btcpay")    _provider = Provider::BTCPAY;
    else if (prov == "lnaddress") _provider = Provider::LNADDRESS;
    else                          _provider = Provider::STACKED;
    _btcpayUrl = prefs.getString(NVS_KEY_BTCPAY_URL, "");
    _storeId   = prefs.getString(NVS_KEY_STORE_ID, "");
    _currency  = prefs.getString(NVS_KEY_CURRENCY, BTCPAY_DEFAULT_CURRENCY);
    _lnAddress = prefs.getString(NVS_KEY_LN_ADDR, "");
    _storeName = prefs.getString(NVS_KEY_LN_NAME, "");
    _lnVerified = prefs.getString(NVS_KEY_LN_OK, "") == "1";

    if (_provisioned) {
        // Sanity check — required fields per provider.
        if (_ssid.isEmpty()) {
            _provisioned = false;
        }
        if (_provider == Provider::LNADDRESS) {
            // Self-custody has no API key — just the address.
            if (_lnAddress.isEmpty()) _provisioned = false;
        } else if (_apiKey.isEmpty()) {
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
                      providerName(_provider), _ssid.c_str(), _apiKey.c_str());
        if (_provider == Provider::BTCPAY) {
            Serial.printf("[CFG] BTCPay: %s store=%s cur=%s\n",
                          _btcpayUrl.c_str(), _storeId.c_str(), _currency.c_str());
        } else if (_provider == Provider::LNADDRESS) {
            Serial.printf("[CFG] LnAddress: %s cur=%s name=%s\n",
                          _lnAddress.c_str(), _currency.c_str(), _storeName.c_str());
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
    // Clear any stale BTCPay / self-custody fields from a previous
    // provisioning.
    prefs.putString(NVS_KEY_BTCPAY_URL, "");
    prefs.putString(NVS_KEY_STORE_ID, "");
    prefs.putString(NVS_KEY_LN_ADDR, "");
    prefs.putString(NVS_KEY_LN_NAME, "");
    prefs.putString(NVS_KEY_SETUP, "1");

    prefs.end();

    _ssid = ssid;
    _pass = pass;
    _provider = Provider::STACKED;
    _apiKey = apiKey;
    _btcpayUrl = "";
    _storeId = "";
    _lnAddress = "";
    _storeName = "";
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
    prefs.putString(NVS_KEY_LN_ADDR, "");
    prefs.putString(NVS_KEY_LN_NAME, "");
    prefs.putString(NVS_KEY_SETUP, "1");

    prefs.end();

    _ssid = ssid;
    _pass = pass;
    _provider = Provider::BTCPAY;
    _apiKey = apiKey;
    _btcpayUrl = serverUrl;
    _storeId = storeId;
    _currency = cur;
    _lnAddress = "";
    _storeName = "";
    _provisioned = true;

    Serial.printf("[CFG] Saved (BTCPay)! SSID=%s url=%s store=%s cur=%s\n",
                  ssid.c_str(), serverUrl.c_str(), storeId.c_str(), cur.c_str());
    return true;
}

bool ConfigStore::saveLnAddress(const String& ssid, const String& pass,
                                const String& lnAddress, const String& currency,
                                const String& storeName) {
    String cur = currency.length() ? currency : String(LNADDR_DEFAULT_CURRENCY);
    cur.toUpperCase();

    prefs.begin(NVS_NAMESPACE, false);  // read-write

    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, pass);
    prefs.putString(NVS_KEY_PROVIDER, "lnaddress");
    prefs.putString(NVS_KEY_LN_ADDR, lnAddress);
    prefs.putString(NVS_KEY_LN_NAME, storeName);
    prefs.putString(NVS_KEY_LN_OK, "");        // re-probe the new address
    prefs.putString(NVS_KEY_CURRENCY, cur);
    // Clear the other providers' fields.
    prefs.putString(NVS_KEY_APIKEY, "");
    prefs.putString(NVS_KEY_BTCPAY_URL, "");
    prefs.putString(NVS_KEY_STORE_ID, "");
    prefs.putString(NVS_KEY_SETUP, "1");

    prefs.end();

    _ssid = ssid;
    _pass = pass;
    _provider = Provider::LNADDRESS;
    _apiKey = "";
    _btcpayUrl = "";
    _storeId = "";
    _currency = cur;
    _lnAddress = lnAddress;
    _storeName = storeName;
    _lnVerified = false;
    _provisioned = true;

    Serial.printf("[CFG] Saved (LnAddress)! SSID=%s addr=%s cur=%s name=%s\n",
                  ssid.c_str(), lnAddress.c_str(), cur.c_str(), storeName.c_str());
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

bool ConfigStore::saveLnVerified() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_LN_OK, "1");
    prefs.end();
    _lnVerified = true;
    Serial.println("[CFG] Lightning Address verified (LUD-21 OK)");
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
    _lnAddress = "";
    _storeName = "";
    _lnVerified = false;
    _provisioned = false;

    Serial.println("[CFG] Factory reset — config cleared");
    return true;
}
