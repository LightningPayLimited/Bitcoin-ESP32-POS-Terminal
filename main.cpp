// ============================================================
// Stacked POS — ESP32-P4 Lightning Bitcoin Point of Sale
// ============================================================
//
// Boot flow:
//   1. Check NVS for saved config (WiFi + API key)
//   2. If not provisioned → start captive portal for setup
//   3. If provisioned → connect WiFi → enter POS mode
//
// POS flow:
//   1. Numpad → merchant enters $NZD amount
//   2. PAY → POST /api/merchant/payment with NZD amount
//   3. Show QR (bolt11) → poll status every 1.5s
//   4. Invoice expires in 60s → auto-refresh via txRef
//   5. paidDate set → success screen → back to numpad
//
// Provisioning:
//   - First boot: captive portal at 192.168.4.1
//   - Or: send JSON over serial: {"ssid":"...","pass":"...","apiKey":"..."}
//   - Or: Stacked webpage pushes config via Web Serial API
//   - Send "RESET" over serial to factory reset
//
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "config_store.h"
#include "setup_portal.h"
#include "display_ui.h"
#include "stacked_api.h"
#include "printer.h"

// ================================================================
// State
// ================================================================
enum class State {
    BOOT,
    SETUP,             // Captive portal running
    WIFI_CONNECTING,
    IDLE,              // Numpad
    CREATING_INVOICE,
    AWAITING_PAYMENT,
    PAID,
    ERROR,
};

static State        state = State::BOOT;
static ConfigStore  config;
static SetupPortal  portal;
static DisplayUI    ui;
static StackedAPI   api;

static String merchantName = "";
static String enteredAmount = "";
static float  activeNzd = 0;

static MerchantInvoice activeInvoice;
static unsigned long   invoiceCreatedAt = 0;
static unsigned long   lastPollAt = 0;
static int             refreshCount = 0;
static unsigned long   stateEnteredAt = 0;

// ================================================================
// Helpers
// ================================================================
static char keyChar(Key k) {
    const char map[] = "0123456789";
    int idx = (int)k - (int)Key::D0;
    return (idx >= 0 && idx <= 9) ? map[idx] : 0;
}

void resetToIdle() {
    state = State::IDLE;
    enteredAmount = "";
    activeNzd = 0;
    refreshCount = 0;
    activeInvoice = {};
    ui.showAmountEntry(enteredAmount, "NZD");
}

void handleKey(Key k) {
    char ch = keyChar(k);
    if (ch) {
        int dot = enteredAmount.indexOf('.');
        if (dot >= 0 && (int)enteredAmount.length() - dot > 2) return;
        if (enteredAmount.length() >= 10) return;
        enteredAmount += ch;
    } else if (k == Key::DOT) {
        if (enteredAmount.indexOf('.') < 0) {
            if (enteredAmount.isEmpty()) enteredAmount = "0";
            enteredAmount += ".";
        }
    } else if (k == Key::DEL) {
        if (enteredAmount.length() > 0)
            enteredAmount.remove(enteredAmount.length() - 1);
    } else if (k == Key::CLEAR) {
        enteredAmount = "";
    } else if (k == Key::CHARGE) {
        float nzd = enteredAmount.toFloat();
        // Compare in integer cents to avoid float-precision rejection of 0.01
        int cents = (int)(nzd * 100.0f + 0.5f);
        if (cents < 1) return;  // Min 1 cent
        activeNzd = cents / 100.0f;
        state = State::CREATING_INVOICE;
        return;
    } else {
        return;
    }
    ui.showAmountEntry(enteredAmount, "NZD");
}

