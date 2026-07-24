// ============================================================
// Lightning Pay POS — ESP32-S3 — Stage C: invoice + QR + payment poll.
//   numpad -> create invoice -> show bolt11 QR -> poll status -> PAID.
//   Stacked + BTCPay providers (BTCPay store auto-selected on first boot).
//   (NFC / Boltcard tap-to-pay is Stage D.)
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "display_ui.h"
#include "config_store.h"
#include "setup_portal.h"
#include "stacked_api.h"
#include "btcpay_api.h"
#include "nfc.h"
#include "boltcard.h"

enum class State { IDLE, CREATING, AWAITING, PAID, ERROR };

static DisplayUI        ui;
static ConfigStore      config;
static SetupPortal      portal;
static StackedAPI       stackedApi;
static BTCPayAPI        btcpayApi;
static PaymentProvider* api = nullptr;

static String  currencyLabel = "NZD";
static String  merchantName = "";

static State   state = State::IDLE;
static String  amount = "";
static float   activeFiat = 0;
static MerchantInvoice activeInvoice;
static unsigned long invoiceCreatedAt = 0, lastPollAt = 0, stateEnteredAt = 0;
static int     refreshCount = 0;
static int     lastShownSecs = -1;
static bool    boltcardSubmitted = false;   // invoice handed to a Boltcard

static String invoiceDetails() {
    String amt = String(activeFiat, 2) + " " + currencyLabel;
    return merchantName.length() ? merchantName + " $" + amt : "$" + amt;
}

static void resetToIdle() {
    state = State::IDLE;
    amount = "";
    activeFiat = 0;
    refreshCount = 0;
    boltcardSubmitted = false;
    activeInvoice = {};
    ui.showAmountEntry(amount, currencyLabel);
}

// ---- numpad ----
static char keyDigit(Key k) {
    int idx = (int)k - (int)Key::D0;
    return (idx >= 0 && idx <= 9) ? ('0' + idx) : 0;
}

static void handleKey(Key k) {
    char d = keyDigit(k);
    if (d) {
        int dot = amount.indexOf('.');
        if (dot >= 0 && (int)amount.length() - dot > 2) return;
        if (amount.length() >= 9) return;
        amount += d;
    } else if (k == Key::DOT) {
        if (amount.indexOf('.') < 0) {
            if (amount.isEmpty()) amount = "0";
            amount += '.';
        }
    } else if (k == Key::DEL) {
        if (amount.length() > 0) amount.remove(amount.length() - 1);
    } else if (k == Key::CLEAR) {
        amount = "";
    } else if (k == Key::CHARGE) {
        int cents = (int)(amount.toFloat() * 100.0f + 0.5f);
        if (cents < 1) return;
        activeFiat = cents / 100.0f;
        state = State::CREATING;
        return;
    } else {
        return;
    }
    ui.updateAmount(amount, currencyLabel);
}

// ---- WiFi ----
static bool connectWiFi() {
    ui.showMessage("Connecting", config.ssid(), COL_ACCENT);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid().c_str(), config.pass().c_str());
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        portal.checkSerial(config);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] OK: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    return false;
}

// BTCPay: pick a store the first time (auto-select first; picker is a TODO).
static void resolveBTCPayStore() {
    while (true) {
        ui.showLoading("Finding store");
        auto stores = btcpayApi.listStores();
        if (!stores.empty()) {
            config.saveStoreId(stores[0].id);
            btcpayApi.setStore(stores[0].id);
            Serial.printf("[BOOT] BTCPay store: %s (%s)\n",
                          stores[0].name.c_str(), stores[0].id.c_str());
            if (stores.size() > 1)
                Serial.printf("[BOOT] %d stores; auto-picked first\n", (int)stores.size());
            return;
        }
        ui.showError("No BTCPay store");
        delay(4000);
    }
}

