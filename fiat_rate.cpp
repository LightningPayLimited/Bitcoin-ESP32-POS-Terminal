#include "fiat_rate.h"
#include "config.h"
#include "tls_bundle.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>

static double        cachedRate = 0;
static String        cachedCur;
static unsigned long cachedAt = 0;

static String httpsGetJson(const String& url) {
    HTTPClient http;
    WiFiClientSecure client;
    useRootCaBundle(client);
    client.setTimeout(10);
    client.setHandshakeTimeout(10);

    if (!http.begin(client, url)) return "";
    http.addHeader("Accept", "application/json");
    http.setTimeout(10000);
    http.setConnectTimeout(10000);

    unsigned long t0 = millis();
    int code = http.GET();
    String payload;
    if (code > 0) {
        payload = http.getString();
        Serial.printf("[RATE] GET %dms code=%d (%d bytes)\n",
                      (int)(millis() - t0), code, payload.length());
        if (code >= 400) payload = "";
    } else {
        Serial.printf("[RATE] GET err %d %s\n", code, url.c_str());
    }
    http.end();
    return payload;
}

// Coinbase: {"data":{"amount":"132365.91","base":"BTC","currency":"NZD"}}
static bool fetchCoinbase(const String& cur, double& out) {
    char url[128];
    snprintf(url, sizeof(url), RATE_URL_PRIMARY, cur.c_str());
    String resp = httpsGetJson(url);
    if (resp.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    // Make sure we got the pair we asked for, not an error body.
    if (String(doc["data"]["base"] | "") != "BTC" ||
        String(doc["data"]["currency"] | "") != cur) return false;
    const char* amt = doc["data"]["amount"] | "";
    out = atof(amt);
    return isfinite(out) && out > 0;
}

// CoinGecko: {"bitcoin":{"nzd":132383}}  (currency key is lowercase)
static bool fetchCoinGecko(const String& cur, double& out) {
    String lc = cur; lc.toLowerCase();
    char url[160];
    snprintf(url, sizeof(url), RATE_URL_FALLBACK, lc.c_str());
    String resp = httpsGetJson(url);
    if (resp.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    if (doc["bitcoin"][lc.c_str()].isNull()) return false;   // unknown currency -> {}
    out = doc["bitcoin"][lc.c_str()] | 0.0;
    return isfinite(out) && out > 0;
}

// A live tick that disagrees with a recent good rate by more than this is
// distrusted; it is then accepted only if the other live source agrees
// with it (a real >25% move), otherwise the recent rate is reused.
#define RATE_MAX_DEVIATION 0.25

static bool plausible(double price, const String& cur) {
    bool haveRecent = cachedRate > 0 && cachedCur == cur &&
                      millis() - cachedAt < RATE_STALE_MAX_MS;
    if (!haveRecent) return true;
    double dev = fabs(price - cachedRate) / cachedRate;
    if (dev > RATE_MAX_DEVIATION) {
        Serial.printf("[RATE] %.2f deviates %.0f%% from recent %.2f — distrusting\n",
                      price, dev * 100, cachedRate);
        return false;
    }
    return true;
}

RateResult fetchBtcRate(const String& currency) {
    RateResult r = {};
    String cur = currency;
    cur.toUpperCase();
    if (cur.isEmpty()) { r.error = "No currency"; return r; }

    double price = 0, p1 = 0, p2 = 0;
    bool ok1 = fetchCoinbase(cur, p1);
    if (ok1 && plausible(p1, cur)) {
        price = p1; r.source = "coinbase";
    } else {
        bool ok2 = fetchCoinGecko(cur, p2);
        bool agree = ok1 && ok2 && fabs(p2 - p1) / p1 <= RATE_MAX_DEVIATION;
        if (ok2 && (plausible(p2, cur) || agree)) {
            price = p2; r.source = agree && !plausible(p2, cur) ? "coinbase+coingecko" : "coingecko";
        }
    }
    if (price > 0) {
        // fall through to cache update below
    } else if (cachedRate > 0 && cachedCur == cur &&
               millis() - cachedAt < RATE_STALE_MAX_MS) {
        Serial.printf("[RATE] live sources failed/implausible — using %lus-old cached rate\n",
                      (millis() - cachedAt) / 1000);
        r.ok = true; r.btcPrice = cachedRate; r.stale = true; r.source = "cache";
        return r;
    } else {
        r.error = "Exchange rate unavailable";
        return r;
    }

    cachedRate = price;
    cachedCur  = cur;
    cachedAt   = millis();
    r.ok = true;
    r.btcPrice = price;
    Serial.printf("[RATE] 1 BTC = %.2f %s (%s)\n", price, cur.c_str(), r.source.c_str());
    return r;
}
