#pragma once
#include <Arduino.h>

// ============================================================
// Single owner of how money is written on screen, paper and in the
// LNURL comment. The shipped GFX fonts cover ASCII only, so non-dollar
// currencies get no symbol rather than a wrong one ("10.00 EUR").
// ============================================================

/// "$" for dollar currencies, "" otherwise (and for SATS).
inline const char* currencyPrefix(const String& cur) {
    return (cur == "NZD" || cur == "AUD" || cur == "USD" || cur == "CAD") ? "$" : "";
}

/// "$12.50 NZD", "12.50 EUR", or "1234 sats" when cur == "SATS".
inline String formatAmount(float fiat, uint64_t sats, const String& cur) {
    char buf[48];
    if (cur == "SATS") {
        snprintf(buf, sizeof(buf), "%lu sats", (unsigned long)sats);
    } else {
        snprintf(buf, sizeof(buf), "%s%.2f %s", currencyPrefix(cur), fiat, cur.c_str());
    }
    return String(buf);
}