#if NFC_PIN_SWEEP
// ---- NFC pin-finder: bit-bang I2C address probe across safe candidate pins ----
static inline void swLow(int p) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
static inline void swRel(int p) { pinMode(p, INPUT_PULLUP); }
static inline void swSclHigh(int scl) {
    pinMode(scl, INPUT_PULLUP);
    uint32_t t0 = micros();
    while (digitalRead(scl) == 0 && micros() - t0 < 1000) { /* clock stretch */ }
}
static bool swProbe(int sda, int scl, uint8_t addr7) {
    const int H = 6;
    swRel(sda); swRel(scl); delayMicroseconds(H * 2);
    swLow(sda); delayMicroseconds(H);                 // START
    swLow(scl); delayMicroseconds(H);
    uint8_t b = (uint8_t)((addr7 << 1) | 0);
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) swRel(sda); else swLow(sda);
        delayMicroseconds(H);
        swSclHigh(scl); delayMicroseconds(H);
        swLow(scl);     delayMicroseconds(H);
        b <<= 1;
    }
    swRel(sda); delayMicroseconds(H);                 // ACK
    swSclHigh(scl); delayMicroseconds(H);
    bool ack = (digitalRead(sda) == 0);
    swLow(scl); delayMicroseconds(H);
    swLow(sda); delayMicroseconds(H);                 // STOP
    swSclHigh(scl); delayMicroseconds(H);
    swRel(sda); delayMicroseconds(H);
    swRel(sda); swRel(scl);
    return ack;
}
static void nfcPinSweep() {
    // Candidate header GPIOs (excludes display 10-13/46, touch 15-18, BL 45,
    // USB 19/20, UART 43/44, flash/PSRAM 26-37). Includes the SD-card pins as
    // a fallback in case the wiring landed there (remove any microSD first).
    static const int CAND[] = { 1, 2, 3, 4, 5, 7, 8, 9, 14, 21, 42,
                                38, 39, 40, 41, 47, 48 };
    const int N = sizeof(CAND) / sizeof(CAND[0]);

    // Pass 1: idle levels. A valid I2C line idles HIGH (internal pull-up); a
    // line stuck LOW is a wedged device or a short — flag it, and don't treat
    // it as SDA (a low SDA reads as a false ACK on every probe).
    Serial.println("[SWEEP] Idle levels (INPUT_PULLUP; LOW = stuck/suspicious):");
    bool high[64] = {false};
    for (int i = 0; i < N; i++) {
        pinMode(CAND[i], INPUT_PULLUP);
    }
    delay(5);
    for (int i = 0; i < N; i++) {
        int lvl = digitalRead(CAND[i]);
        high[CAND[i]] = (lvl == 1);
        if (lvl == 0) Serial.printf("[SWEEP]   GPIO%d = LOW  <-- stuck low\n", CAND[i]);
    }

    // Pass 2: probe only pairs whose SDA idles high.
    Serial.println("[SWEEP] Probing PN532 (0x24) on usable pairs...");
    for (int i = 0; i < N; i++) {
        if (!high[CAND[i]]) continue;                 // skip stuck-low SDA
        for (int j = 0; j < N; j++) {
            if (i == j) continue;
            int sda = CAND[i], scl = CAND[j];
            if (!swProbe(sda, scl, 0x24)) continue;
            Serial.printf("[SWEEP] ACK on SDA=GPIO%d SCL=GPIO%d -> confirming...\n", sda, scl);
            if (nfc.tryPins(sda, scl)) {
                Serial.printf("[SWEEP] >>> PN532 FOUND: SDA=GPIO%d  SCL=GPIO%d <<<\n", sda, scl);
                Serial.println("[SWEEP] Set NFC_SDA_PIN/NFC_SCL_PIN to these in board_config.h.");
                return;
            }
        }
    }
    for (int i = 0; i < N; i++) pinMode(CAND[i], INPUT);
    Serial.println("[SWEEP] No PN532 confirmed on any candidate pair.");
}
#endif

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[S3] Lightning Pay POS — payment stage");

    pinMode(BOOT_BTN, INPUT_PULLUP);   // WiFi-reset hold (see checkWifiResetButton)

    ui.begin();
    ui.showSplash();
    delay(1200);

#if NFC_PIN_SWEEP
    ui.showMessage("NFC pin sweep", "see serial", COL_ACCENT);
    nfcPinSweep();
    Serial.println("[SWEEP] Done — halting. Flash with NFC_PIN_SWEEP 0 to run the POS.");
    while (true) delay(1000);
