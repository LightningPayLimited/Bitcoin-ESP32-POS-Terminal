#include "lnaddress_api.h"
#include "config.h"
#include "bolt11.h"
#include "fiat_rate.h"
#include "tls_bundle.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include "mbedtls/sha256.h"

// Invoices remembered per sale: the live one plus older ones from the
// same sale (one per refresh).
#define LNADDR_MAX_SALE_INVOICES  (MAX_INVOICE_REFRESHES + 2)

// Shortest bolt11 validity the POS will put on screen.
#define LNADDR_MIN_EXPIRY_SEC     15

// ================================================================
// Helpers
// ================================================================

// RFC 3986 percent-encoding for a query-string value: everything outside
// the unreserved set is escaped, so '&', '=', '#', spaces and UTF-8 in a
// store name can't break the callback query.
static String urlEncode(const String& s) {
    static const char* HEXCHARS = "0123456789ABCDEF";
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += HEXCHARS[c >> 4];
            out += HEXCHARS[c & 15];
        }
    }
    return out;
}

// Truncate to at most `maxChars` bytes without splitting a UTF-8 sequence.
static String truncateUtf8(const String& s, int maxChars) {
    if ((int)s.length() <= maxChars) return s;
    int cut = maxChars;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    return s.substring(0, cut);
}

static bool isHttps(const String& url) {
    return url.startsWith("https://") && url.length() > 8;
}

static bool clockSynced() { return time(nullptr) >= 1700000000L; }

// Local wall-clock as "YYYY-MM-DD HH:MM:SS" for the receipt, or "" if the
// clock hasn't synced (tx_history.cpp uses the same threshold).
static String localTimeString() {
    if (!clockSynced()) return "";
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    return String(buf);
}

// ================================================================
// Setup / resolve
// ================================================================

void LnAddressAPI::begin(const String& address, const String& currency,
                         const String& storeName) {
    _address = address;
    _address.trim();
    // Pasted-from-wallet forms: "lightning:bob@x.com", "₿bob@x.com".
    {
        String lower = _address;
        lower.toLowerCase();
        if (lower.startsWith("lightning:")) _address = _address.substring(10);
        if (_address.startsWith("\xE2\x82\xBF")) _address = _address.substring(3);  // ₿
        _address.trim();
    }
    _currency = currency.length() ? currency : String(LNADDR_DEFAULT_CURRENCY);
    _currency.toUpperCase();
    _storeName = storeName;
    _resolved = false;
    _sale.clear();
}

