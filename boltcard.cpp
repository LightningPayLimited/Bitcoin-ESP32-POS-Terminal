#include "boltcard.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// NOTE: setInsecure() — we skip TLS cert validation because a Boltcard's
// LNURLW lives on an arbitrary domain we can't pin a CA for. Acceptable for
// getting the flow working; harden later with a CA bundle if needed.
static String httpsGet(const String& url, int& codeOut) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);
    client.setHandshakeTimeout(15);

    if (!http.begin(client, url)) { codeOut = -1000; return ""; }
    http.addHeader("Accept", "application/json");
    http.setTimeout(15000);
    http.setConnectTimeout(15000);

    codeOut = http.GET();
    String payload = (codeOut > 0) ? http.getString() : "";
    http.end();
    return payload;
}

BoltcardResult boltcardPay(const String& lnurlwUrl, const String& bolt11) {
    BoltcardResult r = { false, "" };
    if (bolt11.isEmpty()) { r.error = "No invoice"; return r; }

    // 1) Normalise scheme: lnurlw:// (and lnurl://) map to https://.
    String url = lnurlwUrl;
    if      (url.startsWith("lnurlw://")) url = "https://" + url.substring(9);
    else if (url.startsWith("lnurl://"))  url = "https://" + url.substring(8);
    else if (!url.startsWith("http"))     { r.error = "Not an LNURLW"; return r; }

    Serial.printf("[BOLT] Resolving withdraw request: %s\n", url.c_str());

    // 2) GET the withdraw request.
    int code;
    String resp = httpsGet(url, code);
    if (resp.isEmpty()) {
        Serial.printf("[BOLT] resolve failed code=%d\n", code);
        r.error = "Card server unreachable";
        return r;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { r.error = "Bad card response"; return r; }

    if (String(doc["status"] | "") == "ERROR") {
        r.error = doc["reason"] | "Card rejected";
        return r;
    }
    if (String(doc["tag"] | "") != "withdrawRequest") {
        r.error = "Not a withdraw card";
        return r;
    }

    String callback = doc["callback"] | "";
    String k1       = doc["k1"] | "";
    if (callback.isEmpty() || k1.isEmpty()) { r.error = "Card missing callback/k1"; return r; }

    // 3) Submit the invoice to the callback (bolt11 + k1 are URL-safe).
    String cbUrl = callback + (callback.indexOf('?') >= 0 ? "&" : "?") +
                   "k1=" + k1 + "&pr=" + bolt11;
    Serial.println("[BOLT] Submitting invoice to callback");

    resp = httpsGet(cbUrl, code);
    if (resp.isEmpty()) {
        Serial.printf("[BOLT] callback failed code=%d\n", code);
        r.error = "Card callback unreachable";
        return r;
    }

    JsonDocument cbDoc;
    if (deserializeJson(cbDoc, resp)) { r.error = "Bad callback response"; return r; }

    if (String(cbDoc["status"] | "") != "OK") {
        r.error = cbDoc["reason"] | "Card payment declined";
        Serial.printf("[BOLT] callback declined: %s\n", r.error.c_str());
        return r;
    }

    Serial.println("[BOLT] Accepted — wallet will settle the invoice");
    r.ok = true;
    return r;
}
