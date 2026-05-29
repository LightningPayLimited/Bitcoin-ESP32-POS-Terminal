#pragma once
#include <Arduino.h>

// ============================================================
// Boltcard / LNURL-withdraw client
//
// A Boltcard's NDEF holds an LNURLW (lnurlw://…?p=…&c=…). To pull payment:
//   1. GET that URL          -> withdraw request { callback, k1, ... }
//   2. GET callback?k1=&pr=  -> { status: "OK" }  (wallet then pays the bolt11)
// Once OK, the merchant's normal payment poll detects settlement.
// ============================================================

struct BoltcardResult {
    bool   ok;
    String error;   // human-readable, set when ok == false
};

// Resolve the card's LNURLW and submit `bolt11` to its callback.
// ok == true means the LNURLW service accepted the invoice (status OK).
BoltcardResult boltcardPay(const String& lnurlwUrl, const String& bolt11);
