#include "bolt11.h"
#include <string.h>
#include <stdint.h>
#include <vector>

// ------------------------------------------------------------
// bech32 (BIP-173 charset + checksum; no 90-char limit, as BOLT11 and
// LNURL both exceed it)
// ------------------------------------------------------------
static const char* BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static int8_t bech32CharValue(char c) {
    const char* p = strchr(BECH32_CHARSET, c);
    return (p && c) ? (int8_t)(p - BECH32_CHARSET) : -1;
}

static uint32_t bech32Polymod(const std::vector<uint8_t>& values) {
    static const uint32_t GEN[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa,
                                    0x3d4233dd, 0x2a1462b3};
    uint32_t chk = 1;
    for (uint8_t v : values) {
        uint32_t top = chk >> 25;
        chk = ((chk & 0x1ffffff) << 5) ^ v;
        for (int i = 0; i < 5; i++) {
            if ((top >> i) & 1) chk ^= GEN[i];
        }
    }
    return chk;
}

// Split + checksum-verify a bech32 string. On success `hrp` is lowercase
// and `data` holds the 5-bit groups WITHOUT the 6 checksum groups.
static bool bech32Decode(const char* in, std::string& hrp,
                         std::vector<uint8_t>& data, std::string* err) {
    if (!in) { if (err) *err = "empty"; return false; }
    size_t len = strlen(in);
    if (len < 8) { if (err) *err = "too short"; return false; }

    // Case: all-lower or all-upper, never mixed. Normalise to lower.
    bool hasLower = false, hasUpper = false;
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c < 33 || c > 126) { if (err) *err = "bad char"; return false; }
        if (c >= 'a' && c <= 'z') hasLower = true;
        if (c >= 'A' && c <= 'Z') { hasUpper = true; c = (char)(c - 'A' + 'a'); }
        s.push_back(c);
    }
    if (hasLower && hasUpper) { if (err) *err = "mixed case"; return false; }

    size_t sep = s.rfind('1');
    if (sep == std::string::npos || sep == 0 || sep + 7 > s.size()) {
        if (err) *err = "no separator";
        return false;
    }

    hrp = s.substr(0, sep);
    data.clear();
    data.reserve(s.size() - sep - 1);
    for (size_t i = sep + 1; i < s.size(); i++) {
        int8_t v = bech32CharValue(s[i]);
        if (v < 0) { if (err) *err = "bad data char"; return false; }
        data.push_back((uint8_t)v);
    }

    // Checksum: polymod(hrpExpand(hrp) ++ data) == 1
    std::vector<uint8_t> values;
    values.reserve(hrp.size() * 2 + 1 + data.size());
    for (char c : hrp) values.push_back((uint8_t)c >> 5);
    values.push_back(0);
    for (char c : hrp) values.push_back((uint8_t)c & 31);
    values.insert(values.end(), data.begin(), data.end());
    if (bech32Polymod(values) != 1) { if (err) *err = "bad checksum"; return false; }

    data.resize(data.size() - 6);
    return true;
}

// Repack 5-bit groups into bytes (MSB first). Leftover bits (<8) are
// dropped — BOLT11 pads 256-bit fields to 52 groups (4 spare bits).
static std::vector<uint8_t> groupsToBytes(const uint8_t* g, size_t n) {
    std::vector<uint8_t> out;
    out.reserve((n * 5) / 8 + 1);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 5) | (g[i] & 31);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xff));
        }
    }
    return out;
}

// Big-endian integer from up to 12 groups (60 bits).
static bool groupsToUint(const uint8_t* g, size_t n, uint64_t& out) {
    if (n > 12) return false;
    out = 0;
    for (size_t i = 0; i < n; i++) out = (out << 5) | (g[i] & 31);
    return true;
}

std::string bytesToHex(const uint8_t* data, size_t len) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s.push_back(H[data[i] >> 4]);
        s.push_back(H[data[i] & 15]);
    }
    return s;
}

bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    if (!hex || strlen(hex) != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i * 2 + k];
            uint8_t v;
            if      (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return false;
            b = (b << 4) | v;
        }
        out[i] = b;
    }
    return true;
}

