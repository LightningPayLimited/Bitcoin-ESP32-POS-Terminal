#include "btcpay_api.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// Parse sats from a BOLT11 invoice string (fallback when the API doesn't
// give us the crypto amount directly). Same logic as the Stacked client.
//   lnbc<amount><multiplier>1<data>, multiplier ∈ {m,u,n,p}.
static uint64_t btcpaySatsFromBolt11(const String& s) {
    if (s.length() < 5) return 0;
    int i = 0;
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

void BTCPayAPI::begin(const String& serverUrl, const String& apiKey,
                      const String& storeId, const String& currency) {
    _base = serverUrl;
    if (_base.endsWith("/")) _base.remove(_base.length() - 1);
    _key      = apiKey;
    _store    = storeId;
    _currency = currency.length() ? currency : String("NZD");
}

// ================================================================
// POST /api/v1/stores/{store}/invoices
// Body: { "amount": "<fiat>", "currency": "NZD" }
// Resp: { "id", "amount", "currency", "checkoutLink", "status", ... }
// (No bolt11 here — fetched separately via payment-methods.)
// ================================================================
MerchantInvoice BTCPayAPI::createInvoice(float amount,
                                         const String& details,
                                         const String& txlink) {
    _lastAmount = amount;

    JsonDocument body;
    body["amount"]   = String(amount, 2);
    body["currency"] = _currency;
    if (details.length() > 0) {
        body["metadata"]["itemDesc"] = details;
    }

    String bodyStr;
    serializeJson(body, bodyStr);

    Serial.printf("[BTCPAY] Creating invoice: %.2f %s\n", amount, _currency.c_str());
    String resp = doRequest("POST", invoicesUrl(), bodyStr);

    MerchantInvoice inv = {};
    if (resp.isEmpty()) { inv.error = "No response from BTCPay"; return inv; }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { inv.error = "JSON parse error"; return inv; }

    inv.reference = doc["id"] | "";
    inv.nzdAmount = (doc["amount"] | String("0")).toFloat();
    if (inv.reference.isEmpty()) {
        inv.error = doc["message"] | "No invoice id in response";
        return inv;
    }

    // Second call: resolve the lightning bolt11 for this invoice.
    inv = fetchBolt11(inv);
    if (inv.ok) {
        Serial.printf("[BTCPAY] Invoice OK: %lu sats (%.2f %s) id=%s\n",
                      (unsigned long)inv.satAmount, inv.nzdAmount,
                      _currency.c_str(), inv.reference.c_str());
    } else {
        Serial.printf("[BTCPAY] Invoice FAIL: %s\n", inv.error.c_str());
    }
    return inv;
}

// ================================================================
// GET /api/v1/stores/{store}/invoices/{id}/payment-methods
// Resp: [ { "paymentMethodId":"BTC-LN", "destination":"lnbc...",
//          "amount":"0.00003", "rate":"65000", ... }, ... ]
// ================================================================
MerchantInvoice BTCPayAPI::fetchBolt11(MerchantInvoice inv) {
    String url = invoicesUrl() + "/" + inv.reference + "/payment-methods";
    String resp = doRequest("GET", url, "");
    if (resp.isEmpty()) { inv.error = "No payment-methods response"; return inv; }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { inv.error = "payment-methods parse error"; return inv; }

    JsonArray methods = doc.as<JsonArray>();
    for (JsonObject m : methods) {
        String pmid = m["paymentMethodId"] | "";
        String dest = m["destination"] | "";
        if (dest.isEmpty()) {
            // Older BTCPay used paymentLink ("lightning:lnbc...").
            String link = m["paymentLink"] | "";
            link.replace("lightning:", "");
            dest = link;
        }

        bool isLn = pmid.indexOf("LN") >= 0 ||
                    pmid.indexOf("Lightning") >= 0 ||
                    dest.startsWith("ln");
        if (!isLn || dest.isEmpty()) continue;

        inv.paymentRequest = dest;

        // Crypto amount → sats. Prefer the API's amount, else parse bolt11.
        String cryptoAmt = m["amount"] | "";
        if (cryptoAmt.length() > 0) {
            inv.satAmount = (uint64_t)(cryptoAmt.toFloat() * 1e8 + 0.5);
        }
        if (inv.satAmount == 0) {
            inv.satAmount = btcpaySatsFromBolt11(dest);
        }
        inv.ok = true;
        return inv;
    }

    inv.error = "No lightning payment method (is LN enabled on the store?)";
    return inv;
}

MerchantInvoice BTCPayAPI::refreshInvoice(const String& reference) {
    Serial.println("[BTCPAY] Refreshing — creating new invoice at same amount");
    return createInvoice(_lastAmount);
}

// ================================================================
// GET /api/v1/stores/{store}/invoices/{id}
// Resp: { "id", "status":"New|Processing|Settled|Expired|Invalid",
//         "amount", "currency", ... }
// Lightning settles to "Settled". Older servers may say Complete/Confirmed.
// ================================================================
PaymentStatus BTCPayAPI::checkPayment(const String& reference) {
    PaymentStatus ps = {};
    if (reference.isEmpty()) { ps.error = "No reference to poll"; return ps; }

    String url = invoicesUrl() + "/" + reference;
    String resp = doRequest("GET", url, "");
    if (resp.isEmpty()) { ps.error = "No response"; return ps; }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { ps.error = "JSON parse error"; return ps; }

    ps.reference = reference;
    ps.status    = doc["status"] | "";
    ps.nzdAmount = (doc["amount"] | String("0")).toFloat();

    String s = ps.status;
    s.toLowerCase();
    ps.isPaid = (s == "settled" || s == "complete" || s == "confirmed");
    ps.ok = true;

    // BTCPay stamps a monotonically-increasing receipt time; expose it if
    // present so the receipt printer has something to show.
    uint64_t mon = doc["monitoringExpiration"] | (uint64_t)0;
    (void)mon;

    return ps;
}

// ================================================================
// GET /api/v1/stores
// Resp: [ { "id", "name", "defaultCurrency", ... }, ... ]
// ================================================================
std::vector<BTCPayStore> BTCPayAPI::listStores() {
    std::vector<BTCPayStore> out;

    String resp = doRequest("GET", _base + "/api/v1/stores", "");
    if (resp.isEmpty()) {
        Serial.println("[BTCPAY] listStores: no response");
        return out;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
        Serial.println("[BTCPAY] listStores: JSON parse error");
        return out;
    }

    for (JsonObject s : doc.as<JsonArray>()) {
        BTCPayStore st;
        st.id   = s["id"]   | "";
        st.name = s["name"] | "";
        if (st.id.length()) out.push_back(st);
    }

    Serial.printf("[BTCPAY] listStores: %d store(s)\n", (int)out.size());
    return out;
}

// GET /api/v1/stores/{store} — used to show the store name as the
// "merchant" on the splash/receipts.
MerchantProfile BTCPayAPI::getProfile() {
    MerchantProfile mp = {};
    if (_store.isEmpty()) { mp.error = "no store"; return mp; }

    String resp = doRequest("GET", _base + "/api/v1/stores/" + _store, "");
    if (resp.isEmpty()) { mp.error = "No response"; return mp; }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { mp.error = "Parse error"; return mp; }

    mp.companyName = doc["name"] | "";
    mp.ok = mp.companyName.length() > 0;
    return mp;
}

// ================================================================
// HTTP helper — handles both http:// (LAN) and https:// (setInsecure).
// ================================================================
String BTCPayAPI::doRequest(const char* method, const String& url,
                            const String& body) {
    bool https = url.startsWith("https");

    HTTPClient http;
    WiFiClient      plain;
    WiFiClientSecure secure;
    WiFiClient*     client;

    if (https) {
        secure.setInsecure();          // self-hosted certs — skip pinning
        secure.setTimeout(20);
        secure.setHandshakeTimeout(20);
        client = &secure;
    } else {
        plain.setTimeout(20);
        client = &plain;
    }

    if (!http.begin(*client, url)) return "";

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("Authorization", "token " + _key);
    http.setTimeout(20000);
    http.setConnectTimeout(20000);

    unsigned long t0 = millis();
    int code = (strcmp(method, "POST") == 0) ? http.POST(body) : http.GET();

    String payload;
    if (code > 0) {
        payload = http.getString();
        Serial.printf("[BTCPAY HTTP] %s %dms code=%d (%d bytes)\n",
                      method, (int)(millis() - t0), code, payload.length());
        if (code >= 400) {
            Serial.printf("[BTCPAY HTTP] err body: %s\n",
                          payload.substring(0, 300).c_str());
        }
    } else {
        Serial.printf("[BTCPAY HTTP] %s err %d %s\n", method, code, url.c_str());
    }

    http.end();
    return payload;
}
