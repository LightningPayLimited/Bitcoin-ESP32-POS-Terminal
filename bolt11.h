#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

// ============================================================
// BOLT11 invoice + LNURL (bech32) decoding — minimal, allocation-light,
// no Arduino dependencies so it can be unit-tested on the host.
//
// bolt11Decode() verifies the bech32 checksum and the field structure
// and extracts what the POS needs (amount, payment hash, expiry,
// description). It does NOT verify the node signature — the POS has
// no secp256k1 and doesn't need it: we only use the invoice to display
// a QR and to sanity-check what the LNURL service handed us.
// ============================================================

struct Bolt11 {
    std::string prefix;        // "lnbc" | "lntb" | "lntbs" | "lnbcrt"
    bool        hasAmount = false;
    uint64_t    amountMsat = 0;   // valid when hasAmount
    uint64_t    timestamp = 0;    // seconds since epoch
    uint32_t    expirySec = 3600; // 'x' tag, BOLT11 default when absent
    bool        hasPaymentHash = false;
    uint8_t     paymentHash[32] = {0};
    bool        hasDescHash = false;
    uint8_t     descHash[32] = {0};
    std::string description;      // 'd' tag (UTF-8), empty if absent
};

/// Decode a bech32 BOLT11 invoice (either case). Returns false and sets
/// *err (if given) on any structural/checksum problem.
bool bolt11Decode(const char* invoice, Bolt11& out, std::string* err = nullptr);

/// Decode a bech32 LNURL (LUD-01, "lnurl1..." either case) to its URL.
bool lnurlDecode(const char* lnurl, std::string& urlOut);

/// Lowercase hex of a byte buffer.
std::string bytesToHex(const uint8_t* data, size_t len);

/// Parse exactly `outLen` bytes of hex (either case). False on bad input.
bool hexToBytes(const char* hex, uint8_t* out, size_t outLen);