// ------------------------------------------------------------
// HRP: "ln" + currency prefix + optional amount + optional multiplier
// ------------------------------------------------------------
static bool parseHrp(const std::string& hrp, Bolt11& out, std::string* err) {
    if (hrp.size() < 4 || hrp.compare(0, 2, "ln") != 0) {
        if (err) *err = "not a lightning invoice";
        return false;
    }
    size_t i = 2;
    while (i < hrp.size() && hrp[i] >= 'a' && hrp[i] <= 'z') i++;
    out.prefix = hrp.substr(0, i);
    if (out.prefix.size() <= 2) { if (err) *err = "bad prefix"; return false; }
    if (out.prefix != "lnbc" && out.prefix != "lntb" &&
        out.prefix != "lntbs" && out.prefix != "lnbcrt") {
        if (err) *err = "unknown network prefix";
        return false;
    }

    if (i == hrp.size()) { out.hasAmount = false; return true; }

    // Amount digits: 19 keeps the accumulator inside uint64 (the
    // per-multiplier check below rejects anything the msat product
    // couldn't hold). Pico amounts of a few BTC legitimately run to 15+.
    uint64_t amount = 0;
    size_t digits = 0;
    while (i < hrp.size() && hrp[i] >= '0' && hrp[i] <= '9') {
        if (++digits > 19) { if (err) *err = "amount too large"; return false; }
        amount = amount * 10 + (hrp[i] - '0');
        i++;
    }
    if (digits == 0) { if (err) *err = "bad amount"; return false; }

    char mult = 0;
    if (i < hrp.size()) {
        mult = hrp[i++];
        if (i != hrp.size()) { if (err) *err = "trailing hrp chars"; return false; }
    }

    // 1 BTC = 1e11 msat. Bound each product so it can't wrap uint64.
    uint64_t scale;
    switch (mult) {
        case 0:   scale = 100000000000ULL; break;   // BTC
        case 'm': scale = 100000000ULL;    break;   // 1e-3 BTC
        case 'u': scale = 100000ULL;       break;   // 1e-6 BTC
        case 'n': scale = 100ULL;          break;   // 1e-9 BTC
        case 'p':                                   // 1e-12 BTC = 0.1 msat
            if (amount % 10 != 0) { if (err) *err = "pico amount not msat-aligned"; return false; }
            out.amountMsat = amount / 10;
            out.hasAmount = true;
            return true;
        default:  if (err) *err = "bad amount multiplier"; return false;
    }
    if (amount > UINT64_MAX / scale) { if (err) *err = "amount too large"; return false; }
    out.amountMsat = amount * scale;
    out.hasAmount = true;
    return true;
}

// ------------------------------------------------------------
// Tagged fields
// ------------------------------------------------------------
bool bolt11Decode(const char* invoice, Bolt11& out, std::string* err) {
    out = Bolt11();

    std::string hrp;
    std::vector<uint8_t> data;
    if (!bech32Decode(invoice, hrp, data, err)) return false;
    if (!parseHrp(hrp, out, err)) return false;

    // timestamp (7 groups) + tagged fields + signature (104 groups)
    if (data.size() < 7 + 104) { if (err) *err = "data too short"; return false; }
    uint64_t ts;
    groupsToUint(&data[0], 7, ts);
    out.timestamp = ts;

    size_t pos = 7;
    const size_t end = data.size() - 104;   // signature is the tail
    while (pos < end) {
        if (pos + 3 > end) { if (err) *err = "truncated tag"; return false; }
        uint8_t type = data[pos];
        size_t  len  = ((size_t)data[pos + 1] << 5) | data[pos + 2];
        pos += 3;
        if (pos + len > end) { if (err) *err = "tag overruns data"; return false; }
        const uint8_t* field = &data[pos];

        switch (type) {
            case 1:   // p — payment hash (256 bits in 52 groups)
                if (len == 52 && !out.hasPaymentHash) {
                    std::vector<uint8_t> b = groupsToBytes(field, len);
                    memcpy(out.paymentHash, b.data(), 32);
                    out.hasPaymentHash = true;
                }
                break;   // wrong length → skip (per spec)
            case 23:  // h — description hash
                if (len == 52 && !out.hasDescHash) {
                    std::vector<uint8_t> b = groupsToBytes(field, len);
                    memcpy(out.descHash, b.data(), 32);
                    out.hasDescHash = true;
                }
                break;
            case 13: { // d — description (UTF-8)
                std::vector<uint8_t> b = groupsToBytes(field, len);
                out.description.assign(b.begin(), b.end());
                break;
            }
            case 6: {  // x — expiry seconds
                uint64_t x;
                if (groupsToUint(field, len, x) && x <= 0xffffffffULL) {
                    out.expirySec = (uint32_t)x;
                }
                break;
            }
            default:  // s, n, f, r, 9, c, m, unknown — not needed here
                break;
        }
        pos += len;
    }

    if (!out.hasPaymentHash) { if (err) *err = "no payment hash"; return false; }
    return true;
}

// ------------------------------------------------------------
// LNURL (LUD-01): bech32 with hrp "lnurl", data = URL bytes
// ------------------------------------------------------------
bool lnurlDecode(const char* lnurl, std::string& urlOut) {
    std::string hrp;
    std::vector<uint8_t> data;
    if (!bech32Decode(lnurl, hrp, data, nullptr)) return false;
    if (hrp != "lnurl") return false;
    std::vector<uint8_t> b = groupsToBytes(data.data(), data.size());
    urlOut.assign(b.begin(), b.end());
    return !urlOut.empty();
}
