#include "stacked_api.h"
#include "config.h"
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
    inv.reference      = result["reference"] | "";
    inv.nzdAmount      = result["nzdAmount"] | 0.0f;
    inv.satAmount      = result["satAmount"] | (uint64_t)0;

    if (inv.paymentRequest.isEmpty()) {
        inv.error = "Empty payment_request";
        return inv;
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

    String url = _base + "/api/merchant/payment?reference=" + reference;
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

    ps.ok        = true;
    ps.reference = result["reference"] | "";
    ps.status    = result["status"] | "";
    ps.satAmount = result["satAmount"] | (uint64_t)0;
    ps.nzdAmount = result["nzdAmount"] | 0.0f;
    ps.paidDate  = result["paidDate"] | "";
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
    client.setInsecure();  // TODO: pin Stacked root CA

    if (!http.begin(client, url)) return "";

    http.addHeader("Accept", "application/json");
    http.addHeader("api-key", _key);
    http.setTimeout(10000);

    int code = http.GET();
    String payload;
    if (code > 0) {
        payload = http.getString();
        if (code != 200)
            Serial.printf("[HTTP] GET %d %s\n", code, url.c_str());
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

    if (!http.begin(client, url)) return "";

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("api-key", _key);
    http.setTimeout(10000);

    int code = http.POST(body);
    String payload;
    if (code > 0) {
        payload = http.getString();
        if (code != 200 && code != 201)
            Serial.printf("[HTTP] POST %d %s → %s\n", code, url.c_str(), payload.c_str());
    } else {
        Serial.printf("[HTTP] POST err %d %s\n", code, url.c_str());
    }

    http.end();
    return payload;
}
