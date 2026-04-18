#pragma once
#include <Arduino.h>

// ============================================================
// Stacked Merchant API Client
// Base: https://app.stackedbitcoin.com
// Auth: api-key header
//
// POST /api/merchant/payment   — create/refresh invoice
// GET  /api/merchant/payment   — poll payment status
// GET  /api/merchant/profile   — merchant name/GST
// ============================================================

struct MerchantInvoice {
    bool     ok;
    String   paymentRequest;  // BOLT11 invoice string
    String   lnurl;
    String   reference;       // Transaction ref (for polling + refresh)
    float    nzdAmount;
    uint64_t satAmount;
    String   error;
};

struct PaymentStatus {
    bool     ok;
    String   reference;
    String   status;
    uint64_t satAmount;
    float    nzdAmount;
    String   paidDate;
    bool     isPaid;          // true when paidDate is set
    String   error;
};

struct MerchantProfile {
    bool   ok;
    String companyName;
    String gstNumber;
    String error;
};

class StackedAPI {
public:
    void begin(const String& baseUrl, const String& apiKey);

    /// Create a new invoice. Amount is in NZD.
    /// Stacked handles NZD→sats conversion at current rate.
    MerchantInvoice createInvoice(float nzdAmount,
                                  const String& details = "",
                                  const String& txlink = "");

    /// Refresh an expired invoice via txRef.
    /// Gets new bolt11 with updated BTC/NZD rate.
    MerchantInvoice refreshInvoice(const String& txRef);

    /// Poll payment status. Paid when result.isPaid == true.
    PaymentStatus checkPayment(const String& reference);

    /// Get merchant profile.
    MerchantProfile getProfile();

private:
    String _base;
    String _key;
    String doGet(const String& url);
    String doPost(const String& url, const String& body);
    MerchantInvoice parseInvoiceResponse(const String& resp);
};
