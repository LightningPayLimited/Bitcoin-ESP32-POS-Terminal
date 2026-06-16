#include "setup_portal.h"
#include "config.h"
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
// AP SSID — base name plus a random 4-char base64 suffix so several
// devices being set up simultaneously advertise distinct networks.
// ================================================================
String SetupPortal::apSSID() {
    static String ssid;
    if (ssid.length() == 0) {
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        ssid = SETUP_AP_SSID "-";
        for (int i = 0; i < 4; i++) {
            ssid += b64[esp_random() & 0x3F];
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
<title>Lightning Pay Setup</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, sans-serif; background: #111; color: #fff;
         display: flex; justify-content: center; padding: 20px; }
  .card { background: #1a1a1a; border-radius: 16px; padding: 32px;
          max-width: 400px; width: 100%; }
  h1 { color: #f7931a; font-size: 24px; margin-bottom: 4px; }
  .sub { color: #888; font-size: 14px; margin-bottom: 24px; }
  label { display: block; color: #ccc; font-size: 14px; margin-bottom: 6px; margin-top: 16px; }
  input, select { width: 100%; padding: 12px; border-radius: 8px; border: 1px solid #333;
          background: #222; color: #fff; font-size: 16px; appearance: none; }
  input:focus, select:focus { border-color: #f7931a; outline: none; }
  .hint { color: #666; font-size: 12px; margin-top: 4px; }
  button { width: 100%; padding: 14px; border-radius: 8px; border: none;
           background: #f7931a; color: #000; font-size: 18px; font-weight: 700;
           cursor: pointer; margin-top: 24px; }
  button:hover { background: #d97e00; }
  .row { display: flex; gap: 8px; align-items: center; }
  .row select { flex: 1; }
  .iconbtn { width: auto; padding: 12px 14px; background: #333; color: #f7931a;
             border-radius: 8px; border: 1px solid #444; font-size: 16px; cursor: pointer;
             margin: 0; }
  .iconbtn:hover { background: #3a3a3a; }
  .linkbtn { background: none; color: #f7931a; border: none; padding: 6px 0;
             font-size: 13px; cursor: pointer; text-align: left; width: auto; margin-top: 4px; }
  .hidden { display: none; }
  .rssi { color: #888; font-size: 12px; margin-left: 4px; }
</style>
</head>
<body>
<div class="card">
  <h1>&#x26A1; Lightning Pay</h1>
  <p class="sub">Configure your Bitcoin point of sale terminal</p>

  <form method="POST" action="/save" id="f">
    <label>Payment Provider</label>
    <select name="provider" id="provider">
      <option value="stacked" selected>Stacked</option>
      <option value="btcpay">BTCPayServer</option>
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

function updateProvider() {
  const bp = provider.value === 'btcpay';
  stackedFields.classList.toggle('hidden', bp);
  btcpayFields.classList.toggle('hidden', !bp);
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
  }).catch(() => {
    sel.innerHTML = '<option value="">Scan failed</option>';
  });
}

toggle.addEventListener('click', () => {
  const manualOn = manual.classList.toggle('hidden') === false;
  sel.disabled = manualOn;
  toggle.textContent = manualOn ? '← Pick from list' : 'Enter SSID manually →';
  if (manualOn) manual.focus();
});

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
<title>Lightning Pay</title>
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
                "LIGHTNING PAY (TEST)",
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
// Accepts JSON on serial: {"ssid":"...","pass":"...","apiKey":"..."}
// Also accepts "RESET" to factory reset
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
