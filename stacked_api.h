#pragma once
#include <Arduino.h>
#include "payment_provider.h"

// ============================================================
// Stacked Merchant API Client
// Base: https://app.stackedbitcoin.com
// Auth: api-key header
//
// POST /api/merchant/payment   — create/refresh invoice
// GET  /api/merchant/payment   — poll payment status
// GET  /api/merchant/profile   — merchant name/GST
// ============================================================

class StackedAPI : public PaymentProvider {
public:
    void begin(const String& baseUrl, const String& apiKey);

    /// Create a new invoice. Amount is in NZD.
    /// Stacked handles NZD→sats conversion at current rate.
    MerchantInvoice createInvoice(float nzdAmount,
                                  const String& details = "",
                                  const String& txlink = "") override;

    /// Refresh an expired invoice via txRef.
    /// Gets new bolt11 with updated BTC/NZD rate.
    MerchantInvoice refreshInvoice(const String& txRef) override;

    /// Poll payment status. Paid when result.isPaid == true.
    PaymentStatus checkPayment(const String& reference) override;

    /// Get merchant profile.
    MerchantProfile getProfile() override;

    /// GET /api/merchant/transactions — one page of history.
    TxPage getTransactions(const String& fromIso, const String& toIso,
                           int limit, int offset) override;

    /// Re-check a past invoice via POST /api/merchant/payment { txRef }.
    InvoiceState checkInvoiceState(const String& reference) override;

private:
    String _base;
    String _key;
    String doGet(const String& url);
    String doPost(const String& url, const String& body);
    MerchantInvoice parseInvoiceResponse(const String& resp);
};
