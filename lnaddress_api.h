#pragma once
#include <Arduino.h>
#include <vector>
#include "payment_provider.h"

// ============================================================
// Self-custody provider — Lightning Address / LNURL-pay
//
// No intermediary: the POS asks the merchant's own wallet for invoices
// and confirms settlement itself.
//
//   resolve   : user@domain -> https://domain/.well-known/lnurlp/user  (LUD-16)
//               GET -> { callback, minSendable, maxSendable, metadata,
//                        commentAllowed }                              (LUD-06/12)
//   invoice   : GET callback?amount=<msat>[&comment=…]
//               -> { pr, verify }                                      (LUD-21)
//   settle    : GET verify -> { settled: true, preimage, pr }          (LUD-21)
//
// Fiat -> sats uses the public spot rate (fiat_rate.h); "SATS" as the
// currency skips conversion. Every HTTPS call validates the server
// certificate against the root-CA bundle (tls_bundle.h) — with bare
// LNURL there is no other guard against an invoice being swapped for
// an attacker's on the wire.
//
// The wallet MUST implement LUD-21 `verify`; without it the POS has no
// way to learn that a bolt11 was paid. probeVerifySupport() checks this
// once after provisioning so the merchant finds out before the first
// customer (main.cpp persists the result and skips the probe afterwards).
// ============================================================

class LnAddressAPI : public PaymentProvider {
public:
    enum class Probe { OK, UNSUPPORTED, ERROR };

    void begin(const String& address, const String& currency,
               const String& storeName);

    /// Purely local check that the configured address can be turned into
    /// an https LNURL-pay URL (no network). False = fix the address.
    bool validate(String& errOut) const { String u; return addressToUrl(u, errOut); }

    /// Resolve the address to its LNURL-pay parameters. One HTTPS GET.
    /// On failure returns false with a short user-facing reason.
    bool resolve(String& errOut);
    bool resolved() const { return _resolved; }

    /// Request a minimum-amount invoice and check the callback returned
    /// a LUD-21 verify URL. UNSUPPORTED is permanent (wrong wallet);
    /// ERROR is transient (network / wallet down) — retry.
    /// Leaves one tiny unpaid invoice in the wallet.
    Probe probeVerifySupport(String& errOut);

    MerchantInvoice createInvoice(float amount,
                                  const String& details = "",
                                  const String& txlink = "") override;

    /// New invoice at the same fiat amount (re-rated). If the wallet can't
    /// be reached but the live invoice is still valid, hands that one back
    /// so the sale keeps going. The previous invoice stays in the sale's
    /// list so a customer who paid it right at the swap is still detected.
    MerchantInvoice refreshInvoice(const String& reference) override;

    /// Polls the referenced invoice's verify URL, plus one older invoice
    /// from the same sale per call (round-robin).
    PaymentStatus checkPayment(const String& reference) override;

    /// Store name if set, else the address.
    MerchantProfile getProfile() override;

    void endSale() override;

    /// The normalised address (what payments are routed to).
    String address()  const { return _address; }
    String endpoint() const { return _lnurlpUrl; }

    /// What receipts/splash show as the payee: the user@domain address,
    /// or for URL / lnurl1 forms the identifier the service advertises
    /// (LUD-06 text/identifier), else just the LNURL host.
    String payeeLabel() const;

private:
    struct SaleInvoice {
        String   ref;          // first 16 hex chars of the payment hash
        String   pr;           // bolt11 (lowercase as received)
        String   verifyUrl;
        uint8_t  payHash[32];
        uint64_t sats;
        float    fiat;
        unsigned long validUntilMs;   // millis() when the bolt11 expires
    };

    String   _address;
    String   _currency;
    String   _storeName;

    // Resolved LNURL-pay parameters
    String   _lnurlpUrl;
    String   _callback;
    uint64_t _minMsat = 0;
    uint64_t _maxMsat = 0;
    int      _commentAllowed = 0;
    String   _identifier;      // metadata text/identifier | text/email
    bool     _resolved = false;

    // Current sale
    float    _lastFiat = 0;
    String   _lastDetails;
    std::vector<SaleInvoice> _sale;
    size_t   _rr = 0;          // round-robin cursor over older invoices

    String httpsGet(const String& url, int& codeOut, unsigned long timeoutMs);
    bool   addressToUrl(String& urlOut, String& err) const;
    bool   fetchCallback(uint64_t msat, const String& comment,
                         String& prOut, String& verifyOut, String& err);
    bool   fiatToMsat(float fiat, uint64_t& msatOut, String& err);
    MerchantInvoice requestInvoice(float fiat, const String& details);
    MerchantInvoice asMerchantInvoice(const SaleInvoice& rec) const;
    bool   pollVerify(const SaleInvoice& inv, bool& settledOut, String& err);
    const SaleInvoice* findRef(const String& ref) const;
};
