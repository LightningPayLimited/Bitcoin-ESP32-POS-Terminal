#include "stacked_api.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

void StackedAPI::begin(const String& baseUrl, const String& apiKey) {
    _base = baseUrl;
    _key = apiKey;
    if (_base.endsWith("/")) _base.remove(_base.length() - 1);
}

// ================================================================
// Shared response parser
// ================================================================

// Parse sats from a BOLT11 invoice string.
// Format: lnbc<amount><multiplier>1<data>  where multiplier ∈ {m, u, n, p}.
//   no multiplier → amount is BTC  (×100_000_000 sats)
//   m (milli)     → amount × 100_000
//   u (micro)     → amount × 100
//   n (nano)      → amount / 10
//   p (pico)      → amount / 10000
static uint64_t satsFromBolt11(const String& s) {
    if (s.length() < 5) return 0;
    int i = 0;
    // Skip prefix (lnbc, lntb, lntbs, lnbcrt, etc.)
    while (i < (int)s.length() && (s[i] < '0' || s[i] > '9')) i++;
    uint64_t amount = 0;
    while (i < (int)s.length() && s[i] >= '0' && s[i] <= '9') {
        amount = amount * 10 + (s[i] - '0');
        i++;
    }
    char mult = (i < (int)s.length()) ? s[i] : 0;
    switch (mult) {
        case 'm': return amount * 100000ULL;
        case 'u': return amount * 100ULL;
        case 'n': return amount / 10ULL;
        case 'p': return amount / 10000ULL;
        default:  return amount * 100000000ULL;
    }
}

MerchantInvoice StackedAPI::parseInvoiceResponse(const String& resp) {
    MerchantInvoice inv = {};

    if (resp.isEmpty()) {
        inv.error = "No response from Stacked";
        return inv;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
        inv.error = "JSON parse error";
        return inv;
    }

    JsonObject result = doc["result"];
    if (result.isNull()) {
        inv.error = doc["error"] | doc["message"] | "No result in response";
        return inv;
    }

    inv.paymentRequest = result["payment_request"] | "";
    inv.lnurl          = result["lnurl"] | "";

    // Stacked nests the transaction record under result.tx — that's where
    // the short alphanumeric `reference` and amount fields live.
    JsonObject tx = result["tx"];
    inv.reference = tx["reference"] | result["reference"] | "";
    inv.nzdAmount = tx["nzdAmount"] | result["nzdAmount"] | 0.0f;
    inv.satAmount = tx["satAmount"] | result["satAmount"] | (uint64_t)0;

    Serial.printf("[PARSE] payment_request=%s...  reference=%s\n",
                  inv.paymentRequest.substring(0, 40).c_str(),
                  inv.reference.c_str());

    if (inv.paymentRequest.isEmpty()) {
        inv.error = "Empty payment_request";
        return inv;
    }

    // If the API didn't return satAmount, derive it from the bolt11 invoice.
    if (inv.satAmount == 0) {
        inv.satAmount = satsFromBolt11(inv.paymentRequest);
    }

    inv.ok = true;
    return inv;
}

// ================================================================
// POST /api/merchant/payment — Create new invoice
//
// The POS sends amount in NZD. Stacked converts to sats at
// current BTC/NZD rate and returns both nzdAmount and satAmount.
//
// Body: { "amount": <nzd_float>, "details": "...", "txlink": "..." }
// Resp: { "result": { "payment_request", "lnurl", "reference",
//                      "nzdAmount", "satAmount" } }
// ================================================================

MerchantInvoice StackedAPI::createInvoice(float nzdAmount,
                                          const String& details,
                                          const String& txlink) {
    JsonDocument body;
    body["amount"]  = nzdAmount;
    body["txRef"]   = "";  // Empty = new transaction
    if (details.length() > 0) body["details"] = details;
    if (txlink.length() > 0)  body["txlink"]  = txlink;

    String bodyStr;
    serializeJson(body, bodyStr);

    Serial.printf("[API] Creating invoice: $%.2f NZD\n", nzdAmount);
    String resp = doPost(_base + "/api/merchant/payment", bodyStr);
    MerchantInvoice inv = parseInvoiceResponse(resp);

    if (inv.ok) {
        Serial.printf("[API] Invoice OK: %lu sats ($%.2f NZD) ref=%s\n",
                      (unsigned long)inv.satAmount, inv.nzdAmount,
                      inv.reference.c_str());
    } else {
        Serial.printf("[API] Invoice FAIL: %s\n", inv.error.c_str());
    }

    return inv;
}

// ================================================================
// POST /api/merchant/payment — Refresh via txRef
// Body: { "txRef": "<reference>" }
// Gets new bolt11 with updated BTC/NZD rate
// ================================================================