#endif

    if (!config.begin()) {
        Serial.println("[BOOT] Not provisioned — captive portal");
        ui.showSetupInfo(SetupPortal::apSSID(), SETUP_AP_IP);
        portal.runCaptivePortal(config);   // blocks until saved, reboots
        return;
    }

    // Select backend
    if (config.provider() == Provider::BTCPAY) {
        btcpayApi.begin(config.btcpayUrl(), config.apiKey(),
                        config.storeId(), config.currency());
        api = &btcpayApi;
        currencyLabel = config.currency();
        Serial.printf("[BOOT] Provider: BTCPay (%s, %s)\n",
                      config.btcpayUrl().c_str(), currencyLabel.c_str());
    } else {
        stackedApi.begin(STACKED_API_BASE, config.apiKey());
        api = &stackedApi;
        currencyLabel = "NZD";
        Serial.println("[BOOT] Provider: Stacked");
    }

    while (!connectWiFi()) {
        ui.showError("WiFi failed - retry");
        delay(3000);
        WiFi.disconnect(true);
        delay(200);
    }

    // NFC reader (PN532 bit-bang on dedicated pins). Optional — taps just won't
    // register if it's absent; the QR path is unaffected.
    nfc.begin();

    if (config.provider() == Provider::BTCPAY && config.storeId().isEmpty())
        resolveBTCPayStore();

    ui.showLoading("Loading merchant");
    MerchantProfile profile = api->getProfile();
    if (profile.ok) {
        merchantName = profile.companyName;
        Serial.printf("[BOOT] Merchant: %s\n", merchantName.c_str());
    }

    ui.showSplash(merchantName);
    delay(1200);
    resetToIdle();
    Serial.println("[POS] Ready");
}

// Hold the BOOT key ~5s (any time the POS is running) to drop the WiFi/setup
// config and reboot into the captive portal. Polled at runtime because GPIO0
// is a strapping pin — held through a reset it selects the ROM bootloader, so
// a power-on check could never see it.
static void checkWifiResetButton() {
    static uint32_t heldSince = 0;
    static int shownSecs = -1;

    if (digitalRead(BOOT_BTN) == HIGH) {
        if (shownSecs != -1) resetToIdle();   // released early — restore UI
        heldSince = 0;
        shownSecs = -1;
        return;
    }

    if (heldSince == 0) { heldSince = millis(); return; }
    uint32_t held = millis() - heldSince;
    if (held < 700) return;                   // debounce/accidental taps

    int secsLeft = 5 - (int)((held - 700) / 1000);
    if (secsLeft > 0) {
        if (secsLeft != shownSecs) {
            shownSecs = secsLeft;
            ui.showMessage("WiFi reset", "keep holding: " + String(secsLeft) + "s",
                           COL_ACCENT);
        }
        return;
    }

    ui.showMessage("WiFi reset", "rebooting to setup", COL_ACCENT);
    config.markUnprovisioned();
    delay(1000);
    ESP.restart();
}

