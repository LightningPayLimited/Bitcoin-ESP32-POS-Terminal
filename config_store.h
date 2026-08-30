#pragma once
#include <Arduino.h>

// ============================================================
// Config Store — NVS-backed persistent configuration
// Stores: WiFi creds + payment-provider settings.
//   - Stacked  : API key (server URL is compiled in)
//   - BTCPay   : server URL, Greenfield API key, store ID, currency
//   - LnAddress: Lightning Address (self-custody), currency, store name
// ============================================================

enum class Provider { STACKED, BTCPAY, LNADDRESS };

class ConfigStore {
public:
    /// Load config from NVS. Returns true if provisioned.
    bool begin();

    /// Check if device has been provisioned
    bool isProvisioned() const { return _provisioned; }

    /// Save Stacked config (WiFi + API key) and mark provisioned.
    bool saveStacked(const String& ssid, const String& pass,
                     const String& apiKey);

    /// Save BTCPay config (WiFi + server/key/store/currency) and mark
    /// provisioned.
    bool saveBTCPay(const String& ssid, const String& pass,
                    const String& serverUrl, const String& apiKey,
                    const String& storeId, const String& currency);

    /// Save self-custody config (WiFi + Lightning Address/currency/optional
    /// store name) and mark provisioned. No API key involved.
    bool saveLnAddress(const String& ssid, const String& pass,
                       const String& lnAddress, const String& currency,
                       const String& storeName);

    /// Persist the store ID chosen after reboot (BTCPay store selection).
    bool saveStoreId(const String& storeId);

    /// Remember that the Lightning Address passed the LUD-21 verify probe
    /// so later boots skip it (and don't mint a probe invoice each time).
    bool saveLnVerified();
    bool lnVerified() const { return _lnVerified; }

    /// Clear all config (factory reset)
    bool clear();

    /// Flip the "provisioned" flag off without wiping creds.
    /// Used when WiFi fails to connect — keeps SSID/pass around
    /// so the portal can pre-fill them for the user to fix.
    bool markUnprovisioned();

    String   ssid()      const { return _ssid; }
    String   pass()      const { return _pass; }
    Provider provider()  const { return _provider; }
    String   apiKey()    const { return _apiKey; }
    String   btcpayUrl() const { return _btcpayUrl; }
    String   storeId()   const { return _storeId; }
    String   currency()  const { return _currency; }
    String   lnAddress() const { return _lnAddress; }
    String   storeName() const { return _storeName; }

    /// NVS string for a provider ("stacked" | "btcpay" | "lnaddress").
    static const char* providerName(Provider p);

private:
    bool   _provisioned = false;
    String _ssid;
    String _pass;
    Provider _provider = Provider::STACKED;
    String _apiKey;
    String _btcpayUrl;
    String _storeId;
    String _currency;
    String _lnAddress;
    String _storeName;
    bool   _lnVerified = false;
};