MerchantInvoice StackedAPI::refreshInvoice(const String& txRef) {
    JsonDocument body;
    body["txRef"] = txRef;

    String bodyStr;
    serializeJson(body, bodyStr);

    Serial.printf("[API] Refreshing invoice ref=%s\n", txRef.c_str());
    String resp = doPost(_base + "/api/merchant/payment", bodyStr);
    MerchantInvoice inv = parseInvoiceResponse(resp);

    if (inv.ok) {
        Serial.printf("[API] Refresh OK: %lu sats ($%.2f NZD)\n",
                      (unsigned long)inv.satAmount, inv.nzdAmount);
    }

    return inv;
}

// ================================================================
// GET /api/merchant/payment?reference=<ref>
// Paid when paidDate is set (non-null, non-empty)
// ================================================================

PaymentStatus StackedAPI::checkPayment(const String& reference) {
    PaymentStatus ps = {};
    if (reference.isEmpty()) {
        ps.error = "No reference to poll";
        return ps;
    }

    String url = _base + "/api/merchant/payment?reference=" + reference;
    Serial.printf("[POLL] reference=%s\n", reference.c_str());
    String resp = doGet(url);

    if (resp.isEmpty()) {
        ps.error = "No response";
        return ps;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
        ps.error = "JSON parse error";
        return ps;
    }

    JsonObject result = doc["result"];
    if (result.isNull()) {
        ps.error = doc["error"] | "No result";
        return ps;
    }

    // Fields may be at top level OR nested under .tx depending on endpoint
    JsonObject tx = result["tx"];
    auto get = [&](const char* key) -> JsonVariantConst {
        JsonVariantConst v = tx[key];
        if (v.isNull()) v = result[key];
        return v;
    };

    ps.ok        = true;
    ps.reference = get("reference") | "";
    ps.status    = get("status") | "";
    ps.satAmount = get("satAmount") | (uint64_t)0;
    ps.nzdAmount = get("nzdAmount") | 0.0f;
    ps.paidDate  = get("paidDate") | "";
    ps.isPaid    = (ps.paidDate.length() > 0 && ps.paidDate != "null");

    return ps;
}

// ================================================================
// GET /api/merchant/profile
// ================================================================

MerchantProfile StackedAPI::getProfile() {
    MerchantProfile mp = {};
    String resp = doGet(_base + "/api/merchant/profile");

    if (resp.isEmpty()) { mp.error = "No response"; return mp; }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { mp.error = "Parse error"; return mp; }

    JsonObject result = doc["result"];
    if (result.isNull()) { mp.error = "No result"; return mp; }

    mp.ok          = true;
    mp.companyName = result["companyName"] | "";
    mp.gstNumber   = result["gstNumber"] | "";
    return mp;
}

// ================================================================
// HTTP helpers
// ================================================================

String StackedAPI::doGet(const String& url) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);
    client.setHandshakeTimeout(20);

    if (!http.begin(client, url)) return "";

    http.addHeader("Accept", "application/json");
    http.addHeader("api-key", _key);
    http.setTimeout(20000);
    http.setConnectTimeout(20000);

    unsigned long t0 = millis();
    int code = http.GET();
    String payload;
    if (code > 0) {
        payload = http.getString();
        Serial.printf("[HTTP] GET %dms code=%d (%d bytes): %s\n",
                      (int)(millis() - t0), code, payload.length(),
                      payload.length() > 300
                          ? (payload.substring(0, 300) + "...").c_str()
                          : payload.c_str());
    } else {
        Serial.printf("[HTTP] GET err %d %s\n", code, url.c_str());
    }

    http.end();
    return payload;
}

String StackedAPI::doPost(const String& url, const String& body) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);            // 20s socket timeout
    client.setHandshakeTimeout(20);   // 20s TLS handshake

    // Quick DNS/reachability diagnostic — extract host from URL
    int hostStart = url.indexOf("://");
    if (hostStart > 0) hostStart += 3;
    int hostEnd = url.indexOf('/', hostStart);
    String host = url.substring(hostStart, hostEnd > 0 ? hostEnd : url.length());
    IPAddress resolved;
    if (WiFi.hostByName(host.c_str(), resolved)) {
        Serial.printf("[HTTP] DNS %s -> %s\n", host.c_str(), resolved.toString().c_str());
    } else {
        Serial.printf("[HTTP] DNS FAILED for %s\n", host.c_str());
    }

    if (!http.begin(client, url)) return "";

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("api-key", _key);
    http.setTimeout(20000);           // 20s HTTP-level timeout
    http.setConnectTimeout(20000);

    unsigned long t0 = millis();
    int code = http.POST(body);
    Serial.printf("[HTTP] POST took %lums, code=%d\n", millis() - t0, code);

    String payload;
    if (code > 0) {
        payload = http.getString();
        Serial.printf("[HTTP] RESP (%d bytes):\n", payload.length());
        // Print in chunks so we see the whole thing (Serial buffers limit printf length)
        for (int i = 0; i < (int)payload.length(); i += 256) {
            Serial.print(payload.substring(i, i + 256));
        }
        Serial.println();
    } else {
        Serial.printf("[HTTP] POST err %d %s\n", code, url.c_str());
    }

    http.end();
    return payload;
}
