#pragma once
#include <Arduino.h>

// ============================================================
// Payment Provider — common interface for the POS backend.
//
// The POS state machine in main.cpp talks to this interface and
// doesn't care whether invoices come from Stacked or a merchant's
// own BTCPay Server. Concrete implementations:
//   - StackedAPI  (stacked_api.h)
//   - BTCPayAPI   (btcpay_api.h, BTCPay Greenfield API)
//
// Amounts are expressed in the provider's fiat currency (NZD for
// Stacked, the merchant-configured currency for BTCPay). The struct
// field is still named `nzdAmount` for historical reasons — read it
// as "fiat amount in the active currency".
// ============================================================

struct MerchantInvoice {
    bool     ok;
    String   paymentRequest;  // BOLT11 invoice string
    String   lnurl;
    String   reference;       // Provider's handle for poll + refresh
    float    nzdAmount;       // Fiat amount (active currency)
    uint64_t satAmount;
    String   error;
};

struct PaymentStatus {
    bool     ok;
    String   reference;
    String   status;
    uint64_t satAmount;
    float    nzdAmount;       // Fiat amount (active currency)
    String   paidDate;
    bool     isPaid;
    String   error;
};

struct MerchantProfile {
    bool   ok;
    String companyName;
    String gstNumber;
    String error;
};

class PaymentProvider {
public:
    virtual ~PaymentProvider() {}

    /// Create a new invoice for the given fiat amount.
    virtual MerchantInvoice createInvoice(float amount,
                                          const String& details = "",
                                          const String& txlink = "") = 0;

    /// Get a fresh invoice (new bolt11 / updated rate) for an active sale.
    virtual MerchantInvoice refreshInvoice(const String& reference) = 0;

    /// Poll payment status. Paid when result.isPaid == true.
    virtual PaymentStatus checkPayment(const String& reference) = 0;

    /// Merchant profile (name/GST). ok == false if unsupported.
    virtual MerchantProfile getProfile() = 0;
};
