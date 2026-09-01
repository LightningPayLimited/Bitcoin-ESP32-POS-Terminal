#include "setup_portal.h"
#include "config.h"
#include "fw_portal.h"
#include "display_ui.h"
#include "printer.h"
#include "nfc.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_random.h>

extern DisplayUI ui;

// ================================================================
// AP SSID — base name plus a random 4-char uppercase-hex suffix so
// several devices being set up simultaneously advertise distinct networks.
// ================================================================
String SetupPortal::apSSID() {
    static String ssid;
    if (ssid.length() == 0) {
        static const char hex[] = "0123456789ABCDEF";
        ssid = SETUP_AP_SSID "-";
        for (int i = 0; i < 4; i++) {
            ssid += hex[esp_random() & 0xF];
        }
    }
    return ssid;
}

// ================================================================
// Captive portal HTML — self-contained setup form
// ================================================================
static const char SETUP_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stacked Bitcoin Setup</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, sans-serif; background: #111; color: #fff;
         display: flex; justify-content: center; padding: 20px; }
  .card { background: #1a1a1a; border-radius: 16px; padding: 32px;
          max-width: 400px; width: 100%; }
  h1 { color: #14afac; font-size: 24px; margin-bottom: 4px; }
  .sub { color: #888; font-size: 14px; margin-bottom: 24px; }
  label { display: block; color: #ccc; font-size: 14px; margin-bottom: 6px; margin-top: 16px; }
  input, select { width: 100%; padding: 12px; border-radius: 8px; border: 1px solid #333;
          background: #222; color: #fff; font-size: 16px; appearance: none; }
  input:focus, select:focus { border-color: #14afac; outline: none; }
  .hint { color: #666; font-size: 12px; margin-top: 4px; }
  button { width: 100%; padding: 14px; border-radius: 8px; border: none;
           background: #14afac; color: #000; font-size: 18px; font-weight: 700;
           cursor: pointer; margin-top: 24px; }
  button:hover { background: #d97e00; }
  .row { display: flex; gap: 8px; align-items: center; }
  .row select { flex: 1; }
  .iconbtn { width: auto; padding: 12px 14px; background: #333; color: #14afac;
             border-radius: 8px; border: 1px solid #444; font-size: 16px; cursor: pointer;
             margin: 0; }
  .iconbtn:hover { background: #3a3a3a; }
  .linkbtn { background: none; color: #14afac; border: none; padding: 6px 0;
             font-size: 13px; cursor: pointer; text-align: left; width: auto; margin-top: 4px; }
  .hidden { display: none; }
  .rssi { color: #888; font-size: 12px; margin-left: 4px; }
</style>
</head>
<body>
<div class="card">
  <h1>&#x26A1; Stacked Bitcoin</h1>
  <p class="sub">Configure your Bitcoin point of sale terminal</p>

  <form method="POST" action="/save" id="f">
    <label>Payment Provider</label>
    <select name="provider" id="provider">
      <option value="stacked" selected>Stacked</option>
      <option value="btcpay">BTCPayServer</option>
      <option value="lnaddress">Self-custody (Lightning Address)</option>
    </select>

    <label>WiFi Network</label>
    <div class="row">
      <select name="ssid" id="ssid"><option value="">Scanning…</option></select>
      <button type="button" class="iconbtn" id="rescan" title="Rescan">&#x21BB;</button>
    </div>
    <button type="button" class="linkbtn" id="togglemanual">Enter SSID manually &rarr;</button>
    <input id="manualssid" name="ssidManual" class="hidden" placeholder="Hidden or custom SSID">

    <label>WiFi Password</label>
    <input name="pass" type="text" placeholder="WiFi password"
           autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false">

    <div id="stackedFields">
      <label>Stacked API Key</label>
      <input name="apiKey" id="apiKey" placeholder="Paste your merchant API key">
      <p class="hint">Find this in your Stacked merchant dashboard</p>
    </div>

    <div id="btcpayFields" class="hidden">
      <label>BTCPay Server URL</label>
      <input name="btcpayUrl" id="btcpayUrl" placeholder="https://btcpay.example.com">
      <label>API Key</label>
      <input name="btcpayKey" id="btcpayKey" placeholder="Greenfield API key">
      <p class="hint">Needs create-invoice &amp; view-invoice permissions</p>
      <label>Currency</label>
      <select name="currency" id="currency">
        <option value="NZD" selected>NZD</option>
        <option value="AUD">AUD</option>
        <option value="USD">USD</option>
        <option value="EUR">EUR</option>
        <option value="GBP">GBP</option>
        <option value="CAD">CAD</option>
        <option value="JPY">JPY</option>
        <option value="SATS">SATS</option>
      </select>
      <p class="hint">You'll pick the store on this device after it restarts</p>
    </div>

    <div id="lnaddrFields" class="hidden">
      <label>Lightning Address</label>
      <input name="lnAddress" id="lnAddress" placeholder="you@walletprovider.com"
             inputmode="email" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false">
      <p class="hint">Payments go straight to your wallet. The wallet must support
        LNURL-pay verify (LUD-21) &mdash; e.g. Alby, Coinos, Stacked, LNbits.</p>
      <label>Currency</label>
      <select name="lnCurrency" id="lnCurrency">
        <option value="NZD" selected>NZD</option>
        <option value="AUD">AUD</option>
        <option value="USD">USD</option>
        <option value="EUR">EUR</option>
        <option value="GBP">GBP</option>
        <option value="CAD">CAD</option>
        <option value="JPY">JPY</option>
        <option value="SATS">SATS (no conversion)</option>
      </select>
      <label>Store name <span style="color:#666">(optional)</span></label>
      <input name="storeName" id="storeName" placeholder="Shown on receipts">
    </div>

    <button type="submit">Save &amp; Connect</button>
  </form>
</div>

<script>
const sel = document.getElementById('ssid');
const manual = document.getElementById('manualssid');
const toggle = document.getElementById('togglemanual');
const rescan = document.getElementById('rescan');
const provider = document.getElementById('provider');
const stackedFields = document.getElementById('stackedFields');
const btcpayFields = document.getElementById('btcpayFields');
const lnaddrFields = document.getElementById('lnaddrFields');

function updateProvider() {
  const p = provider.value;
  stackedFields.classList.toggle('hidden', p !== 'stacked');
  btcpayFields.classList.toggle('hidden', p !== 'btcpay');
  lnaddrFields.classList.toggle('hidden', p !== 'lnaddress');
}

// user@domain, https://…, lnurlp://…, or a bech32 lnurl1… string
function looksLikeLnAddress(v) {
  v = v.replace(/^lightning:/i, '');
  return /^[a-z0-9._+-]+@[a-z0-9-]+(\.[a-z0-9-]+)+(:\d+)?$/i.test(v) ||
         /^(https:\/\/|lnurlp:\/\/)\S+$/i.test(v) ||
         /^lnurl1[a-z0-9]+$/i.test(v);
}
provider.addEventListener('change', updateProvider);
updateProvider();

function scan() {
  sel.innerHTML = '<option value="">Scanning…</option>';
  fetch('/scan').then(r => r.json()).then(list => {
    if (!list.length) {
      sel.innerHTML = '<option value="">No networks found</option>';
      return;
    }
    sel.innerHTML = list.map(n => {
      const lock = n.auth ? ' \uD83D\uDD12' : '';
      const bars = n.rssi > -55 ? '\u25CF\u25CF\u25CF' : n.rssi > -70 ? '\u25CF\u25CF\u25CB' : '\u25CF\u25CB\u25CB';
      return '<option value="' + n.ssid.replace(/"/g,'&quot;') + '">' + n.ssid + lock + ' ' + bars + '</option>';
    }).join('');
    applySsidPrefill();
  }).catch(() => {
    sel.innerHTML = '<option value="">Scan failed</option>';
  });
}

function setManual(on) {
  manual.classList.toggle('hidden', !on);
  sel.disabled = on;
  toggle.textContent = on ? '← Pick from list' : 'Enter SSID manually →';
}
toggle.addEventListener('click', () => {
  const manualOn = manual.classList.contains('hidden');
  setManual(manualOn);
  if (manualOn) manual.focus();
});

// Re-entering setup (tap on a boot error, or a failed WiFi/wallet check):
// the device hands back what it had — everything except passwords/keys —
// so only the wrong field needs retyping.
let prefillSsid = '';
function applySsidPrefill() {
  if (!prefillSsid) return;
  // Never overwrite something the merchant typed themselves.
  const typed = manual.value && manual.value !== prefillSsid;
  if ([...sel.options].some(o => o.value === prefillSsid)) {
    sel.value = prefillSsid;
    if (!typed) { manual.value = ''; setManual(false); }   // it's in the list after all
  } else if (!typed) {
    manual.value = prefillSsid;
    setManual(true);
  }
  // Once applied against a real scan list (placeholders have value ""),
  // later rescans leave the SSID controls alone.
  if (sel.options.length && sel.options[0].value) prefillSsid = '';
}
fetch('/api/prefill').then(r => r.json()).then(c => {
  if (c.provider) { provider.value = c.provider; updateProvider(); }
  if (c.lnAddress) document.getElementById('lnAddress').value = c.lnAddress;
  if (c.storeName) document.getElementById('storeName').value = c.storeName;
  if (c.btcpayUrl) document.getElementById('btcpayUrl').value = c.btcpayUrl;
  if (c.currency) {
    // A code saved via serial/JSON may not be in the list — add it so the
    // re-save doesn't silently fall back to NZD.
    for (const id of ['lnCurrency', 'currency']) {
      const s = document.getElementById(id);
      if (![...s.options].some(o => o.value === c.currency)) s.add(new Option(c.currency, c.currency));
      s.value = c.currency;
    }
  }
  prefillSsid = c.ssid || '';
  applySsidPrefill();
}).catch(() => {});

rescan.addEventListener('click', scan);

document.getElementById('f').addEventListener('submit', e => {
  if (!manual.classList.contains('hidden') && manual.value) {
    sel.value = '';
    sel.name = '';
    manual.name = 'ssid';
  }
  if (provider.value === 'btcpay') {
    if (!document.getElementById('btcpayUrl').value ||
        !document.getElementById('btcpayKey').value) {
      e.preventDefault();
      alert('BTCPay needs a server URL and API key');
    }
  } else if (provider.value === 'lnaddress') {
    const a = document.getElementById('lnAddress').value.trim();
    if (!looksLikeLnAddress(a)) {
      e.preventDefault();
      alert('Enter a Lightning Address like you@walletprovider.com');
    }
  } else if (!document.getElementById('apiKey').value) {
    e.preventDefault();
    alert('A Stacked API key is required');
  }
});

scan();
</script>
</body>
</html>
)rawhtml";

static const char SETUP_OK_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stacked Bitcoin</title>
<style>
  body { font-family: -apple-system, sans-serif; background: #111; color: #fff;
         display: flex; justify-content: center; align-items: center;
         min-height: 100vh; text-align: center; }
  h1 { color: #0f0; font-size: 32px; }
  p { color: #888; margin-top: 12px; }
</style>
</head>
<body>
<div>
  <h1>&#x2705; Saved!</h1>
  <p>POS terminal is restarting...<br>You can close this page.</p>
</div>
</body>
</html>
)rawhtml";

// ================================================================
// Lightning Address sanity check (syntax only — the device is in AP mode
// during setup, so the address is resolved for real at the next boot).
// Accepts user@domain (LUD-16), https:// or lnurlp:// URLs (LUD-17) and
// bech32 lnurl1… strings (LUD-01).
// ================================================================
static bool looksLikeLnAddress(const String& in) {
    String a = in;
    a.trim();
    if (a.isEmpty() || a.indexOf(' ') >= 0) return false;
    String l = a;
    l.toLowerCase();
    // Pasted-from-wallet "lightning:" URI prefix — LnAddressAPI::begin()
    // strips it too, so accept it on every form (matches the portal JS).
    if (l.startsWith("lightning:")) {
        a = a.substring(10); l = l.substring(10);
        a.trim(); l.trim();
        if (a.isEmpty()) return false;
    }
    if (l.startsWith("https://") || l.startsWith("lnurlp://")) return a.length() > 10;
    if (l.startsWith("lnurl1")) return a.length() > 10;
    // user@domain — same charset the provider enforces (LUD-16), so a
    // value accepted here can't fail the local check after reboot.
    int at = l.indexOf('@');
    if (at <= 0 || at == (int)l.length() - 1) return false;
    if (l.indexOf('@', at + 1) >= 0) return false;
    for (int i = 0; i < at; i++) {
        char c = l[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == '+')) return false;
    }
    String dom = l.substring(at + 1);
    for (size_t i = 0; i < dom.length(); i++) {
        char c = dom[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.' || c == ':')) return false;
    }
    int dot = dom.indexOf('.');
    return dot > 0 && dot < (int)dom.length() - 1;
}

// ================================================================
// Captive Portal
// ================================================================

void SetupPortal::runCaptivePortal(ConfigStore& store) {
    Serial.println("[SETUP] Starting captive portal...");

    // AP + STA so we can scan networks while the AP serves the portal
    WiFi.mode(WIFI_AP_STA);
    String ssid = apSSID();
    WiFi.softAP(ssid.c_str(), SETUP_AP_PASS);
    delay(500);
    Serial.printf("[SETUP] AP started: %s @ %s\n",
                  ssid.c_str(), WiFi.softAPIP().toString().c_str());

    // Kick off an async scan so the dropdown is fast on first load
    WiFi.scanNetworks(true /* async */, true /* show hidden */);

    // DNS server — redirect all domains to our IP (captive portal)
    DNSServer dns;
    dns.start(53, "*", WiFi.softAPIP());

    // Web server
    WebServer server(80);

    bool saved = false;

    // Serve setup form on any path (captive portal)
    server.onNotFound([&server]() {
        server.send(200, "text/html", SETUP_HTML);
    });

    server.on("/", HTTP_GET, [&server]() {
        server.send(200, "text/html", SETUP_HTML);
    });

    // WiFi scan endpoint — returns JSON array of nearby networks
    server.on("/scan", HTTP_GET, [&server]() {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            // Already scanning — wait briefly then report what we have
            for (int i = 0; i < 30 && n == WIFI_SCAN_RUNNING; i++) {
                delay(100);
                n = WiFi.scanComplete();
            }
        }
        if (n < 0) {
            // No scan in progress or failed — kick one off synchronously
            n = WiFi.scanNetworks(false /* sync */, true /* show hidden */);
        }

        String out = "[";
        // Deduplicate by SSID, keep strongest signal
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;  // skip hidden

            // Skip dupes we've already emitted
            bool dup = false;
            for (int j = 0; j < i; j++) {
                if (WiFi.SSID(j) == ssid && WiFi.RSSI(j) >= WiFi.RSSI(i)) { dup = true; break; }
            }
            if (dup) continue;

            if (out.length() > 1) out += ",";
            String esc = ssid;
            esc.replace("\\", "\\\\");
            esc.replace("\"", "\\\"");
            out += "{\"ssid\":\"" + esc + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                   ",\"auth\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
        }
        out += "]";
        server.send(200, "application/json", out);

        // Kick off a fresh async scan for next time
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true);
    });

    // Handle form submission
    server.on("/save", HTTP_POST, [&server, &store, &saved]() {
        String ssid     = server.arg("ssid");
        String pass     = server.arg("pass");
        String provider = server.arg("provider");

        if (ssid.isEmpty()) {
            server.send(400, "text/plain", "WiFi SSID is required");
            return;
        }

        if (provider == "btcpay") {
            String url = server.arg("btcpayUrl");
            String key = server.arg("btcpayKey");
            String cur = server.arg("currency");
            if (url.isEmpty() || key.isEmpty()) {
                server.send(400, "text/plain",
                            "BTCPay URL and API key are required");
                return;
            }
            // Store ID is resolved on-device after reboot.
            store.saveBTCPay(ssid, pass, url, key, "", cur);
        } else if (provider == "lnaddress") {
            String addr = server.arg("lnAddress");
            addr.trim();
            String cur  = server.arg("lnCurrency");
            String name = server.arg("storeName");
            name.trim();
            if (!looksLikeLnAddress(addr)) {
                server.send(400, "text/plain",
                            "A Lightning Address (you@walletprovider.com) is required");
                return;
            }
            store.saveLnAddress(ssid, pass, addr, cur, name);
        } else {
            String apiKey = server.arg("apiKey");
            if (apiKey.isEmpty()) {
                server.send(400, "text/plain", "Stacked API key is required");
                return;
            }
            store.saveStacked(ssid, pass, apiKey);
        }

        server.send(200, "text/html", SETUP_OK_HTML);
        saved = true;
    });

    // Also accept JSON POST (for programmatic setup)
    server.on("/api/config", HTTP_POST, [&server, &store, &saved]() {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "application/json", "{\"error\":\"invalid json\"}");
            return;
        }

        String ssid     = doc["ssid"] | "";
        String pass     = doc["pass"] | "";
        String provider = doc["provider"] | "stacked";

        if (ssid.isEmpty()) {
            server.send(400, "application/json", "{\"error\":\"ssid required\"}");
            return;
        }

        if (provider == "btcpay") {
            String url   = doc["btcpayUrl"] | "";
            String key   = doc["apiKey"] | doc["btcpayKey"] | "";
            String stId  = doc["storeId"] | "";  // optional; resolved on-device
            String cur   = doc["currency"] | "";
            if (url.isEmpty() || key.isEmpty()) {
                server.send(400, "application/json",
                            "{\"error\":\"btcpayUrl and apiKey required\"}");
                return;
            }
            store.saveBTCPay(ssid, pass, url, key, stId, cur);
        } else if (provider == "lnaddress") {
            String addr = doc["lnAddress"] | "";
            addr.trim();
            String cur  = doc["currency"] | "";
            String name = doc["storeName"] | "";
            name.trim();
            if (!looksLikeLnAddress(addr)) {
                server.send(400, "application/json",
                            "{\"error\":\"valid lnAddress required\"}");
                return;
            }
            store.saveLnAddress(ssid, pass, addr, cur, name);
        } else {
            String apiKey = doc["apiKey"] | "";
            if (apiKey.isEmpty()) {
                server.send(400, "application/json", "{\"error\":\"apiKey required\"}");
                return;
            }
            store.saveStacked(ssid, pass, apiKey);
        }

        server.send(200, "application/json", "{\"ok\":true}");
        saved = true;
    });

    // Saved, non-secret settings for the form to pre-fill after a
    // "re-enter setup" (passwords / API keys are never served — the setup
    // AP is open).
    server.on("/api/prefill", HTTP_GET, [&server, &store]() {
        JsonDocument doc;
        doc["ssid"]      = store.ssid();
        doc["provider"]  = ConfigStore::providerName(store.provider());
        doc["currency"]  = store.currency();
        doc["btcpayUrl"] = store.btcpayUrl();
        doc["lnAddress"] = store.lnAddress();
        doc["storeName"] = store.storeName();
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    // Firmware update portal — also reachable during setup at
    // http://192.168.4.1/update (explicit routes win over onNotFound).
    FirmwarePortal::attach(server);

    server.begin();

    // Also listen on serial while portal is running
    Serial.println("[SETUP] Waiting for config via web form or serial...");
    Serial.println("[SETUP] Serial format: {\"ssid\":\"...\",\"pass\":\"...\",\"apiKey\":\"...\"}");

    while (!saved) {
        dns.processNextRequest();
        server.handleClient();

        // Check serial too
        if (checkSerial(store)) {
            saved = true;
        }

        // NFC card test — tap a card during setup to verify the reader.
        // Logs to serial AND shows the UID on-screen so bench NFC diagnostics
        // need no serial monitor.
        {
            String uid, url;
            if (nfc.readCard(uid, url)) {
                static int nfcTapCount = 0;
                nfcTapCount++;
                Serial.printf("[NFC] tap uid=%s url=%s\n",
                              uid.c_str(),
                              url.length() ? url.c_str() : "(no NDEF URL)");
                ui.showNfcTap(uid, url, nfcTapCount);
            }
        }

        // On-device Test Print button — fires a fake-receipt print so the
        // merchant can verify the printer before completing setup.
        if (ui.pollTouch() == Key::TEST_PRINT) {
            Serial.println("[SETUP] Test print requested (touch)");
            printer.printReceipt(
                "STACKED BITCOIN (TEST)",
                "123-456-789",
                "NZD",
                12.34f,
                21000,
                "TEST-RECEIPT",
                "2026-05-01 12:34:56"
            );
        }

        delay(10);
    }

    // Config saved — reboot
    Serial.println("[SETUP] Config saved! Rebooting in 2s...");
    delay(2000);
    ESP.restart();
}

// ================================================================
// Serial config receiver
// Accepts JSON on serial:
//   Stacked  : {"ssid":"...","pass":"...","apiKey":"..."}
//   BTCPay   : {"ssid":"...","pass":"...","provider":"btcpay",
//               "btcpayUrl":"...","apiKey":"...","currency":"..."}
//   LnAddress: {"ssid":"...","pass":"...","provider":"lnaddress",
//               "lnAddress":"you@wallet.com","currency":"NZD",
//               "storeName":"..."}
// Also accepts "RESET" (factory reset) and "SCAN" (list WiFi
// networks — replies with SCAN_RESULT:[{ssid,rssi,auth},...])
// ================================================================

bool SetupPortal::checkSerial(ConfigStore& store) {
    if (!Serial.available()) return false;

    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.isEmpty()) return false;

    // Factory reset command
    if (line.equalsIgnoreCase("RESET")) {
        store.clear();
        Serial.println("[SERIAL] Factory reset. Rebooting...");
        delay(1000);
        ESP.restart();
        return false;
    }

    // WiFi scan command — used by the USB setup page to populate its
    // network dropdown. The captive portal keeps an async scan running
    // (started at portal boot and after each /scan), and scanNetworks()
    // returns WIFI_SCAN_RUNNING instead of scanning while one is in
    // flight — so reuse completed results / wait for the running scan,
    // and only fall back to a fresh sync scan (~2-3s) when idle.
    // scanNetworks enables STA itself so this works in any WiFi mode.
    if (line.equalsIgnoreCase("SCAN")) {
        Serial.println("[SERIAL] Scanning WiFi networks...");
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            // Async scan in flight — wait for it (up to 10s)
            for (int i = 0; i < 100 && n == WIFI_SCAN_RUNNING; i++) {
                delay(100);
                n = WiFi.scanComplete();
            }
        }
        if (n < 0) {
            // No results and nothing running — do a fresh sync scan.
            // scanNetworks() refuses (returns RUNNING) if a scan raced in
            // meanwhile, so wait that one out too instead of giving up.
            n = WiFi.scanNetworks(false /* sync */, true /* show hidden */);
            for (int i = 0; i < 100 && n == WIFI_SCAN_RUNNING; i++) {
                delay(100);
                n = WiFi.scanComplete();
            }
        }
        // Serialize with ArduinoJson — SSIDs are arbitrary bytes and can
        // contain control chars that hand-rolled escaping would let poison
        // JSON.parse on the page.
        JsonDocument scanDoc;
        JsonArray nets = scanDoc.to<JsonArray>();
        // Deduplicate by SSID, keep strongest signal (same as /scan)
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;  // skip hidden

            bool dup = false;
            for (int j = 0; j < i; j++) {
                if (WiFi.SSID(j) == ssid && WiFi.RSSI(j) >= WiFi.RSSI(i)) { dup = true; break; }
            }
            if (dup) continue;

            JsonObject net = nets.add<JsonObject>();
            net["ssid"] = ssid;
            net["rssi"] = WiFi.RSSI(i);
            net["auth"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        String out;
        serializeJson(nets, out);
        Serial.print("SCAN_RESULT:");
        Serial.println(out);
        Serial.flush();
        WiFi.scanDelete();
        return false;
    }

    // Try to parse as JSON config
    JsonDocument doc;
    if (deserializeJson(doc, line)) {
        // Not JSON — ignore
        return false;
    }

    String ssid     = doc["ssid"] | "";
    String pass     = doc["pass"] | "";
    String provider = doc["provider"] | "stacked";

    if (ssid.isEmpty()) {
        Serial.println("[SERIAL] Need ssid");
        return false;
    }

    if (provider == "btcpay") {
        String url  = doc["btcpayUrl"] | "";
        String key  = doc["apiKey"] | doc["btcpayKey"] | "";
        String stId = doc["storeId"] | "";  // optional; resolved on-device
        String cur  = doc["currency"] | "";
        if (url.isEmpty() || key.isEmpty()) {
            Serial.println("[SERIAL] Need btcpayUrl and apiKey");
            return false;
        }
        store.saveBTCPay(ssid, pass, url, key, stId, cur);
    } else if (provider == "lnaddress") {
        String addr = doc["lnAddress"] | "";
        addr.trim();
        String cur  = doc["currency"] | "";
        String name = doc["storeName"] | "";
        name.trim();
        if (!looksLikeLnAddress(addr)) {
            Serial.println("[SERIAL] Need a valid lnAddress (you@wallet.com)");
            return false;
        }
        store.saveLnAddress(ssid, pass, addr, cur, name);
    } else {
        String apiKey = doc["apiKey"] | "";
        if (apiKey.isEmpty()) {
            Serial.println("[SERIAL] Need apiKey");
            return false;
        }
        store.saveStacked(ssid, pass, apiKey);
    }

    Serial.println("[SERIAL] Config saved!");
    return true;
}