// ================================================================
// Setup
// ================================================================
void setup() {
    Serial.begin(115200);
    delay(2000);  // give USB-CDC host time to (re)connect
    Serial.println("\n=============================");
    Serial.println("  Stacked POS — ESP32-P4");
    Serial.println("=============================");
    Serial.flush();

    // Factory-reset button — active-low with internal pull-up
    pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

    // Thermal printer — initialises whether or not one is plugged in
    printer.begin();

    Serial.println("[BOOT] Calling ui.begin()...");
    Serial.flush();
    ui.begin();
    Serial.println("[BOOT] ui.begin() returned OK");
    Serial.flush();

    ui.showSplash();
    delay(1000);

    // Load config from NVS
    bool provisioned = config.begin();

    if (!provisioned) {
        // --- SETUP MODE ---
        Serial.println("[BOOT] Not provisioned — entering setup mode");
        state = State::SETUP;
        ui.showSetupInfo();

        // This blocks until config is saved, then reboots
        portal.runCaptivePortal(config);
        return;  // Won't reach here — device reboots
    }

    // --- POS MODE ---
    Serial.println("[BOOT] Config loaded — connecting WiFi...");
    Serial.printf("[WIFI] SSID='%s' pass.len=%d\n",
                  config.ssid().c_str(), config.pass().length());
    Serial.printf("[WIFI] MAC=%s\n", WiFi.macAddress().c_str());

    // Paint live WiFi status directly on the display so we can see it even
    // when USB Serial is dropping our output.
    ui.gfx()->fillScreen(COL_BG);
    ui.gfx()->setTextSize(2);
    ui.gfx()->setTextColor(COL_ACCENT, COL_BG);
    ui.gfx()->setCursor(10, 20);
    ui.gfx()->printf("WiFi: %s", config.ssid().c_str());
    ui.gfx()->setCursor(10, 50);
    ui.gfx()->printf("MAC:  %s", WiFi.macAddress().c_str());
    ui.gfx()->setCursor(10, 80);
    ui.gfx()->setTextColor(COL_FG, COL_BG);
    ui.gfx()->print("Connecting...");

    static uint16_t lastReason = 0;
    static bool     gotStart = false, gotAssoc = false, gotIP = false;

    // Install verbose event logging — paint each event on the display too.
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        extern DisplayUI ui;
        auto* gfx = ui.gfx();
        gfx->setTextSize(2);
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_START:
                gotStart = true;
                gfx->setTextColor(0x07E0, COL_BG);  // green
                gfx->setCursor(10, 120);
                gfx->print("STA started      ");
                Serial.println("[WIFI EV] STA started");
                break;
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                gotAssoc = true;
                gfx->setTextColor(0x07E0, COL_BG);
                gfx->setCursor(10, 150);
                gfx->printf("Associated ch=%u ",
                            info.wifi_sta_connected.channel);
                Serial.printf("[WIFI EV] Associated ch=%u auth=%u\n",
                              info.wifi_sta_connected.channel,
                              info.wifi_sta_connected.authmode);
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                gotIP = true;
                gfx->setTextColor(0x07E0, COL_BG);
                gfx->setCursor(10, 180);
                gfx->printf("IP: %s        ",
                            IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
                Serial.printf("[WIFI EV] Got IP: %s\n",
                              IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
                lastReason = info.wifi_sta_disconnected.reason;
                const char* r;
                switch (lastReason) {
                    case WIFI_REASON_AUTH_EXPIRE:       r = "AUTH_EXPIRE"; break;
                    case WIFI_REASON_AUTH_FAIL:         r = "AUTH_FAIL (bad pass)"; break;
                    case WIFI_REASON_NO_AP_FOUND:       r = "NO_AP (not visible)"; break;
                    case WIFI_REASON_ASSOC_FAIL:        r = "ASSOC_FAIL"; break;
                    case WIFI_REASON_HANDSHAKE_TIMEOUT: r = "HANDSHAKE_TO"; break;
                    case WIFI_REASON_BEACON_TIMEOUT:    r = "BEACON_TO (weak)"; break;
                    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: r = "4WAY_TO"; break;
                    case 8:                             r = "ASSOC_LEAVE"; break;
                    default:                            r = "other"; break;
                }
                gfx->setTextColor(COL_ERROR, COL_BG);
                gfx->setCursor(10, 210);
                gfx->printf("Disc %u %-16s", lastReason, r);
                Serial.printf("[WIFI EV] Disconnect reason=%u (%s)\n", lastReason, r);
                break;
            }
            default: break;
        }
    });

    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid().c_str(), config.pass().c_str());

    int tries = 0;
    wl_status_t last = WiFi.status();
    while (last != WL_CONNECTED && tries < 40) {
        delay(500);
        wl_status_t now = WiFi.status();
        if (now != last) {
            const char* s;
            switch (now) {
                case WL_IDLE_STATUS:     s = "IDLE"; break;
                case WL_NO_SSID_AVAIL:   s = "NO_SSID_AVAIL"; break;
                case WL_SCAN_COMPLETED:  s = "SCAN_COMPLETED"; break;
                case WL_CONNECTED:       s = "CONNECTED"; break;
                case WL_CONNECT_FAILED:  s = "CONNECT_FAILED"; break;
                case WL_CONNECTION_LOST: s = "CONNECTION_LOST"; break;
                case WL_DISCONNECTED:    s = "DISCONNECTED"; break;
                default:                 s = "?"; break;
            }
            Serial.printf("\n[WIFI] status -> %d (%s)\n", now, s);
            last = now;
        } else {
            Serial.print(".");
        }
        portal.checkSerial(config);
        tries++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WIFI] Final status: %d, RSSI: %d dBm\n",
                      WiFi.status(), WiFi.RSSI());

        // Scan and log what's actually visible
        Serial.println("[WIFI] Scanning for diagnosis...");
        int n = WiFi.scanNetworks(false, true);
        Serial.printf("[WIFI] Visible networks: %d\n", n);
        for (int i = 0; i < n; i++) {
            Serial.printf("  %2d) %-32s rssi=%d ch=%d auth=%d%s\n",
                          i, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                          WiFi.channel(i), (int)WiFi.encryptionType(i),
                          WiFi.SSID(i) == config.ssid() ? "  <-- target" : "");
        }
        WiFi.scanDelete();

        Serial.println("[BOOT] WiFi failed — dropping back into setup mode");
        ui.showError("WiFi failed. Reconfiguring...");
        delay(2500);

        // Keep saved creds around so the user can see what was wrong and just
        // fix the password, but flip the provisioned flag so the portal stays
        // up until the user saves again.
        config.markUnprovisioned();

        ui.showSetupInfo();
        portal.runCaptivePortal(config);
        return;  // runCaptivePortal reboots on save
    }

    Serial.printf("[BOOT] WiFi OK: %s\n", WiFi.localIP().toString().c_str());

    // Init API
    api.begin(STACKED_API_BASE, config.apiKey());

    // Fetch merchant profile
    ui.showLoading("Loading merchant...");
    MerchantProfile profile = api.getProfile();
    if (profile.ok) {
        merchantName = profile.companyName;
        Serial.printf("[BOOT] Merchant: %s\n", merchantName.c_str());
    }

    // Brief splash with merchant name
    ui.showSplash(merchantName);
    delay(1500);

    // Ready
    resetToIdle();
    Serial.println("[POS] Ready — enter NZD amount");
}

