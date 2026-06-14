#pragma once
#include <Arduino.h>

// ============================================================
// Config Store — NVS-backed persistent configuration
// Stores: WiFi creds + payment-provider settings.
//   - Stacked: API key (server URL is compiled in)
//   - BTCPay : server URL, Greenfield API key, store ID, currency
// ============================================================

enum class Provider { STACKED, BTCPAY };

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

    /// Persist the store ID chosen after reboot (BTCPay store selection).
    bool saveStoreId(const String& storeId);

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

private:
    bool   _provisioned = false;
    String _ssid;
    String _pass;
    Provider _provider = Provider::STACKED;
    String _apiKey;
    String _btcpayUrl;
    String _storeId;
    String _currency;
};