void loop() {
    portal.checkSerial(config);
    checkWifiResetButton();

    switch (state) {

    case State::IDLE: {
        Key k = ui.pollTouch();
        if (k != Key::NONE) handleKey(k);
        break;
    }

    case State::CREATING: {
        if (WiFi.status() != WL_CONNECTED) {
            state = State::ERROR; stateEnteredAt = millis();
            ui.showError("No WiFi"); break;
        }
        ui.showLoading("Creating invoice");
        activeInvoice = api->createInvoice(activeFiat, invoiceDetails());
        if (!activeInvoice.ok) {
            state = State::ERROR; stateEnteredAt = millis();
            ui.showError(activeInvoice.error.length() ? activeInvoice.error : "Invoice failed");
            break;
        }
        invoiceCreatedAt = millis();
        lastPollAt = 0;
        refreshCount = 0;
        boltcardSubmitted = false;
        lastShownSecs = -1;
        state = State::AWAITING;
        ui.showQR(activeInvoice.paymentRequest, activeInvoice.satAmount,
                  activeInvoice.nzdAmount > 0 ? activeInvoice.nzdAmount : activeFiat,
                  currencyLabel, INVOICE_EXPIRY_SEC);
        break;
    }

    case State::AWAITING: {
        unsigned long now = millis();
        unsigned long elapsed = now - invoiceCreatedAt;
        int secsLeft = INVOICE_EXPIRY_SEC - (int)(elapsed / 1000);

        // Auto-refresh near expiry for a fresh bolt11 / rate. Suspended once a
        // Boltcard has accepted this invoice — the wallet is paying this exact
        // bolt11, so we must not swap it out mid-withdrawal.
        if (!boltcardSubmitted &&
            elapsed >= (unsigned long)(INVOICE_EXPIRY_SEC * 1000 - INVOICE_REFRESH_BUFFER_MS)) {
            if (refreshCount >= MAX_INVOICE_REFRESHES) {
                state = State::ERROR; stateEnteredAt = millis();
                ui.showError("Payment timeout"); break;
            }
            MerchantInvoice r = api->refreshInvoice(activeInvoice.reference);
            if (!r.ok) r = api->createInvoice(activeFiat, invoiceDetails());
            if (r.ok) {
                activeInvoice = r;
                invoiceCreatedAt = millis();
                refreshCount++;
                lastPollAt = 0;
                lastShownSecs = -1;
                ui.showQR(activeInvoice.paymentRequest, activeInvoice.satAmount,
                          activeInvoice.nzdAmount > 0 ? activeInvoice.nzdAmount : activeFiat,
                          currencyLabel, INVOICE_EXPIRY_SEC);
            } else {
                state = State::ERROR; stateEnteredAt = millis();
                ui.showError("Cannot refresh");
            }
            break;
        }

        // Poll status
        if (now - lastPollAt >= PAYMENT_POLL_INTERVAL_MS) {
            lastPollAt = now;
            PaymentStatus ps = api->checkPayment(activeInvoice.reference);
            if (ps.ok && ps.isPaid) {
                float f = ps.nzdAmount > 0 ? ps.nzdAmount : activeFiat;
                uint64_t s = ps.satAmount > 0 ? ps.satAmount : activeInvoice.satAmount;
                state = State::PAID; stateEnteredAt = millis();
                ui.showPaid(s, f, currencyLabel);
                Serial.printf("[POS] PAID %.2f %s\n", f, currencyLabel.c_str());
                break;
            }
        }

        // Countdown (frozen once a Boltcard tap is being settled).
        if (!boltcardSubmitted && secsLeft != lastShownSecs) {
            ui.updateCountdown(secsLeft);
            lastShownSecs = secsLeft;
        }

        // NFC: Boltcard tap -> resolve its LNURLW and submit the active invoice.
        // On success the wallet settles this bolt11 and the poll above flips us
        // to PAID. Only submit once per invoice.
        if (!boltcardSubmitted) {
            String uid, url;
            if (nfc.readCard(uid, url) && url.length()) {
                Serial.printf("[NFC] tap uid=%s url=%s\n", uid.c_str(), url.c_str());
                ui.showLoading("Card detected");
                BoltcardResult br = boltcardPay(url, activeInvoice.paymentRequest);
                if (br.ok) {
                    boltcardSubmitted = true;
                    lastPollAt = 0;   // poll for settlement immediately
                    ui.showLoading("Awaiting payment");
                } else {
                    ui.showError(br.error.length() ? br.error : "Card declined");
                    delay(1800);
                    ui.showQR(activeInvoice.paymentRequest, activeInvoice.satAmount,
                              activeInvoice.nzdAmount > 0 ? activeInvoice.nzdAmount : activeFiat,
                              currencyLabel, INVOICE_EXPIRY_SEC);
                    lastShownSecs = -1;
                }
            }
        }

        if (ui.qrCloseTouched()) resetToIdle();   // cancel via the X button only
        break;
    }

    case State::PAID:
        if (ui.anyTouch()) resetToIdle();
        break;

    case State::ERROR:
        if (millis() - stateEnteredAt > 5000 || ui.anyTouch()) resetToIdle();
        break;
    }

    delay(15);
}
