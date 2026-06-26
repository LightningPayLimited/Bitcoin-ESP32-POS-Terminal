#pragma once
#include <Arduino.h>
#include <vector>
#include "payment_provider.h"

// A store the API key can access (for post-setup store selection).
struct BTCPayStore {
    String id;
    String name;
};

// ============================================================
// BTCPay Server client — Greenfield API
// Auth: "Authorization: token <apiKey>"
//
// POST /api/v1/stores/{store}/invoices                       — create
// GET  /api/v1/stores/{store}/invoices/{id}/payment-methods  — bolt11
// GET  /api/v1/stores/{store}/invoices/{id}                  — status
//
// The Greenfield key needs the btcpay.store.cancreateinvoice and
// btcpay.store.canviewinvoices permissions.
//
// HTTPS endpoints are accepted without certificate pinning
// (setInsecure) so self-hosted servers with arbitrary certs work;
// plain-HTTP servers (e.g. a LAN instance) are also supported.
// ============================================================

class BTCPayAPI : public PaymentProvider {
public:
    void begin(const String& serverUrl, const String& apiKey,
               const String& storeId, const String& currency);

    /// List stores the API key can access. Empty on error/none.
    /// Used during setup so the merchant picks a store instead of
    /// hand-entering its ID.
    std::vector<BTCPayStore> listStores();

    /// Point the client at a store chosen after listStores().
    void setStore(const String& storeId) { _store = storeId; }
    String storeId() const { return _store; }

    MerchantInvoice createInvoice(float amount,
                                  const String& details = "",
                                  const String& txlink = "") override;

    /// BTCPay invoices can't be refreshed in place — this creates a
    /// fresh invoice for the same fiat amount as the last create.
    MerchantInvoice refreshInvoice(const String& reference) override;

    PaymentStatus checkPayment(const String& reference) override;

    /// Not supported by Greenfield in a single call — returns ok=false.
    MerchantProfile getProfile() override;

private:
    String _base;
    String _key;
    String _store;
    String _currency;
    float  _lastAmount = 0;   // for refreshInvoice()

    String invoicesUrl() const { return _base + "/api/v1/stores/" + _store + "/invoices"; }

    // method: "GET" or "POST". Empty body for GET.
    String doRequest(const char* method, const String& url, const String& body);
    MerchantInvoice fetchBolt11(MerchantInvoice inv);
};