// ================================================================
// Loop
// ================================================================
// Factory-reset via long-hold of FACTORY_RESET_PIN.
// Shows a countdown on screen while the button is held.
static void checkFactoryResetButton() {
    static unsigned long pressStart = 0;
    static int lastSecsShown = -1;

    bool pressed = (digitalRead(FACTORY_RESET_PIN) == LOW);
    if (!pressed) {
        if (pressStart != 0 && lastSecsShown >= 0) {
            // Released before completion — redraw current screen state
            ui.showAmountEntry("", "NZD");
        }
        pressStart = 0;
        lastSecsShown = -1;
        return;
    }

    if (pressStart == 0) pressStart = millis();
    unsigned long held = millis() - pressStart;

    if (held >= FACTORY_RESET_HOLD_MS) {
        Serial.println("[SYS] Factory reset triggered — clearing NVS");
        ui.gfx()->fillScreen(COL_BG);
        ui.gfx()->setTextSize(3);
        ui.gfx()->setTextColor(COL_ERROR, COL_BG);
        ui.gfx()->setCursor(30, SCREEN_HEIGHT / 2 - 20);
        ui.gfx()->print("FACTORY RESET");
        ui.gfx()->setCursor(30, SCREEN_HEIGHT / 2 + 20);
        ui.gfx()->setTextSize(2);
        ui.gfx()->print("Rebooting...");
        delay(500);
        config.clear();
        delay(500);
        ESP.restart();
    }

    int secsLeft = (FACTORY_RESET_HOLD_MS - (int)held + 999) / 1000;
    if (secsLeft != lastSecsShown) {
        lastSecsShown = secsLeft;
        ui.gfx()->fillScreen(COL_BG);
        ui.gfx()->setTextColor(COL_ACCENT, COL_BG);
        ui.gfx()->setTextSize(3);
        ui.gfx()->setCursor(30, 100);
        ui.gfx()->print("Factory reset in");
        ui.gfx()->setTextSize(10);
        ui.gfx()->setTextColor(COL_ERROR, COL_BG);
        ui.gfx()->setCursor(SCREEN_WIDTH/2 - 40, SCREEN_HEIGHT/2);
        ui.gfx()->printf("%d", secsLeft);
        ui.gfx()->setTextColor(COL_DIM, COL_BG);
        ui.gfx()->setTextSize(2);
        ui.gfx()->setCursor(30, SCREEN_HEIGHT - 80);
        ui.gfx()->print("Release to cancel");
    }
}

