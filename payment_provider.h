#pragma once
#include <Arduino.h>
#include <time.h>
#include <vector>

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

// One transaction as returned by the provider's history endpoint.
// `satAmount`/`nzdAmount` are the effective amounts — the received value
// when settled, otherwise the invoiced value. Times are UTC epoch seconds.
struct TxRecord {
    String   reference;
    uint64_t satAmount;
    float    nzdAmount;
    time_t   createdAt;   // 0 if the timestamp couldn't be parsed
    bool     isPaid;
};

// Live status of a past invoice, re-checked on demand from the history screen.
enum class InvoiceState { ERROR, PENDING, PAID, EXPIRED };

// One page of transaction history.
struct TxPage {
    bool                  ok;
    int                   total;     // total records on the server (pre-filter)
    int                   rawCount;  // records the server returned this page,
                                     // before any client-side type filtering
    std::vector<TxRecord> records;
    String                error;
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

    /// Fetch one page of transaction history within [fromIso, toIso]
    /// (ISO-8601 UTC). Providers that don't support history return
    /// ok == false (the default below) so the POS can show a notice.
    virtual TxPage getTransactions(const String& fromIso, const String& toIso,
                                   int limit, int offset) {
        TxPage p = {};
        p.error = "Transaction history not supported";
        return p;
    }

    /// Re-check a past invoice's live status by reference. Default unsupported.
    virtual InvoiceState checkInvoiceState(const String& reference) {
        return InvoiceState::ERROR;
    }
};