// user@domain (LUD-16), https:// or lnurlp:// (LUD-17), lnurl1… (LUD-01)
bool LnAddressAPI::addressToUrl(String& urlOut, String& err) const {
    String a = _address;
    String lower = a;
    lower.toLowerCase();

    if (lower.startsWith("lnurl1")) {
        std::string url;
        if (!lnurlDecode(a.c_str(), url)) { err = "Invalid LNURL"; return false; }
        urlOut = url.c_str();
    } else if (lower.startsWith("lnurlp://")) {
        urlOut = "https://" + a.substring(9);
    } else if (lower.startsWith("https://")) {
        urlOut = "https://" + a.substring(8);   // normalise an upper-case scheme
    } else if (lower.startsWith("http://")) {
        err = "Address must be https";
        return false;
    } else {
        int at = a.indexOf('@');
        if (at <= 0 || at == (int)a.length() - 1 || a.indexOf('@', at + 1) >= 0) {
            err = "Invalid Lightning Address";
            return false;
        }
        // LUD-16: usernames are lowercase a-z0-9-_. (+ for tags); domains
        // are case-insensitive. Normalise both. The charset also keeps the
        // URL path well-formed without any encoding.
        String user = lower.substring(0, at);
        String dom  = lower.substring(at + 1);
        for (size_t i = 0; i < user.length(); i++) {
            char c = user[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '+';
            if (!ok) { err = "Invalid Lightning Address"; return false; }
        }
        for (size_t i = 0; i < dom.length(); i++) {
            char c = dom[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '.' || c == ':';
            if (!ok) { err = "Invalid Lightning Address"; return false; }
        }
        if (dom.indexOf('.') <= 0) { err = "Invalid Lightning Address"; return false; }
        urlOut = "https://" + dom + "/.well-known/lnurlp/" + user;
    }

    if (!isHttps(urlOut)) { err = "Address must be https"; return false; }
    return true;
}

bool LnAddressAPI::resolve(String& err) {
    _resolved = false;
    if (!addressToUrl(_lnurlpUrl, err)) return false;

    Serial.printf("[LNADDR] Resolving %s\n", _lnurlpUrl.c_str());
    int code;
    String resp = httpsGet(_lnurlpUrl, code, LNURL_HTTP_TIMEOUT_MS);
    if (resp.isEmpty()) {
        if (code == 404)     err = "Address not found (404)";
        else if (code > 0)   err = "Wallet error HTTP " + String(code);
        else                 err = "Wallet unreachable";
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { err = "Bad wallet response"; return false; }

    if (String(doc["status"] | "") == "ERROR") {
        err = doc["reason"] | "Wallet rejected address";
        return false;
    }
    if (String(doc["tag"] | "") != "payRequest") {
        err = "Not an LNURL-pay address";
        return false;
    }

    // Every URL the service hands back is untrusted input: TLS only.
    _callback = doc["callback"] | "";
    if (!isHttps(_callback)) { err = "Wallet callback not https"; return false; }

    _minMsat = doc["minSendable"] | (uint64_t)1000;
    _maxMsat = doc["maxSendable"] | (uint64_t)0;
    if (_maxMsat == 0) {
        Serial.println("[LNADDR] no maxSendable — treating as unlimited");
        _maxMsat = UINT64_MAX;
    }
    if (_minMsat == 0) _minMsat = 1;
    _commentAllowed = doc["commentAllowed"] | 0;

    // metadata is a JSON array serialised inside a string:
    //   "[[\"text/plain\",\"…\"],[\"text/identifier\",\"user@domain\"]]"
    _identifier = "";
    const char* meta = doc["metadata"] | "";
    if (*meta) {
        JsonDocument md;
        if (!deserializeJson(md, meta)) {
            for (JsonArray entry : md.as<JsonArray>()) {
                String kind = entry[0] | "";
                if (kind == "text/identifier" || kind == "text/email") {
                    _identifier = entry[1] | "";
                }
            }
        }
    }
    // The service says who it pays; if that isn't the configured address
    // (aliases and custom domains legitimately differ), say so on serial.
    if (_identifier.length() && _address.indexOf('@') > 0) {
        String a = _address, b = _identifier;
        a.toLowerCase(); b.toLowerCase();
        if (a != b) Serial.printf("[LNADDR] note: service identifies as %s\n", _identifier.c_str());
    }

    _resolved = true;
    Serial.printf("[LNADDR] OK callback=%s min=%llu max=%llu msat comment=%d id=%s\n",
                  _callback.c_str(), (unsigned long long)_minMsat,
                  (unsigned long long)_maxMsat, _commentAllowed, _identifier.c_str());
    return true;
}

LnAddressAPI::Probe LnAddressAPI::probeVerifySupport(String& err) {
    if (!_resolved && !resolve(err)) return Probe::ERROR;

    // Whole sats, at least 1 sat: a service may advertise minSendable=1 msat
    // yet refuse to issue sub-sat invoices.
    uint64_t msat = _minMsat < 1000 ? 1000 : ((_minMsat + 999) / 1000) * 1000;
    if (msat > _maxMsat) msat = _maxMsat;

    String pr, verify;
    if (!fetchCallback(msat, "", pr, verify, err)) return Probe::ERROR;

    if (verify.isEmpty()) {
        err = "Wallet has no LUD-21 verify";
        return Probe::UNSUPPORTED;
    }
    if (!isHttps(verify)) {
        err = "Wallet verify URL not https";
        return Probe::UNSUPPORTED;
    }
    Bolt11 inv;
    if (!bolt11Decode(pr.c_str(), inv)) {
        err = "Wallet sent a bad invoice";
        return Probe::ERROR;
    }
    if (inv.prefix != LNADDR_BOLT11_PREFIX) {
        err = "Wallet is not on mainnet";
        return Probe::UNSUPPORTED;
    }
    Serial.println("[LNADDR] LUD-21 verify supported");
    return Probe::OK;
}

// ================================================================
// Invoice creation
// ================================================================

bool LnAddressAPI::fetchCallback(uint64_t msat, const String& comment,
                                 String& pr, String& verify, String& err) {
    String url = _callback + (_callback.indexOf('?') >= 0 ? "&" : "?") +
                 "amount=" + String((unsigned long long)msat);
    if (_commentAllowed > 0 && comment.length() > 0) {
        url += "&comment=" + urlEncode(truncateUtf8(comment, _commentAllowed));
    }

    int code;
    String resp = httpsGet(url, code, LNURL_HTTP_TIMEOUT_MS);
    if (resp.isEmpty()) {
        err = (code > 0) ? "Wallet error HTTP " + String(code) : "Wallet unreachable";
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { err = "Bad wallet response"; return false; }
    if (String(doc["status"] | "") == "ERROR") {
        err = doc["reason"] | "Wallet refused invoice";
        return false;
    }
    pr     = doc["pr"] | "";
    verify = doc["verify"] | "";
    if (pr.isEmpty()) { err = "No invoice from wallet"; return false; }
    return true;
}

bool LnAddressAPI::fiatToMsat(float fiat, uint64_t& msat, String& err) {
    uint64_t sats;
    if (_currency == "SATS") {
        sats = (uint64_t)(fiat + 0.5f);
    } else {
        RateResult r = fetchBtcRate(_currency);
        if (!r.ok) { err = r.error; return false; }
        double s = (double)fiat / r.btcPrice * 1e8;
        sats = (uint64_t)(s + 0.5);
        Serial.printf("[LNADDR] %.2f %s @ %.2f = %llu sats%s\n", fiat,
                      _currency.c_str(), r.btcPrice, (unsigned long long)sats,
                      r.stale ? " (stale rate)" : "");
    }
    if (sats == 0) { err = "Amount too small"; return false; }
    msat = sats * 1000ULL;
    if (msat < _minMsat) {
        err = "Below wallet minimum (" +
              String((unsigned long)((_minMsat + 999) / 1000)) + " sats)";
        return false;
    }
    if (msat > _maxMsat) {
        err = "Above wallet maximum";
        return false;
    }
    return true;
}

MerchantInvoice LnAddressAPI::asMerchantInvoice(const SaleInvoice& rec) const {
    MerchantInvoice inv = {};
    inv.ok             = true;
    inv.paymentRequest = rec.pr;
    inv.reference      = rec.ref;
    inv.satAmount      = rec.sats;
    inv.nzdAmount      = rec.fiat;

    // Countdown: the bolt11's remaining validity, capped to the provider's
    // cadence (rate lock) and floored so the state machine never spins.
    long remaining = (long)(rec.validUntilMs - millis()) / 1000;
    int exp = (int)remaining;
    if (exp > LNADDR_INVOICE_EXPIRY_SEC) exp = LNADDR_INVOICE_EXPIRY_SEC;
    if (exp < LNADDR_MIN_EXPIRY_SEC)     exp = LNADDR_MIN_EXPIRY_SEC;
    inv.expirySec = exp;
    return inv;
}

MerchantInvoice LnAddressAPI::requestInvoice(float fiat, const String& details) {
    MerchantInvoice inv = {};

    String err;
    if (!_resolved && !resolve(err)) { inv.error = err; return inv; }

    uint64_t msat;
    if (!fiatToMsat(fiat, msat, err)) { inv.error = err; return inv; }

    String pr, verify;
    if (!fetchCallback(msat, details, pr, verify, err)) {
        // The cached payRequest may have gone stale (providers rotate
        // callback URLs). Re-resolve once and retry before giving up.
        String rerr;
        Serial.printf("[LNADDR] callback failed (%s) — re-resolving\n", err.c_str());
        if (!resolve(rerr) || !fetchCallback(msat, details, pr, verify, err)) {
            inv.error = err;
            return inv;
        }
    }

    // A wallet without verify can't be polled — refuse rather than show a
    // QR we could never confirm (probeVerifySupport catches this at setup).
    if (!isHttps(verify)) { inv.error = "Wallet has no LUD-21 verify"; return inv; }

    // LUD-06 step 7: the invoice must be for exactly what we asked, on
    // mainnet, and decodable (the payment hash gates PAID later).
    Bolt11 b;
    std::string berr;
    if (!bolt11Decode(pr.c_str(), b, &berr)) {
        Serial.printf("[LNADDR] bad bolt11: %s\n", berr.c_str());
        inv.error = "Wallet sent a bad invoice";
        return inv;
    }
    if (b.prefix != LNADDR_BOLT11_PREFIX) {
        inv.error = "Invoice is not mainnet";
        return inv;
    }
    if (!b.hasAmount || b.amountMsat != msat) {
        Serial.printf("[LNADDR] amount mismatch: asked %llu got %llu msat\n",
                      (unsigned long long)msat, (unsigned long long)b.amountMsat);
        inv.error = "Invoice amount mismatch";
        return inv;
    }
    if (pr.length() > QR_MAX_CHARS) {
        inv.error = "Invoice too long for QR";
        return inv;
    }

    // Validity: x counts from the invoice's own timestamp. Use the wall
    // clock when synced; otherwise assume it was minted just now.
    long remaining = (long)b.expirySec;
    if (clockSynced()) {
        remaining = (long)((long long)b.timestamp + b.expirySec - (long long)time(nullptr));
    }
    if (remaining < LNADDR_MIN_EXPIRY_SEC) {
        Serial.printf("[LNADDR] invoice expires in %lds — rejecting\n", remaining);
        inv.error = "Invoice expires too soon";
        return inv;
    }
    // Some wallets mint month-long invoices; keep the millis() deadline
    // far from the 32-bit wrap (49.7 days) — a sale never lasts that long.
    if (remaining > 7L * 86400L) remaining = 7L * 86400L;

    SaleInvoice rec;
    rec.ref = bytesToHex(b.paymentHash, 8).c_str();   // 16 hex chars
    rec.pr  = pr;
    rec.pr.toLowerCase();
    rec.verifyUrl = verify;
    memcpy(rec.payHash, b.paymentHash, 32);
    rec.sats = msat / 1000ULL;
    rec.fiat = fiat;
    rec.validUntilMs = millis() + (unsigned long)remaining * 1000UL;
    if (_sale.size() >= LNADDR_MAX_SALE_INVOICES) _sale.erase(_sale.begin());
    _sale.push_back(rec);

    _lastFiat    = fiat;
    _lastDetails = details;

    inv = asMerchantInvoice(rec);
    Serial.printf("[LNADDR] Invoice %llu sats ref=%s valid %lds (shown %ds) verify=%s\n",
                  (unsigned long long)rec.sats, rec.ref.c_str(), remaining,
                  inv.expirySec, verify.c_str());
    return inv;
}

MerchantInvoice LnAddressAPI::createInvoice(float amount, const String& details,
                                            const String& txlink) {
    (void)txlink;
    Serial.printf("[LNADDR] Creating invoice %.2f %s\n", amount, _currency.c_str());
    return requestInvoice(amount, details);
}

MerchantInvoice LnAddressAPI::refreshInvoice(const String& reference) {
    Serial.println("[LNADDR] Refreshing — new invoice at same amount");
    MerchantInvoice fresh = requestInvoice(_lastFiat, _lastDetails);
    if (fresh.ok) return fresh;

    // Couldn't mint a replacement (rate API / wallet hiccup). If the live
    // invoice is still good at the wallet, keep showing it rather than
    // aborting a sale the customer may be paying right now.
    const SaleInvoice* cur = findRef(reference);
    if (cur && (long)(cur->validUntilMs - millis()) / 1000 >= LNADDR_MIN_EXPIRY_SEC) {
        Serial.printf("[LNADDR] refresh failed (%s) — keeping current invoice\n",
                      fresh.error.c_str());
        return asMerchantInvoice(*cur);
    }
    return fresh;
}

// ================================================================
// Settlement
// ================================================================

const LnAddressAPI::SaleInvoice* LnAddressAPI::findRef(const String& ref) const {
    for (const SaleInvoice& s : _sale) if (s.ref == ref) return &s;
    return nullptr;
}

// GET verify -> settled? LUD-21 makes the preimage non-null exactly when
// settled, so "settled" only counts with a preimage that hashes to the
// invoice's payment hash; an echoed `pr` must be our invoice.
bool LnAddressAPI::pollVerify(const SaleInvoice& inv, bool& settled, String& err) {
    settled = false;
    int code;
    String resp = httpsGet(inv.verifyUrl, code, LNADDR_POLL_TIMEOUT_MS);
    if (resp.isEmpty()) { err = "No response"; return false; }

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { err = "JSON parse error"; return false; }
    if (String(doc["status"] | "") == "ERROR") {
        err = doc["reason"] | "verify error";
        return false;
    }

    String echoed = doc["pr"] | "";
    if (echoed.length()) {
        echoed.toLowerCase();
        if (echoed != inv.pr) {
            Serial.println("[LNADDR] verify echoed a different invoice — ignoring");
            err = "Invoice mismatch";
            return false;
        }
    }

    if (!(doc["settled"] | false)) return true;

    const char* preimage = doc["preimage"] | "";
    uint8_t pre[32], hash[32];
    if (!hexToBytes(preimage, pre, 32)) {
        Serial.println("[LNADDR] settled without a valid preimage — not accepting");
        err = "Settled but unproven";
        return false;
    }
    mbedtls_sha256(pre, 32, hash, 0);
    if (memcmp(hash, inv.payHash, 32) != 0) {
        Serial.println("[LNADDR] settled but preimage != payment hash — not accepting");
        err = "Preimage mismatch";
        return false;
    }
    settled = true;
    return true;
}

PaymentStatus LnAddressAPI::checkPayment(const String& reference) {
    PaymentStatus ps = {};
    const SaleInvoice* cur = findRef(reference);
    if (!cur) { ps.error = "Unknown reference"; return ps; }
    if (WiFi.status() != WL_CONNECTED) { ps.error = "No WiFi"; return ps; }

    // 1) The live invoice.
    bool settled = false;
    String err;
    bool ok = pollVerify(*cur, settled, err);
    const SaleInvoice* paid = (ok && settled) ? cur : nullptr;
    if (!ok) Serial.printf("[LNADDR] verify %s: %s\n", cur->ref.c_str(), err.c_str());

    // 2) One older invoice from this sale per poll, round-robin — the
    //    customer may have scanned the previous QR just before a refresh.
    //    A wallet that no longer knows an old invoice ("Not found") or one
    //    that has expired is dropped from the rotation. Skipped when the
    //    live poll already failed at the transport level, so a dead host
    //    costs one timeout per poll, not two.
    const bool hostDown = (!ok && err == "No response");
    if (!paid && !hostDown && _sale.size() > 1) {
        size_t n = _sale.size();
        for (size_t k = 0; k < n; k++) {
            size_t idx = (_rr + k) % n;
            const SaleInvoice& o = _sale[idx];
            if (&o == cur) continue;
            _rr = (idx + 1) % n;
            bool oSettled = false;
            String oerr;
            bool ook = pollVerify(o, oSettled, oerr);
            if (ook && oSettled) {
                Serial.printf("[LNADDR] older invoice %s settled\n", o.ref.c_str());
                paid = &o;
            } else if ((!ook && oerr.indexOf("ound") >= 0) ||          // "Not found"
                       (long)(o.validUntilMs - millis()) < 0) {
                Serial.printf("[LNADDR] dropping old invoice %s (%s)\n",
                              o.ref.c_str(), ook ? "expired" : oerr.c_str());
                _sale.erase(_sale.begin() + idx);
                cur = findRef(reference);       // vector moved
            }
            break;
        }
    }

    const SaleInvoice& rec = paid ? *paid : *cur;
    ps.ok        = ok || paid;
    ps.reference = rec.ref;
    ps.satAmount = rec.sats;
    ps.nzdAmount = rec.fiat;
    ps.isPaid    = paid != nullptr;
    ps.status    = ps.isPaid ? "settled" : "pending";
    if (ps.isPaid) ps.paidDate = localTimeString();
    if (!ps.ok) ps.error = err;
    return ps;
}

String LnAddressAPI::payeeLabel() const {
    if (_address.indexOf('@') > 0) return _address;
    if (_identifier.length())      return _identifier;
    int s = _lnurlpUrl.indexOf("://");
    if (s < 0) return _address;
    int e = _lnurlpUrl.indexOf('/', s + 3);
    return e < 0 ? _lnurlpUrl.substring(s + 3) : _lnurlpUrl.substring(s + 3, e);
}

MerchantProfile LnAddressAPI::getProfile() {
    MerchantProfile mp = {};
    mp.ok = true;
    mp.companyName = _storeName.length() ? _storeName : payeeLabel();
    return mp;
}

void LnAddressAPI::endSale() {
    _sale.clear();
    _rr = 0;
}

// ================================================================
// HTTP — TLS validated against the root-CA bundle. Redirects are followed
// (custom-domain addresses often 301 to the wallet provider); HTTPClient
// refuses a redirect that changes scheme, so https stays https.
// ================================================================

String LnAddressAPI::httpsGet(const String& url, int& codeOut, unsigned long timeoutMs) {
    if (WiFi.status() != WL_CONNECTED) { codeOut = -1001; return ""; }

    HTTPClient http;
    WiFiClientSecure client;
    useRootCaBundle(client);
    client.setTimeout(timeoutMs / 1000);
    client.setHandshakeTimeout(timeoutMs / 1000);

    if (!http.begin(client, url)) { codeOut = -1000; return ""; }
    http.addHeader("Accept", "application/json");
    http.setTimeout(timeoutMs);
    http.setConnectTimeout(timeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(3);

    unsigned long t0 = millis();
    codeOut = http.GET();
    String payload;
    if (codeOut > 0) {
        payload = http.getString();
        Serial.printf("[LNADDR HTTP] GET %dms code=%d (%d bytes): %s\n",
                      (int)(millis() - t0), codeOut, payload.length(),
                      payload.length() > 200
                          ? (payload.substring(0, 200) + "...").c_str()
                          : payload.c_str());
        // LNURL services put the real error in a JSON body even on 4xx/5xx
        // — keep it so callers can surface `reason`; drop non-JSON bodies
        // (an HTML 404 page) so they read as "no response".
        if (codeOut >= 400 && !payload.startsWith("{")) payload = "";
    } else {
        Serial.printf("[LNADDR HTTP] GET err %d (%s) %s\n", codeOut,
                      http.errorToString(codeOut).c_str(), url.c_str());
    }
    http.end();
    return payload;
}
