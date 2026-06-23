#include "stacked_api.h"
#include "config.h"
#include "stacked_ca.h"
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

    // Stacked uses a top-level boolean `isPaid` on the poll response;
    // older versions only set `tx.paidDate` when paid. Honour both.
    bool isPaidFlag = result["isPaid"] | false;
    ps.isPaid = isPaidFlag ||
                (ps.paidDate.length() > 0 && ps.paidDate != "null");

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
// GET /api/merchant/transactions
//
// IMPORTANT: the endpoint takes EITHER a date range (from+to) OR
// limit+offset — never both. Sending all four returns nothing. This
// mirrors the reference POS client. The POS uses the date-range form
// for a single window fetch (see tx_history.cpp). Response shape:
//   { "result": { "data": [ { createdAt, satAmount, nzdAmount,
//                              receivedSat, receivedNzd, paidDate,
//                              type, reference, ... } ],
//                  "pagination": { "total": N } } }
// ================================================================

// Parse an ISO-8601 UTC timestamp ("2026-06-23T21:56:26.043Z") to epoch
// seconds. The trailing ".043Z" is ignored. Uses the days-from-civil
// algorithm so it doesn't depend on timezone/TZ state. Returns 0 on error.
static time_t parseIsoUtc(const char* s) {
    if (!s || !*s) return 0;
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    int y = Y - (M <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return (time_t)(days * 86400L + h * 3600L + m * 60L + sec);
}

TxPage StackedAPI::getTransactions(const String& fromIso, const String& toIso,
                                   int limit, int offset) {
    TxPage page = {};

    // Date range OR limit/offset — not both (see note above). The reference
    // client sends the ISO timestamps unencoded, so we do the same.
    String url = _base + "/api/merchant/transactions";
    if (fromIso.length() && toIso.length()) {
        url += "?from=" + fromIso + "&to=" + toIso;
    } else {
        url += "?limit=" + String(limit) + "&offset=" + String(offset);
    }

    Serial.printf("[API] GET %s\n", url.c_str());
    String resp = doGet(url);
    if (resp.isEmpty()) { page.error = "No response"; return page; }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { page.error = "JSON parse error"; return page; }

    JsonObject result = doc["result"];
    if (result.isNull()) {
        page.error = doc["error"] | doc["message"] | "No result";
        return page;
    }

    JsonArray data = result["data"];
    page.rawCount = (int)data.size();
    page.records.reserve(data.size());

    // DEBUG: tally the distinct `type` values so we can confirm which one
    // represents a POS sale. (No type filter applied yet — keep everything.)
    std::vector<String> tk; std::vector<int> tc;

    for (JsonObject o : data) {
        String type = o["type"] | "(none)";
        int fi = -1;
        for (size_t i = 0; i < tk.size(); i++) if (tk[i] == type) { fi = i; break; }
        if (fi < 0) { tk.push_back(type); tc.push_back(1); } else tc[fi]++;

        TxRecord r = {};
        r.reference = o["reference"] | "";
        r.createdAt = parseIsoUtc(o["createdAt"] | "");

        // Prefer the actually-received amount; fall back to the invoiced one.
        uint64_t recvSat = o["receivedSat"] | (uint64_t)0;
        r.satAmount = recvSat ? recvSat : (o["satAmount"] | (uint64_t)0);
        float recvNzd = o["receivedNzd"] | 0.0f;
        r.nzdAmount = recvNzd > 0 ? recvNzd : (o["nzdAmount"] | 0.0f);

        // Paid when paidDate is set (matches checkPayment's convention).
        const char* paid = o["paidDate"] | "";
        r.isPaid = (paid[0] != '\0' && strcmp(paid, "null") != 0);

        page.records.push_back(r);
    }

    page.total = result["pagination"]["total"] | page.rawCount;
    page.ok = true;

    String tally;
    for (size_t i = 0; i < tk.size(); i++) tally += tk[i] + "=" + tc[i] + "  ";
    Serial.printf("[API] tx types: %s\n", tally.c_str());
    Serial.printf("[API] Transactions: %d kept / %d raw (total %d)\n",
                  (int)page.records.size(), page.rawCount, page.total);
    return page;
}

// ================================================================
// POST /api/merchant/payment { txRef } — re-check a past invoice.
//
// Same call the POS uses to refresh an active invoice; here we read the
// status out of the response instead of the new bolt11:
//   PAID    — { success:false, error:"Transaction already paid" }, or an
//             isPaid/paidDate flag if the status ever comes back in a result
//   EXPIRED — an error / status mentioning expired / cancelled / void
//   PENDING — a fresh invoice came back (still open, not yet paid)
//   ERROR   — no/!parseable response, or an unrecognised error
// The full response is logged by doPost so the mapping can be refined.
// ================================================================
InvoiceState StackedAPI::checkInvoiceState(const String& reference) {
    if (reference.isEmpty()) return InvoiceState::ERROR;

    JsonDocument body;
    body["txRef"] = reference;
    String bodyStr;
    serializeJson(body, bodyStr);

    Serial.printf("[API] Checking invoice ref=%s\n", reference.c_str());
    String resp = doPost(_base + "/api/merchant/payment", bodyStr);
    if (resp.isEmpty()) return InvoiceState::ERROR;

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return InvoiceState::ERROR;

    // An already-paid (or expired) invoice can't be refreshed, so the server
    // replies { success:false, error:"Transaction already paid" } with no
    // result. Read the top-level error first.
    String err = doc["error"] | doc["message"] | "";
    err.toLowerCase();
    if (err.indexOf("paid") >= 0)    return InvoiceState::PAID;
    if (err.indexOf("expire") >= 0)  return InvoiceState::EXPIRED;

    JsonObject result = doc["result"];
    if (!result.isNull()) {
        JsonObject tx = result["tx"];
        auto get = [&](const char* key) -> JsonVariantConst {
            JsonVariantConst v = tx[key];
            if (v.isNull()) v = result[key];
            return v;
        };

        bool   isPaidFlag = result["isPaid"] | false;
        String paidDate   = get("paidDate") | "";
        if (isPaidFlag || (paidDate.length() > 0 && paidDate != "null"))
            return InvoiceState::PAID;

        String status = get("status") | "";
        status.toLowerCase();
        if (status.indexOf("expire") >= 0 || status.indexOf("cancel") >= 0 ||
            status.indexOf("void") >= 0)
            return InvoiceState::EXPIRED;

        // A refreshed invoice (new bolt11) means it's still open.
        return InvoiceState::PENDING;
    }

    // Some other error with no result.
    return err.length() ? InvoiceState::ERROR : InvoiceState::PENDING;
}

// ================================================================
// HTTP helpers
// ================================================================

String StackedAPI::doGet(const String& url) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setCACert(STACKED_ROOT_CA);
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
    client.setCACert(STACKED_ROOT_CA);
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