void loop() {
    // Always check for serial commands (RESET, config JSON)
    portal.checkSerial(config);
    checkFactoryResetButton();

    switch (state) {

    // --- Numpad ---
    case State::IDLE: {
        Key k = ui.pollTouch();
        if (k != Key::NONE) handleKey(k);
        break;
    }

    // --- Create invoice ---
    case State::CREATING_INVOICE: {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[POS] PAY pressed but WiFi not connected — aborting");
            state = State::ERROR;
            stateEnteredAt = millis();
            ui.showError("No WiFi connection");
            break;
        }
        ui.showLoading("Creating invoice...");

        String details = merchantName.length() > 0
            ? merchantName + " $" + String(activeNzd, 2) + " NZD"
            : "$" + String(activeNzd, 2) + " NZD";

        Serial.printf("[POS] Creating invoice $%.2f NZD\n", activeNzd);
        activeInvoice = api.createInvoice(activeNzd, details);

        if (!activeInvoice.ok) {
            state = State::ERROR;
            stateEnteredAt = millis();
            ui.showError(activeInvoice.error);
            break;
        }

        invoiceCreatedAt = millis();
        lastPollAt = 0;
        refreshCount = 0;
        state = State::AWAITING_PAYMENT;

        ui.showQR(activeInvoice.paymentRequest,
                  activeInvoice.satAmount,
                  activeInvoice.nzdAmount > 0 ? activeInvoice.nzdAmount : activeNzd,
                  INVOICE_EXPIRY_SEC,
                  refreshCount);

        Serial.printf("[POS] Invoice: $%.2f NZD = %lu sats\n",
                      activeInvoice.nzdAmount, (unsigned long)activeInvoice.satAmount);
        break;
    }

    // --- Awaiting payment: poll + auto-refresh ---
    case State::AWAITING_PAYMENT: {
        unsigned long now = millis();
        unsigned long elapsed = now - invoiceCreatedAt;
        int secsLeft = INVOICE_EXPIRY_SEC - (int)(elapsed / 1000);

        // Auto-refresh before expiry
        if (elapsed >= (unsigned long)(INVOICE_EXPIRY_SEC * 1000 - INVOICE_REFRESH_BUFFER_MS)) {
            if (refreshCount >= MAX_INVOICE_REFRESHES) {
                state = State::ERROR;
                stateEnteredAt = millis();
                ui.showError("Payment timeout");
                break;
            }

            Serial.printf("[POS] Refreshing invoice (%d)...\n", refreshCount + 1);
            MerchantInvoice refreshed = api.refreshInvoice(activeInvoice.reference);

            // If refresh fails (e.g. old invoice expired server-side), fall
            // back to creating a brand-new invoice at the same NZD amount
            // so the customer isn't stranded with a dead QR.
            if (!refreshed.ok) {
                Serial.printf("[POS] Refresh failed (%s) — creating new invoice\n",
                              refreshed.error.c_str());
                String details = merchantName.length() > 0
                    ? merchantName + " $" + String(activeNzd, 2) + " NZD"
                    : "$" + String(activeNzd, 2) + " NZD";
                refreshed = api.createInvoice(activeNzd, details);
            }

            if (refreshed.ok) {
                activeInvoice = refreshed;
                invoiceCreatedAt = millis();
                refreshCount++;
                lastPollAt = 0;

                ui.showQR(activeInvoice.paymentRequest,
                          activeInvoice.satAmount,
                          activeInvoice.nzdAmount > 0 ? activeInvoice.nzdAmount : activeNzd,
                          INVOICE_EXPIRY_SEC,
                          refreshCount);
            } else {
                Serial.printf("[POS] Invoice creation failed too: %s\n",
                              refreshed.error.c_str());
                state = State::ERROR;
                stateEnteredAt = millis();
                ui.showError("Cannot create invoice");
            }
            break;
        }

        // Poll payment status
        if (now - lastPollAt >= PAYMENT_POLL_INTERVAL_MS) {
            lastPollAt = now;
            Serial.printf("[POS] Polling (elapsed=%lus, ref='%s')\n",
                          elapsed / 1000, activeInvoice.reference.c_str());
            PaymentStatus ps = api.checkPayment(activeInvoice.reference);

            if (ps.ok && ps.isPaid) {
                state = State::PAID;
                stateEnteredAt = millis();
                float nzd = ps.nzdAmount > 0 ? ps.nzdAmount : activeNzd;
                uint64_t sats = ps.satAmount > 0 ? ps.satAmount : activeInvoice.satAmount;
                ui.showPaid(sats, nzd);
                Serial.printf("[POS] PAID! $%.2f NZD (%lu sats)\n",
                              nzd, (unsigned long)sats);
                printer.printReceipt(merchantName, "", nzd, sats,
                                     activeInvoice.reference, ps.paidDate);
                break;
            }
        }

        // Update countdown
        if (secsLeft >= 0) ui.updateTimer(secsLeft, refreshCount);

        // Cancel on touch
        if (ui.anyTouch()) {
            Serial.println("[POS] Cancelled");
            resetToIdle();
        }
        break;
    }

    // --- Paid — stay until the merchant taps to dismiss ---
    case State::PAID: {
        if (ui.anyTouch()) resetToIdle();
        break;
    }

    // --- Error ---
    case State::ERROR: {
        if (millis() - stateEnteredAt > ERROR_DISPLAY_MS || ui.anyTouch())
            resetToIdle();
        break;
    }

    default: break;
    }

    delay(50);
}
