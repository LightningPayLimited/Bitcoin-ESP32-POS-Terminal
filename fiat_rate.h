#pragma once
#include <Arduino.h>

// ============================================================
// BTC/fiat spot rate for the self-custody provider (Stacked and BTCPay
// convert server-side; a bare Lightning Address can't).
//
// Sources (no API key, TLS validated against the root-CA bundle):
//   1. Coinbase   GET /v2/prices/BTC-<CUR>/spot   -> data.amount
//   2. CoinGecko  GET /simple/price?ids=bitcoin&vs_currencies=<cur>
// The last good rate is cached; if both sources fail, a cached value
// younger than RATE_STALE_MAX_MS is returned with stale == true.
// ============================================================

struct RateResult {
    bool   ok;
    double btcPrice;   // one BTC in `currency`
    bool   stale;      // served from cache because live fetches failed
    String source;     // "coinbase" | "coingecko" | "cache"
    String error;      // set when ok == false
};

RateResult fetchBtcRate(const String& currency);
